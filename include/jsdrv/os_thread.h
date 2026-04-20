/*
 * Copyright 2021-2026 Jetperch LLC
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
 * @brief Cross-platform thread abstraction.
 */

#ifndef JSDRV_OS_THREAD_H_
#define JSDRV_OS_THREAD_H_

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

#if _WIN32
#include <windows.h>
#else  /* presume POSIX */
#include <pthread.h>
#endif

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_os_thread Thread abstraction
 *
 * @brief Provide a cross-platform thread abstraction.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

#if _WIN32
#define JSDRV_THREAD_RETURN_TYPE DWORD WINAPI
#define JSDRV_THREAD_ARG_TYPE LPVOID
#define JSDRV_THREAD_RETURN() return 0
typedef DWORD (__stdcall *jsdrv_thread_fn)(LPVOID arg);
struct jsdrv_thread_s {
    HANDLE thread;
    DWORD thread_id;
};
typedef struct jsdrv_thread_s jsdrv_thread_t;
#else  /* presume POSIX */
#define JSDRV_THREAD_RETURN_TYPE void *
#define JSDRV_THREAD_ARG_TYPE void *
typedef void * (*jsdrv_thread_fn)(void * arg);
#define JSDRV_THREAD_RETURN() return NULL
typedef pthread_t jsdrv_thread_t;
#endif

/**
 * @brief Create a new thread.
 *
 * @param thread The thread instance to populate.
 * @param fn The function to call in the new thread context.
 * @param fn_arg The argument to fn.
 * @param priority The thread priority.
 *      0 is default. 1 is above normal, 2 is highest.
 *      -1 is below normal, -2 is lowest.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_thread_create(jsdrv_thread_t * thread,
                                       jsdrv_thread_fn fn,
                                       JSDRV_THREAD_ARG_TYPE fn_arg,
                                       int32_t priority);

/**
 * @brief Wait for a thread to finish and release resources.
 *
 * @param thread The thread instance.
 * @param timeout_ms The maximum time to wait in milliseconds.
 * @return 0 on success, or JSDRV_ERROR_TIMED_OUT.
 *
 * On POSIX, a timed-out join leaks a small helper allocation
 * (~24 bytes) because the detached join thread cannot be
 * safely cancelled.  This is acceptable for rare timeout paths.
 */
JSDRV_API int32_t jsdrv_thread_join(jsdrv_thread_t * thread,
                                     uint32_t timeout_ms);

/**
 * @brief Check if the caller is the specified thread.
 *
 * @param thread The thread instance to compare.
 * @return true if the calling thread is the specified thread.
 */
JSDRV_API bool jsdrv_thread_is_current(const jsdrv_thread_t * thread);

/**
 * @brief Sleep the calling thread.
 *
 * @param duration_ms The duration to sleep in milliseconds.
 */
JSDRV_API void jsdrv_thread_sleep_ms(uint32_t duration_ms);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_THREAD_H_ */
