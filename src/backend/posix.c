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

#include "jsdrv_prv/assert.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/event.h"
#include "jsdrv_prv/mutex.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>

int64_t jsdrv_time_utc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t t = JSDRV_TIME_SECOND * ((int64_t) ts.tv_sec -JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS);
    t += JSDRV_NANOSECONDS_TO_TIME(ts.tv_nsec);
    return t;
}

uint32_t jsdrv_time_ms_u32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = 1000 * ((int64_t) ts.tv_sec -JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS);
    t += (uint64_t) (ts.tv_nsec / 1000000);
    return t;
}

jsdrv_os_mutex_t jsdrv_os_mutex_alloc(const char * name) {
    jsdrv_os_mutex_t mutex;
    JSDRV_LOGI("mutex alloc '%s'", name);
    mutex = jsdrv_alloc_clr(sizeof(*mutex));
    if (pthread_mutex_init(&mutex->mutex, NULL)) {
        jsdrv_free(mutex);
        return NULL;
    }
    jsdrv_cstr_copy(mutex->name, name, sizeof(mutex->name));
    return mutex;
}

void jsdrv_os_mutex_free(jsdrv_os_mutex_t mutex) {
    if (NULL != mutex) {
        pthread_mutex_destroy(&mutex->mutex);
        jsdrv_free(mutex);
    }
}

void jsdrv_os_mutex_lock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if (NULL != mutex) {
        int rc = pthread_mutex_lock(&mutex->mutex);
        if (rc) {
            snprintf(msg, sizeof(msg), "mutex lock '%s' failed %d", mutex->name, rc);
            JSDRV_FATAL(msg);
        }
    }
}

void jsdrv_os_mutex_unlock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if (NULL != mutex) {
        int rc = pthread_mutex_unlock(&mutex->mutex);
        if (rc) {
            snprintf(msg, sizeof(msg), "mutex unlock '%s' failed %d", mutex->name, rc);
            JSDRV_FATAL(msg);
        }
    }
}

void jsdrv_fatal(const char * file, uint32_t line, const char * msg) {
    printf("FATAL: %s:%u %s\n", file, line, msg);
    fflush(stdout);
    exit(1);
}

void jsdrv_os_event_free(jsdrv_os_event_t ev) {
    close(ev->fd_signal);
    close(ev->fd_poll);
    jsdrv_free(ev);
}

jsdrv_os_event_t jsdrv_os_event_alloc(void) {
    int pipefd[2];
    jsdrv_os_event_t ev;
    ev = jsdrv_alloc_clr(sizeof(*ev));
    if (!ev) {
        return NULL;
    }
    if (pipe(pipefd)) {
        jsdrv_free(ev);
        return NULL;
    }
    ev->fd_poll = pipefd[0];
    ev->events = POLLIN;
    ev->fd_signal = pipefd[1];
    fcntl(ev->fd_poll, F_SETFL, O_NONBLOCK);
    return ev;
}

void jsdrv_os_event_signal(jsdrv_os_event_t ev) {
    uint8_t wr_buf[1] = {1};
    if (write(ev->fd_signal, wr_buf, 1) <= 0) {
        JSDRV_LOGE("jsdrv_os_event_signal failed %d", errno);
    }
}

void jsdrv_os_event_reset(jsdrv_os_event_t ev) {
    uint8_t rd_buf[1024];
    if (read(ev->fd_poll, rd_buf, sizeof(rd_buf)) < 0) {
        ; // reset even on error.
    }
}

int32_t jsdrv_thread_create(jsdrv_thread_t * thread, jsdrv_thread_fn fn, THREAD_ARG_TYPE fn_arg, int priority) {
    (void) priority;
    int rc;

    // may need permissions to change thread priority
    // consider pthread_attr_setschedpolicy SCHED_RR
    // pthread_attr_setschedparam

    rc = pthread_create(thread, NULL, fn, fn_arg);
    if (rc) {
        JSDRV_LOGE("pthread_create failed: %d", rc);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

int32_t jsdrv_thread_join(jsdrv_thread_t * thread, uint32_t timeout_ms) {
    (void) timeout_ms;
    int rc = pthread_join(*thread, NULL);
    if (rc) {
        JSDRV_LOGE("jsdrv_thread_join failed: %d", rc);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

bool jsdrv_thread_is_current(jsdrv_thread_t const * thread) {
    return (*thread == pthread_self());
}

void jsdrv_thread_sleep_ms(uint32_t duration_ms) {
    struct timespec ts;
    ts.tv_sec = duration_ms / 1000;
    ts.tv_nsec = (duration_ms - (ts.tv_sec * 1000)) * 1000000;
    nanosleep(&ts, NULL);
}

static jsdrv_os_mutex_t heap_mutex = NULL;

void jsdrv_free(void * ptr) {
    jsdrv_os_mutex_lock(heap_mutex);
    free(ptr);
    jsdrv_os_mutex_unlock(heap_mutex);
}

void * jsdrv_alloc(size_t size_bytes) {
    jsdrv_os_mutex_lock(heap_mutex);
    void * ptr = malloc(size_bytes);
    if (!ptr) {
        JSDRV_FATAL("out of memory");
    }
    jsdrv_os_mutex_unlock(heap_mutex);
    return ptr;
}

int32_t jsdrv_platform_initialize(void) {
    heap_mutex = jsdrv_os_mutex_alloc("heap");
    struct rlimit limit = {
        .rlim_cur = 0,
        .rlim_max = 0,
    };
    getrlimit(RLIMIT_NOFILE, &limit);
    if (limit.rlim_max < 4096) {
        JSDRV_LOGE("file descriptor limit too small: %llu", limit.rlim_max);
        return JSDRV_ERROR_NOT_SUPPORTED;
    }
    limit.rlim_cur = 4096;
    setrlimit(RLIMIT_NOFILE, &limit);
    return 0;
}

void jsdrv_platform_finalize(void) {
}
