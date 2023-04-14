/*
 * Copyright 2014-2023 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 *
 * @brief Joulescope driver log handler
 */

#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/mutex.h"
#include "tinyprintf.h"
#include <stdio.h>

#if _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

#define LOG_DPRINTF       (0)
#define MSG_COUNT_INIT    (1024U)
#define MSG_PEND_COUNT_MAX (1024U)
#define LOCK_MSG()        jsdrv_os_mutex_lock(log_instance_.msg_mutex)
#define UNLOCK_MSG()      jsdrv_os_mutex_unlock(log_instance_.msg_mutex)
#define LOCK_DISPATCH()   jsdrv_os_mutex_lock(log_instance_.dispatch_mutex)
#define UNLOCK_DISPATCH() jsdrv_os_mutex_unlock(log_instance_.dispatch_mutex)

#ifdef _MSC_VER
#define dprintf(fmt, ...) do { if (LOG_DPRINTF) {printf(fmt "\n", __VA_ARGS__); }} while (0)
#else
/* GCC compiler support */
// zero length variadic arguments are not allowed for macros
// this hack ensures that dprintf(message) and dprintf(format, args...) are both supported.
// https://stackoverflow.com/questions/5588855/standard-alternative-to-gccs-va-args-trick
#define _DPRINTF_INNER(fmt, ...) do { if (LOG_DPRINTF) {printf(fmt "\n", __VA_ARGS__); }} while (0)
#define _DPRINTF_SELECT(_11, _10, _9, _8, _7, _6, _5, _4, _3, _2, _1, SUFFIX, ...) _DPRINTF_ ## SUFFIX
#define _DPRINTF_1(message) _DPRINTF_INNER("%s", message)
#define _DPRINTF_N(fmt, ...) _DPRINTF_INNER(fmt, __VA_ARGS__)
#define dprintf(...)  _DPRINTF_SELECT(__VA_ARGS__, N, N, N, N, N, N, N, N, N, N, 1, 0)(__VA_ARGS__)
#endif


char const * const jsdrv_log_level_str[JSDRV_LOG_LEVEL_ALL + 1] = {
        "EMERGENCY",
        "ALERT",
        "CRITICAL",
        "ERROR",
        "WARN",
        "NOTICE"
        "INFO",
        "DEBUG",
        "DEBUG2"
        "DEBUG3",
        "ALL"
};

char const jsdrv_log_level_char[JSDRV_LOG_LEVEL_ALL + 1] = {
        '!', 'A', 'C', 'E', 'W', 'N', 'I', 'D', 'D', 'D', '.'
};

// platform-specific functions
static void thread_notify();
static int32_t thread_start();
static void thread_stop();


struct msg_s {
    struct jsdrv_list_s item;  // must be first
    struct jsdrv_log_header_s header;
    char filename[JSDRV_LOG_FILENAME_SIZE_MAX];
    char message[JSDRV_LOG_MESSAGE_SIZE_MAX];
};

struct dispatch_s {
    struct jsdrv_list_s item;  // must be first
    jsdrv_log_recv fn;
    void * user_data;
};

struct log_s {
    volatile uint32_t initialized;
    volatile uint32_t active_count;
    volatile int8_t quit;
    volatile int8_t level;
    volatile uint8_t dropping;
    volatile uint32_t msg_pend_count;

    struct jsdrv_list_s dispatch_list;
    struct jsdrv_list_s msg_free;
    struct jsdrv_list_s msg_pend;
    jsdrv_os_mutex_t dispatch_mutex;
    jsdrv_os_mutex_t msg_mutex;

#if _WIN32
    // Windows
    HANDLE event;
    HANDLE thread;
    DWORD thread_id;
#else
    pthread_t thread_id;
    int fd_read;
    int fd_write;
#endif
};

static struct log_s log_instance_ = {
        .initialized=0,
        .active_count=0,
        .quit=0,
        .level=JSDRV_LOG_LEVEL_OFF,
        .dropping=0,
        .msg_pend_count=0,
        .dispatch_list={NULL, NULL},
        .msg_free={NULL, NULL},
        .msg_pend={NULL, NULL},
#if _WIN32
        .event=NULL,
        .thread=NULL,
        .thread_id=0,
#else
        .thread_id=0,
        .fd_read=-1,
        .fd_write=-1,
#endif
};

