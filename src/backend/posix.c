/*
* Copyright 2014-2026 Jetperch LLC
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
#include "jsdrv/os_event.h"
#include "jsdrv/os_mutex.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/os_sem.h"
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

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#endif

// --- Time ---

int64_t jsdrv_time_utc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t t = JSDRV_TIME_SECOND * ((int64_t) ts.tv_sec - JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS);
    t += JSDRV_NANOSECONDS_TO_TIME(ts.tv_nsec);
    return t;
}

uint32_t jsdrv_time_ms_u32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = 1000 * ((int64_t) ts.tv_sec - JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS);
    t += (uint64_t) (ts.tv_nsec / 1000000);
    return t;
}

// --- Mutex ---

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
#ifdef __APPLE__
        // macOS does not provide pthread_mutex_timedlock.
        // Use trylock with polling as a deadlock safety net.
        uint32_t elapsed = 0;
        while (elapsed < JSDRV_CONFIG_OS_MUTEX_LOCK_TIMEOUT_MS) {
            int rc = pthread_mutex_trylock(&mutex->mutex);
            if (rc == 0) {
                return;
            }
            struct timespec ts = {0, 1000000L};  // 1 ms
            nanosleep(&ts, NULL);
            ++elapsed;
        }
        snprintf(msg, sizeof(msg),
                 "mutex lock '%s' timed out", mutex->name);
        JSDRV_FATAL(msg);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t) ts.tv_nsec
            + (uint64_t) JSDRV_CONFIG_OS_MUTEX_LOCK_TIMEOUT_MS * 1000000ULL;
        ts.tv_sec += (time_t) (ns / 1000000000ULL);
        ts.tv_nsec = (long) (ns % 1000000000ULL);
        int rc = pthread_mutex_timedlock(&mutex->mutex, &ts);
        if (rc == 0) {
            return;
        }
        snprintf(msg, sizeof(msg),
                 "mutex lock '%s' failed %d", mutex->name, rc);
        JSDRV_FATAL(msg);
#endif
    }
}

void jsdrv_os_mutex_unlock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if (NULL != mutex) {
        int rc = pthread_mutex_unlock(&mutex->mutex);
        if (rc) {
            snprintf(msg, sizeof(msg),
                     "mutex unlock '%s' failed %d", mutex->name, rc);
            JSDRV_FATAL(msg);
        }
    }
}

// --- Fatal ---

void jsdrv_fatal(const char * file, uint32_t line, const char * msg) {
    printf("FATAL: %s:%u %s\n", file, line, msg);
    fflush(stdout);
    exit(1);
}

// --- Event ---

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

void jsdrv_os_event_free(jsdrv_os_event_t ev) {
    if (NULL != ev) {
        close(ev->fd_signal);
        close(ev->fd_poll);
        jsdrv_free(ev);
    }
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

int32_t jsdrv_os_event_wait(jsdrv_os_event_t ev, uint32_t timeout_ms) {
    struct pollfd pfd;
    pfd.fd = ev->fd_poll;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, (int) timeout_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) {
        return 0;
    }
    return JSDRV_ERROR_TIMED_OUT;
}

// --- Thread ---

int32_t jsdrv_thread_create(jsdrv_thread_t * thread,
                            jsdrv_thread_fn fn,
                            JSDRV_THREAD_ARG_TYPE fn_arg,
                            int32_t priority) {
    int rc;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    if (priority != 0) {
        struct sched_param param;
        int policy = SCHED_OTHER;
        int min_prio = sched_get_priority_min(policy);
        int max_prio = sched_get_priority_max(policy);
        int range = max_prio - min_prio;
        if (range > 0) {
            int mid = min_prio + range / 2;
            int p = mid + (priority * range) / 4;
            if (p < min_prio) p = min_prio;
            if (p > max_prio) p = max_prio;
            param.sched_priority = p;
            pthread_attr_setschedpolicy(&attr, policy);
            pthread_attr_setschedparam(&attr, &param);
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        }
    }

    rc = pthread_create(thread, &attr, fn, fn_arg);
    pthread_attr_destroy(&attr);
    if (rc) {
        // Retry without priority if permission denied
        if (priority != 0) {
            rc = pthread_create(thread, NULL, fn, fn_arg);
        }
        if (rc) {
            JSDRV_LOGE("pthread_create failed: %d", rc);
            return JSDRV_ERROR_UNSPECIFIED;
        }
    }
    return 0;
}

struct join_helper_s {
    pthread_t target;
    volatile int done;
};

static void * join_helper_fn(void * arg) {
    struct join_helper_s * h = (struct join_helper_s *) arg;
    pthread_join(h->target, NULL);
    h->done = 1;
    return NULL;
}

int32_t jsdrv_thread_join(jsdrv_thread_t * thread, uint32_t timeout_ms) {
    // Heap-allocate so the detached helper can safely write
    // to it even if we return early on timeout.
    struct join_helper_s * helper = jsdrv_alloc_clr(sizeof(*helper));
    helper->target = *thread;
    helper->done = 0;

    pthread_t helper_thread;
    if (pthread_create(&helper_thread, NULL, join_helper_fn, helper)) {
        jsdrv_free(helper);
        // Fallback: blocking join
        pthread_join(*thread, NULL);
        return 0;
    }
    pthread_detach(helper_thread);

    uint32_t elapsed = 0;
    uint32_t step = 1;
    while (!helper->done && elapsed < timeout_ms) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = step * 1000000L;
        nanosleep(&ts, NULL);
        elapsed += step;
        if (step < 10) {
            step++;
        }
    }

    if (!helper->done) {
        // helper is leaked intentionally: the detached thread
        // still holds a reference and will write done=1 later.
        JSDRV_LOGE("jsdrv_thread_join timed out");
        return JSDRV_ERROR_TIMED_OUT;
    }
    jsdrv_free(helper);
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

// --- Semaphore ---

struct jsdrv_os_sem_s {
#ifdef __APPLE__
    dispatch_semaphore_t dsema;
    int32_t initial_count;
    volatile int32_t balance;  // track net waits for safe free
#else
    sem_t sem;
#endif
    int32_t max_count;  // informational
};

jsdrv_os_sem_t jsdrv_os_sem_alloc(int32_t initial_count, int32_t max_count) {
    jsdrv_os_sem_t sem = jsdrv_alloc_clr(sizeof(*sem));
    sem->max_count = max_count;
#ifdef __APPLE__
    sem->initial_count = initial_count;
    sem->balance = 0;
    sem->dsema = dispatch_semaphore_create(initial_count);
    if (!sem->dsema) {
        jsdrv_free(sem);
        return NULL;
    }
#else
    if (sem_init(&sem->sem, 0, (unsigned int) initial_count)) {
        jsdrv_free(sem);
        return NULL;
    }
#endif
    return sem;
}

void jsdrv_os_sem_free(jsdrv_os_sem_t sem) {
    if (NULL != sem) {
#ifdef __APPLE__
        // dispatch_semaphore traps if released with count < initial.
        // Signal to restore the count before releasing.
        while (sem->balance > 0) {
            dispatch_semaphore_signal(sem->dsema);
            --sem->balance;
        }
        dispatch_release(sem->dsema);
#else
        sem_destroy(&sem->sem);
#endif
        jsdrv_free(sem);
    }
}

int32_t jsdrv_os_sem_wait(jsdrv_os_sem_t sem, uint32_t timeout_ms) {
#ifdef __APPLE__
    dispatch_time_t dt = dispatch_time(
        DISPATCH_TIME_NOW,
        (int64_t) timeout_ms * 1000000LL);
    long rc = dispatch_semaphore_wait(sem->dsema, dt);
    if (rc == 0) {
        ++sem->balance;
        return 0;
    }
    return JSDRV_ERROR_TIMED_OUT;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t) ts.tv_nsec
        + (uint64_t) timeout_ms * 1000000ULL;
    ts.tv_sec += (time_t) (ns / 1000000000ULL);
    ts.tv_nsec = (long) (ns % 1000000000ULL);
    while (1) {
        int rc = sem_timedwait(&sem->sem, &ts);
        if (rc == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return JSDRV_ERROR_TIMED_OUT;
    }
#endif
}

void jsdrv_os_sem_release(jsdrv_os_sem_t sem) {
#ifdef __APPLE__
    --sem->balance;
    dispatch_semaphore_signal(sem->dsema);
#else
    sem_post(&sem->sem);
#endif
}

// --- Heap ---

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

// --- Platform ---

int32_t jsdrv_platform_initialize(void) {
    heap_mutex = jsdrv_os_mutex_alloc("heap");
    struct rlimit limit = {
        .rlim_cur = 0,
        .rlim_max = 0,
    };
    getrlimit(RLIMIT_NOFILE, &limit);
    if (limit.rlim_max < 4096) {
        JSDRV_LOGE("file descriptor limit too small: %llu",
                    (unsigned long long) limit.rlim_max);
        return JSDRV_ERROR_NOT_SUPPORTED;
    }
    limit.rlim_cur = 4096;
    setrlimit(RLIMIT_NOFILE, &limit);
    return 0;
}

void jsdrv_platform_finalize(void) {
}
