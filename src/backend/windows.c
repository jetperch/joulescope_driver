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

#include "jsdrv_prv/windows.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv/cstr.h"
#include "jsdrv/os_event.h"
#include "jsdrv/os_mutex.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/os_sem.h"
#include "jsdrv/time.h"
#include <stdio.h>
#include <stdlib.h>

// https://stackoverflow.com/questions/1387064/how-to-get-the-error-message-from-the-error-code-returned-by-getlasterror
BOOL GetErrorMessage(DWORD dwErrorCode, char * pBuffer, DWORD cchBufferLength) {
    char* p = pBuffer;
    if (cchBufferLength == 0) {
        return FALSE;
    }
    pBuffer[0] = 0;

    DWORD cchMsg = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  NULL,
                                  dwErrorCode,
                                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  pBuffer,
                                  cchBufferLength,
                                  NULL);

    while (*p) {
        if ((*p == '\n') || (*p == '\r')) {
            *p = 0;
            break;
        }
        ++p;
    }
    return (cchMsg > 0);
}

int64_t jsdrv_time_utc(void) {
    static const int64_t offset_s = 131592384000000000LL;  // 100 ns
    static const uint64_t frequency = 10000000; // 100 ns
    FILETIME filetime;
    GetSystemTimePreciseAsFileTime(&filetime);
    uint64_t t = ((uint64_t) filetime.dwLowDateTime) | (((uint64_t) filetime.dwHighDateTime) << 32);
    t -= offset_s;
    return JSDRV_COUNTER_TO_TIME(t, frequency);
}

uint32_t jsdrv_time_ms_u32(void) {
    return GetTickCount();
}

// --- Mutex ---

jsdrv_os_mutex_t jsdrv_os_mutex_alloc(const char * name) {
    jsdrv_os_mutex_t mutex;
    JSDRV_LOGI("mutex alloc '%s'", name);
    mutex = jsdrv_alloc_clr(sizeof(*mutex));
    mutex->mutex = CreateMutex(NULL, FALSE, NULL);
    JSDRV_ASSERT_ALLOC(mutex->mutex);
    jsdrv_cstr_copy(mutex->name, name, sizeof(mutex->name));
    return mutex;
}

void jsdrv_os_mutex_free(jsdrv_os_mutex_t mutex) {
    if (NULL != mutex) {
        CloseHandle(mutex->mutex);
        jsdrv_free(mutex);
    }
}

void jsdrv_os_mutex_lock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if ((NULL != mutex) && mutex->mutex) {
        DWORD rc = WaitForSingleObject(mutex->mutex, JSDRV_CONFIG_OS_MUTEX_LOCK_TIMEOUT_MS);
        if (WAIT_OBJECT_0 != rc) {
            _snprintf_s(msg, sizeof(msg), sizeof(msg), "mutex lock '%s' failed", mutex->name);
            JSDRV_FATAL(msg);
        }
    } else {
        JSDRV_LOGD1("lock, but mutex is null");
    }
}

void jsdrv_os_mutex_unlock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if ((NULL != mutex) && mutex->mutex) {
        if (!ReleaseMutex(mutex->mutex)) {
            _snprintf_s(msg, sizeof(msg), sizeof(msg), "mutex unlock '%s' failed", mutex->name);
            JSDRV_FATAL(msg);
        }
    } else {
        JSDRV_LOGD1("unlock, but mutex is null");
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
    return CreateEvent(NULL, TRUE, FALSE, NULL);
}

void jsdrv_os_event_free(jsdrv_os_event_t ev) {
    CloseHandle(ev);
}

void jsdrv_os_event_signal(jsdrv_os_event_t ev) {
    SetEvent(ev);
}

void jsdrv_os_event_reset(jsdrv_os_event_t ev) {
    ResetEvent(ev);
}

int32_t jsdrv_os_event_wait(jsdrv_os_event_t ev, uint32_t timeout_ms) {
    DWORD rc = WaitForSingleObject(ev, timeout_ms);
    if (WAIT_OBJECT_0 == rc) {
        return 0;
    }
    return JSDRV_ERROR_TIMED_OUT;
}

// --- Thread ---

int32_t jsdrv_thread_create(jsdrv_thread_t * thread, jsdrv_thread_fn fn, THREAD_ARG_TYPE fn_arg, int priority) {
    thread->thread = CreateThread(NULL, 0, fn, fn_arg, 0, &thread->thread_id);
    if (thread->thread == NULL) {
        JSDRV_LOGE("CreateThread failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (priority > 2) {
        priority = 2;
    } else if (priority < -2) {
        priority = -2;
    }
    switch (priority) {
        case -2: priority = THREAD_PRIORITY_LOWEST; break;
        case -1: priority = THREAD_PRIORITY_BELOW_NORMAL; break;
        case 0: priority = THREAD_PRIORITY_NORMAL; break;
        case 1: priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case 2: priority = THREAD_PRIORITY_HIGHEST; break;
        default: priority = 0; break;
    }
    if (!SetThreadPriority(thread->thread, priority)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }
    return 0;
}

int32_t jsdrv_thread_join(jsdrv_thread_t * thread, uint32_t timeout_ms) {
    int32_t rv = 0;
    if (WAIT_OBJECT_0 != WaitForSingleObject(thread->thread, timeout_ms)) {
        JSDRV_LOGE("jsdrv thread not closed cleanly");
        rv = JSDRV_ERROR_TIMED_OUT;
    }
    CloseHandle(thread->thread);
    thread->thread = NULL;
    thread->thread_id = 0;
    return rv;
}

bool jsdrv_thread_is_current(jsdrv_thread_t const * thread) {
    return (thread->thread_id == GetCurrentThreadId());
}

void jsdrv_thread_sleep_ms(uint32_t duration_ms) {
    Sleep(duration_ms);
}

// --- Semaphore ---

struct jsdrv_os_sem_s {
    HANDLE handle;
};

jsdrv_os_sem_t jsdrv_os_sem_alloc(int32_t initial_count, int32_t max_count) {
    jsdrv_os_sem_t sem = jsdrv_alloc_clr(sizeof(*sem));
    sem->handle = CreateSemaphore(NULL, initial_count, max_count, NULL);
    if (NULL == sem->handle) {
        jsdrv_free(sem);
        return NULL;
    }
    return sem;
}

void jsdrv_os_sem_free(jsdrv_os_sem_t sem) {
    if (NULL != sem) {
        CloseHandle(sem->handle);
        jsdrv_free(sem);
    }
}

int32_t jsdrv_os_sem_wait(jsdrv_os_sem_t sem, uint32_t timeout_ms) {
    DWORD rc = WaitForSingleObject(sem->handle, timeout_ms);
    if (WAIT_OBJECT_0 == rc) {
        return 0;
    }
    return JSDRV_ERROR_TIMED_OUT;
}

void jsdrv_os_sem_release(jsdrv_os_sem_t sem) {
    ReleaseSemaphore(sem->handle, 1, NULL);
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

    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        WINDOWS_LOGE("Could not raise process priority using %s", "SetPriorityClass");
    }

    timeBeginPeriod(1);
    return 0;
}

void jsdrv_platform_finalize(void) {
    timeEndPeriod(1);
}