static struct msg_s * msg_alloc() {
    struct jsdrv_list_s * item;
    struct msg_s * msg;
    if (NULL == log_instance_.msg_free.next) {
        jsdrv_list_initialize(&log_instance_.msg_free);
    }
    if (NULL == log_instance_.msg_pend.next) {
        jsdrv_list_initialize(&log_instance_.msg_pend);
    }
    item = jsdrv_list_remove_head(&log_instance_.msg_free);
    if (NULL == item) {
        msg = jsdrv_alloc(sizeof(struct msg_s));
        jsdrv_list_initialize(&msg->item);
    } else {
        msg = JSDRV_CONTAINER_OF(item, struct msg_s, item);
    }
    return msg;
}

void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...) {
    va_list args;
    if (0 == log_instance_.active_count) {
        dprintf("jsdrv_log_publish but not active");
        return;
    } else if (log_instance_.quit) {
        dprintf("jsdrv_log_publish but quit");
        return;
    } else if (level > log_instance_.level) {
        // dprintf("jsdrv_log_publish but ignore");
        return;
    } else if (log_instance_.dropping != 0) {
        // dprintf("jsdrv_log_publish dropping");
        return;
    } else {
        // dprintf("jsdrv_log_publish");
    }

    va_start(args, format);
    LOCK_MSG();
    if (0 == log_instance_.dropping) {
        struct msg_s *msg = msg_alloc();
        if (NULL == msg) {
            // do nothing
        } else if (log_instance_.msg_pend_count >= MSG_PEND_COUNT_MAX) {
            log_instance_.dropping = 1;
            ++log_instance_.msg_pend_count;
            msg->header.version = JSDRV_LOG_VERSION;
            msg->header.level = JSDRV_LOG_LEVEL_ERROR;
            msg->header.line = __LINE__;
            msg->header.rsvu8_1 = 0;
            msg->header.rsvu8_2 = 0;
            msg->header.timestamp = jsdrv_time_utc();
            jsdrv_cstr_copy(msg->filename, __FILENAME__, sizeof(msg->filename));
            jsdrv_cstr_copy(msg->message, "log drop due to overflow\n   ... missing messages ...", sizeof(msg->message));
            jsdrv_list_add_tail(&log_instance_.msg_pend, &msg->item);
            thread_notify(&log_instance_);
        } else {
            ++log_instance_.msg_pend_count;
            msg->header.version = JSDRV_LOG_VERSION;
            msg->header.level = level;
            msg->header.line = line;
            msg->header.rsvu8_1 = 0;
            msg->header.rsvu8_2 = 0;
            msg->header.timestamp = jsdrv_time_utc();
            jsdrv_cstr_copy(msg->filename, filename, sizeof(msg->filename));
            vsnprintf(msg->message, sizeof(msg->message), format, args);
            jsdrv_list_add_tail(&log_instance_.msg_pend, &msg->item);
            thread_notify(&log_instance_);
        }
    }
    UNLOCK_MSG();
    va_end(args);
}

int32_t jsdrv_log_register(jsdrv_log_recv fn, void * user_data) {
    dprintf("jsdrv_log_register %p", fn);
    struct dispatch_s * dispatch = jsdrv_alloc(sizeof(struct dispatch_s));
    jsdrv_list_initialize(&dispatch->item);
    dispatch->fn = fn;
    dispatch->user_data = user_data;
    LOCK_DISPATCH();
    if (NULL == log_instance_.dispatch_list.next) {
        jsdrv_list_initialize(&log_instance_.dispatch_list);
    }
    jsdrv_list_add_tail(&log_instance_.dispatch_list, &dispatch->item);
    dprintf("jsdrv_log_register %zu", jsdrv_list_length(&log_instance_.dispatch_list));
    UNLOCK_DISPATCH();
    return 0;
}

int32_t jsdrv_log_unregister(jsdrv_log_recv fn, void * user_data) {
    if (0 == log_instance_.active_count) {
        return JSDRV_ERROR_UNAVAILABLE;
    }
    struct jsdrv_list_s * item;
    struct dispatch_s * dispatch;
    dprintf("jsdrv_log_unregister %p", fn);
    LOCK_DISPATCH();
    jsdrv_list_foreach(&log_instance_.dispatch_list, item) {
        dispatch = JSDRV_CONTAINER_OF(item, struct dispatch_s, item);
        if ((dispatch->fn == fn) && (dispatch->user_data == user_data)) {
            jsdrv_list_remove(item);
            jsdrv_free(dispatch);
        }
    }
    UNLOCK_DISPATCH();
    return 0;
}

