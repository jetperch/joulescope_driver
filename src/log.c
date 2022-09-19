/*
 * Copyright 2014-2022 Jetperch LLC
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
#endif

#define LOG_DPRINTF 0
#define MSG_COUNT_INIT 1024U
#define LOCK_MSG() jsdrv_os_mutex_lock(self_.msg_mutex)
#define UNLOCK_MSG() jsdrv_os_mutex_unlock(self_.msg_mutex)
#define LOCK_DISPATCH() jsdrv_os_mutex_lock(self_.dispatch_mutex)
#define UNLOCK_DISPATCH() jsdrv_os_mutex_unlock(self_.dispatch_mutex)

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


struct log_s;  // forward declaration

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
static void thread_notify(struct log_s * self);
static int32_t thread_start(struct log_s * self);
static void thread_stop(struct log_s * self);


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
    volatile uint32_t active_count;
    struct jsdrv_list_s dispatch;
    jsdrv_os_mutex_t dispatch_mutex;
    struct jsdrv_list_s msg_free;
    struct jsdrv_list_s msg_pend;
    jsdrv_os_mutex_t msg_mutex;
    volatile int8_t quit;
    volatile int8_t level;

#if _WIN32
    // Windows
    HANDLE event;
    HANDLE thread;
    DWORD thread_id;
#else
    pthread_t thread_id;
    int fd_read;
    int fd_write;
    // todo
#endif
};

static volatile struct log_s self_ = {0};

static struct msg_s * msg_alloc(struct log_s * self) {
    struct jsdrv_list_s * item;
    struct msg_s * msg;
    if (!self) {
        return NULL;
    }
    item = jsdrv_list_remove_head(&self->msg_free);
    if (!item) {
        msg = jsdrv_alloc(sizeof(struct msg_s));
        jsdrv_list_initialize(&msg->item);
    } else {
        msg = JSDRV_CONTAINER_OF(item, struct msg_s, item);
    }
    return msg;
}

void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...) {
    va_list args;
    struct log_s * self = (struct log_s *) &self_;
    if (0 == self->active_count) {
        dprintf("jsdrv_log_publish but not active");
        return;
    }
    if (level > self->level) {
        return;
    }

    va_start(args, format);
    LOCK_MSG();
    struct msg_s * msg = msg_alloc(self);
    msg->header.version = JSDRV_LOG_VERSION;
    msg->header.level = level;
    msg->header.line = line;
    msg->header.timestamp = jsdrv_time_utc();
    jsdrv_cstr_copy(msg->filename, filename, sizeof(msg->filename));
    va_start(args, format);
    //tfp_vsnprintf(msg->message, sizeof(msg->message), format, args);
    vsnprintf(msg->message, sizeof(msg->message), format, args);
    jsdrv_list_add_tail(&self->msg_pend, &msg->item);
    UNLOCK_MSG();
    thread_notify(self);
    va_end(args);
}

int32_t jsdrv_log_register(jsdrv_log_recv fn, void * user_data) {
    struct log_s * self = (struct log_s *) &self_;
    if (!self) {
        return JSDRV_ERROR_UNAVAILABLE;
    }
    struct dispatch_s * dispatch = jsdrv_alloc(sizeof(struct dispatch_s));
    jsdrv_list_initialize(&dispatch->item);
    dispatch->fn = fn;
    dispatch->user_data = user_data;
    LOCK_DISPATCH();
    jsdrv_list_add_tail(&self->dispatch, &dispatch->item);
    UNLOCK_DISPATCH();
    return 0;
}

int32_t jsdrv_log_unregister(jsdrv_log_recv fn, void * user_data) {
    struct log_s * self = (struct log_s *) &self_;
    if (!self) {
        return JSDRV_ERROR_UNAVAILABLE;
    }
    struct jsdrv_list_s * item;
    struct dispatch_s * dispatch;
    LOCK_DISPATCH();
    jsdrv_list_foreach(&self->dispatch, item) {
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
    struct jsdrv_list_s * item;
    struct msg_s * msg;
    struct dispatch_s * dispatch;

    LOCK_DISPATCH();
    while (1) {
        LOCK_MSG();
        item = jsdrv_list_remove_head(&self->msg_pend);
        UNLOCK_MSG();
        if (!item) {
            break;
        }
        msg = JSDRV_CONTAINER_OF(item, struct msg_s, item);
        if (msg->header.level > self->level) {
            continue;
        }
        jsdrv_list_foreach(&self->dispatch, item) {
            dispatch = JSDRV_CONTAINER_OF(item, struct dispatch_s, item);
            dispatch->fn(dispatch->user_data, &msg->header, msg->filename, msg->message);
        }
    }
    UNLOCK_DISPATCH();
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
    struct log_s * self = (struct log_s *) lpParam;
    while (!self->quit) {
        WaitForSingleObject(self->event, 100);
        ResetEvent(self->event);
        process(self);
    }
    return 0;
}

static void thread_notify(struct log_s * self) {
    if (self && self->event) {
        SetEvent(self->event);
    }
}

static int32_t thread_start(struct log_s * self) {
    self->event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );
    self->thread = CreateThread(
            NULL,               // default security attributes
            0,                  // use default stack size
            log_thread,         // thread function name
            self,               // argument to thread function
            0,                  // use default creation flags
            &self->thread_id);  // returns the thread identifier
    if (self->thread == NULL) {
        JSDRV_LOGE("CreateThread log failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

static void thread_stop(struct log_s * self) {
    self->quit = 1;
    SetEvent(self->event);
    if (WAIT_OBJECT_0 != WaitForSingleObject(self->thread, 1000)) {
        printf("ERROR: log thread_stop not closed cleanly\n");
    }
    CloseHandle(self->thread);
    self->thread = NULL;
    CloseHandle(self->event);
    self->event = NULL;
}

#else
static void * log_thread(void * arg) {
    uint8_t rd_buf[1024];
    struct pollfd fds;
    struct log_s * self = (struct log_s *) arg;
    fds.fd = self->fd_read;
    fds.events = POLLIN;
    while (!self->quit) {
        fds.revents = 0;
        poll(&fds, 1, 100);
        if (read(self->fd_read, rd_buf, sizeof(rd_buf)) <= 0) {
            break;  // EOF or error
        }
        process(self);
    }
    return 0;
}

static void thread_notify(struct log_s * self) {
    uint8_t wr_buf[] = {1};
    if (write(self->fd_write, wr_buf, 1) <= 0) {
        printf("ERROR: thread_notify write failed, errno=%d\n", errno);
    }
}

static int32_t thread_start(struct log_s * self) {
    int pipefd[2];
    if (pipe(pipefd)) {
        return JSDRV_ERROR_IO;
    }
    self->fd_read = pipefd[0];
    self->fd_write = pipefd[1];
    fcntl(self->fd_read, F_SETFL, O_NONBLOCK);
    int rc = pthread_create(&self->thread_id, NULL, log_thread, self);
    if (rc) {
        JSDRV_LOGE("pthread_create failed %d", rc);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

static void thread_stop(struct log_s * self) {
    uint8_t wr_buf[] = {1};
    self->quit = 1;
    if (write(self->fd_write, wr_buf, 1) <= 0) {
        printf("ERROR: thread_stop write failed, errno=%d\n", errno);
    }
    int rc = pthread_join(self->thread_id, NULL);
    if (rc) {
        printf("ERROR: log thread_stop not closed cleanly: %d\n", rc);
    }
    close(self->fd_write);
    close(self->fd_read);
}
#endif

void jsdrv_log_initialize() {
    dprintf("jsdrv_log_initialize");
    struct log_s * self = (struct log_s *) &self_;

    if (0 == self->active_count) {
        self->msg_mutex = jsdrv_os_mutex_alloc("jsdrv_log_msg");
        LOCK_MSG();
        self->dispatch_mutex = jsdrv_os_mutex_alloc("jsdrv_log_dispatch");
        LOCK_DISPATCH();
        self->level = JSDRV_LOG_LEVEL_OFF;
        jsdrv_list_initialize(&self->dispatch);
        jsdrv_list_initialize(&self->msg_free);
        jsdrv_list_initialize(&self->msg_pend);
        for (size_t i = 0; i < MSG_COUNT_INIT; ++i) {
            struct msg_s *msg = jsdrv_alloc(sizeof(struct msg_s));
            jsdrv_list_initialize(&msg->item);
            jsdrv_list_add_tail(&self->msg_free, &msg->item);
        }
        ++self->active_count;
        UNLOCK_MSG();
        UNLOCK_DISPATCH();
        thread_start(self);
    } else {
        LOCK_MSG();
        LOCK_DISPATCH();
        ++self->active_count;
        UNLOCK_MSG();
        UNLOCK_DISPATCH();
    }
}

void jsdrv_log_finalize() {
    dprintf("jsdrv_log_finalize");
    struct log_s * self = (struct log_s *) &self_;
    if (0 == self->active_count) {
        dprintf("ERROR: jsdrv_log_finalize but 0 == self->active_count");
        return;
    }
    uint32_t do_finalize = 0;

    LOCK_DISPATCH();
    if (self->active_count) {
        --self->active_count;
        if (self->active_count == 0) {
            do_finalize = 1;
        }
    }
    UNLOCK_DISPATCH();

    dprintf("jsdrv_log_finalize do_finalize=%d", do_finalize);
    if (do_finalize) {
        thread_stop(self);
        list_free(&self->dispatch);
        list_free(&self->msg_pend);
        list_free(&self->msg_free);
        jsdrv_os_mutex_free(self->msg_mutex);
        jsdrv_os_mutex_free(self->dispatch_mutex);
    }
}

void jsdrv_log_level_set(int8_t level) {
    self_.level = level;
}

int8_t jsdrv_log_level_get() {
    return self_.level;
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
