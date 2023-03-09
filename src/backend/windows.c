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

#include "jsdrv_prv/windows.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/event.h"
#include "jsdrv_prv/mutex.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv/time.h"
#include <stdio.h>
#include <stdlib.h>

// https://stackoverflow.com/questions/1387064/how-to-get-the-error-message-from-the-error-code-returned-by-getlasterror
// This functions fills a caller-defined character buffer (pBuffer)
// of max length (cchBufferLength) with the human-readable error message
// for a Win32 error code (dwErrorCode).
//
// Returns TRUE if successful, or FALSE otherwise.
// If successful, pBuffer is guaranteed to be NUL-terminated.
// On failure, the contents of pBuffer are undefined.
BOOL GetErrorMessage(DWORD dwErrorCode, char * pBuffer, DWORD cchBufferLength) {
    char* p = pBuffer;
    if (cchBufferLength == 0) {
        return FALSE;
    }
    pBuffer[0] = 0;

    DWORD cchMsg = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  NULL,  /* (not used with FORMAT_MESSAGE_FROM_SYSTEM) */
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
    // Contains a 64-bit value representing the number of 100-nanosecond intervals since January 1, 1601 (UTC).
    // python
    // import dateutil.parser
    // dateutil.parser.parse('2018-01-01T00:00:00Z').timestamp() - dateutil.parser.parse('1601-01-01T00:00:00Z').timestamp()
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

jsdrv_os_mutex_t jsdrv_os_mutex_alloc(const char * name) {
    jsdrv_os_mutex_t mutex;
    JSDRV_LOGI("mutex alloc '%s'", name);
    mutex = jsdrv_alloc_clr(sizeof(*mutex));
    mutex->mutex = CreateMutex(
            NULL,                   // default security attributes
            FALSE,                  // initially not owned
            NULL);                  // unnamed mutex
    JSDRV_ASSERT_ALLOC(mutex->mutex);
    jsdrv_cstr_copy(mutex->name, name, sizeof(mutex->name));
    return mutex;
}

void jsdrv_os_mutex_free(jsdrv_os_mutex_t mutex) {
    if (mutex) {
        CloseHandle(mutex->mutex);
        jsdrv_free(mutex);
    }
}

void jsdrv_os_mutex_lock(jsdrv_os_mutex_t mutex) {
    char msg[128];
    if (mutex && mutex->mutex) {
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
    if (mutex && mutex->mutex) {
        if (!ReleaseMutex(mutex->mutex)) {
            _snprintf_s(msg, sizeof(msg), sizeof(msg), "mutex unlock '%s' failed", mutex->name);
            JSDRV_FATAL(msg);
        }
    } else {
        JSDRV_LOGD1("unlock, but mutex is null");
    }
}

void jsdrv_fatal(const char * file, uint32_t line, const char * msg) {
    printf("FATAL: %s:%u %s\n", file, line, msg);
    fflush(stdout);
    exit(1);
}

void jsdrv_os_event_free(jsdrv_os_event_t ev) {
    CloseHandle(ev);
}

jsdrv_os_event_t jsdrv_os_event_alloc(void) {
    return CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            FALSE, // start unsignaled
            NULL   // no name
    );
}

void jsdrv_os_event_signal(jsdrv_os_event_t ev) {
    SetEvent(ev);
}

void jsdrv_os_event_reset(jsdrv_os_event_t ev) {
    ResetEvent(ev);
}

int32_t jsdrv_thread_create(jsdrv_thread_t * thread, jsdrv_thread_fn fn, THREAD_ARG_TYPE fn_arg) {
    thread->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            fn,                     // thread function name
            fn_arg,                 // argument to thread function
            0,                      // use default creation flags
            &thread->thread_id);    // returns the thread identifier
    if (thread->thread == NULL) {
        JSDRV_LOGE("CreateThread failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    return 0;
}

int32_t jsdrv_thread_join(jsdrv_thread_t * thread, uint32_t timeout_ms) {
    int32_t rv = 0;
    if (WAIT_OBJECT_0 != WaitForSingleObject(thread->thread, timeout_ms)) {
        JSDRV_LOGE("jsdrv thread not closed cleanly");
        rv = JSDRV_ERROR_UNSPECIFIED;
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

int32_t jsdrv_platform_initialize(void) {
    return 0;
}