static void process(struct log_s * self) {
    struct jsdrv_list_s * item = NULL;
    struct msg_s * msg = NULL;
    struct dispatch_s * dispatch = NULL;

    while (1) {
        LOCK_MSG();
        if (NULL != msg) {
            jsdrv_list_add_tail(&self->msg_free, &msg->item);
        }
        item = jsdrv_list_remove_head(&self->msg_pend);
        UNLOCK_MSG();
        if (!item) {
            break;
        }
        if (log_instance_.msg_pend_count > 0) {
            --log_instance_.msg_pend_count;
        }
        if (log_instance_.msg_pend_count == 0) {
            log_instance_.dropping = 0;
        }
        msg = JSDRV_CONTAINER_OF(item, struct msg_s, item);
        if (msg->header.level > self->level) {
            continue;
        }
        LOCK_DISPATCH();
        dprintf("dispatch list start %p %zu %p", &self->dispatch_list, jsdrv_list_length(&self->dispatch_list), self->dispatch_list.next);
        jsdrv_list_foreach(&self->dispatch_list, item) {
            dispatch = JSDRV_CONTAINER_OF(item, struct dispatch_s, item);
            dprintf("dispatch %p", dispatch->fn);
            dispatch->fn(dispatch->user_data, &msg->header, msg->filename, msg->message);
        }
        dprintf("dispatch list done %p %zu %p", &self->dispatch_list, jsdrv_list_length(&self->dispatch_list), self->dispatch_list.next);
        UNLOCK_DISPATCH();
    }
}

static void list_free(struct jsdrv_list_s * list) {
    struct jsdrv_list_s * item;
    while (1) {
        item = jsdrv_list_remove_head(list);
        if (!item) {
            break;
        }
        jsdrv_free(item);
    }
}

#if _WIN32
static DWORD WINAPI log_thread(LPVOID lpParam) {
    (void) lpParam;
    while (!log_instance_.quit) {
        WaitForSingleObject(log_instance_.event, 100);
        ResetEvent(log_instance_.event);
        process(&log_instance_);
    }
    process(&log_instance_);
    dprintf("log_thread exit");
    return 0;
}

static void thread_notify() {
    if (NULL != log_instance_.event) {
        SetEvent(log_instance_.event);
    }
}

static int32_t thread_start() {
    log_instance_.event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );
    if (log_instance_.event == NULL) {
        printf("log CreateEvent failed\n");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    log_instance_.thread = CreateThread(
            NULL,               // default security attributes
            0,                  // use default stack size
            log_thread,         // thread function name
            &log_instance_,         // argument to thread function
            0,                  // use default creation flags
            &log_instance_.thread_id);  // returns the thread identifier
    if (log_instance_.thread == NULL) {
        printf("log CreateThread failed\n");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

static void thread_stop() {
    log_instance_.quit = 1;
    SetEvent(log_instance_.event);
    if (WAIT_OBJECT_0 != WaitForSingleObject(log_instance_.thread, 10000)) {
        printf("ERROR: log thread_stop not closed cleanly\n");
    } else {
        dprintf("log thread stopped");
    }
    CloseHandle(log_instance_.thread);
    log_instance_.thread = NULL;
    CloseHandle(log_instance_.event);
    log_instance_.event = NULL;
}

#else
static void * log_thread(void * arg) {
    (void) arg;
    uint8_t rd_buf[1024];
    struct pollfd fds;
    fds.fd = log_instance_.fd_read;
    fds.events = POLLIN;
    while (!log_instance_.quit) {
        fds.revents = 0;
        poll(&fds, 1, 100);
        ssize_t rv = read(log_instance_.fd_read, rd_buf, sizeof(rd_buf));
        if ((rv <= 0) && (errno != EAGAIN)) {
            printf("log_thread READ error %d, %d\n", (int) rv, errno);
            break;
        }
        process(&log_instance_);
    }
    process(&log_instance_);
    return 0;
}

static void thread_notify() {
    uint8_t wr_buf[] = {1};
    if (write(log_instance_.fd_write, wr_buf, 1) <= 0) {
        printf("ERROR: thread_notify write failed, errno=%d\n", errno);
    }
}

static int32_t thread_start() {
    int pipefd[2];
    if (pipe(pipefd)) {
        return JSDRV_ERROR_IO;
    }
    log_instance_.fd_read = pipefd[0];
    log_instance_.fd_write = pipefd[1];
    fcntl(log_instance_.fd_read, F_SETFL, O_NONBLOCK);
    int rc = pthread_create(&log_instance_.thread_id, NULL, log_thread, &log_instance_);
    if (rc) {
        JSDRV_LOGE("pthread_create failed %d", rc);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

static void thread_stop() {
    uint8_t wr_buf[] = {1};
    log_instance_.quit = 1;
    if (write(log_instance_.fd_write, wr_buf, 1) <= 0) {
        printf("ERROR: thread_stop write failed, errno=%d\n", errno);
    }
    int rc = pthread_join(log_instance_.thread_id, NULL);
    if (rc) {
        printf("ERROR: log thread_stop not closed cleanly: %d\n", rc);
    }
    close(log_instance_.fd_write);
    close(log_instance_.fd_read);
}
#endif

void jsdrv_log_initialize() {
    dprintf("jsdrv_log_initialize");

    if (0 == log_instance_.initialized) {
        log_instance_.msg_mutex = jsdrv_os_mutex_alloc("jsdrv_log_msg");
        LOCK_MSG();
        log_instance_.dispatch_mutex = jsdrv_os_mutex_alloc("jsdrv_log_dispatch");
        LOCK_DISPATCH();
        log_instance_.initialized = 1;
        UNLOCK_DISPATCH();
        UNLOCK_MSG();
    }

    LOCK_DISPATCH();
    if (0 == log_instance_.active_count) {
        log_instance_.dropping = 0;
        log_instance_.msg_pend_count = 0;
        log_instance_.active_count = 1;
        log_instance_.quit = 0;
        if (NULL == log_instance_.dispatch_list.next) {
            jsdrv_list_initialize(&log_instance_.dispatch_list);
        }
        LOCK_MSG();

        if (NULL == log_instance_.msg_free.next) {
            jsdrv_list_initialize(&log_instance_.msg_free);
        } else {
            list_free(&log_instance_.msg_free);
        }

        if (NULL == log_instance_.msg_pend.next) {
            jsdrv_list_initialize(&log_instance_.msg_pend);
        } else {
            list_free(&log_instance_.msg_pend);
        }

        for (size_t i = 0; i < MSG_COUNT_INIT; ++i) {
            struct msg_s *msg = jsdrv_alloc(sizeof(struct msg_s));
            jsdrv_list_initialize(&msg->item);
            jsdrv_list_add_tail(&log_instance_.msg_free, &msg->item);
        }

        thread_start();
        UNLOCK_MSG();
    } else {
        ++log_instance_.active_count;
    }
    UNLOCK_DISPATCH();
}

void jsdrv_log_finalize() {
    dprintf("jsdrv_log_finalize");
    if (0 == log_instance_.active_count) {
        dprintf("ERROR: jsdrv_log_finalize but 0 == active_count");
        return;
    }
    int do_finalize = 0;

    LOCK_DISPATCH();
    if (log_instance_.active_count) {
        --log_instance_.active_count;
        if (log_instance_.active_count == 0) {
            log_instance_.quit = 1;
            do_finalize = 1;
        }
    }
    UNLOCK_DISPATCH();

    dprintf("jsdrv_log_finalize do_finalize=%d, dispatch_len=%zu",
            do_finalize, jsdrv_list_length(&log_instance_.dispatch_list));
    if (do_finalize) {
        thread_stop(&log_instance_);
        LOCK_DISPATCH();
        list_free(&log_instance_.dispatch_list);
        LOCK_MSG();
        list_free(&log_instance_.msg_pend);
        list_free(&log_instance_.msg_free);
        UNLOCK_MSG();
        UNLOCK_DISPATCH();
        // do not free the mutexes, for thread safety on exit
        // jsdrv_os_mutex_free(log_instance_.msg_mutex);
        // jsdrv_os_mutex_free(log_instance_.dispatch_mutex);
    }
}

void jsdrv_log_level_set(int8_t level) {
    log_instance_.level = level;
}

int8_t jsdrv_log_level_get() {
    return log_instance_.level;
}

JSDRV_API const char * jsdrv_log_level_to_str(int8_t level) {
    if (level < 0) {
        return "OFF";
    }
    if (level >= JSDRV_LOG_LEVEL_ALL) {
        return jsdrv_log_level_str[JSDRV_LOG_LEVEL_ALL];
    }
    return jsdrv_log_level_str[level];
}

JSDRV_API char jsdrv_log_level_to_char(int8_t level) {
    if (level < 0) {
        return '*';
    }
    if (level >= JSDRV_LOG_LEVEL_ALL) {
        return jsdrv_log_level_char[JSDRV_LOG_LEVEL_ALL];
    }
    return jsdrv_log_level_char[level];
}
