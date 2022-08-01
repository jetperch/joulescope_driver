/*
 * Copyright 2021-2022 Jetperch LLC
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
 * @brief OS thread abstraction.
 */

#ifndef JSDRV_OS_THREAD_H__
#define JSDRV_OS_THREAD_H__

#include "jsdrv/cmacro_inc.h"

#if _WIN32
#include <windows.h>
#else  /* presume POSIX */
#include <pthread.h>
#endif

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_thread Thread abstraction
 *
 * @brief Provide a simple thread abstraction.
 *
 * @{
 */


JSDRV_CPP_GUARD_START

#if _WIN32
#define THREAD_RETURN_TYPE DWORD WINAPI
#define THREAD_ARG_TYPE LPVOID
#define THREAD_RETURN() return 0
typedef DWORD (*jsdrv_thread_fn)(LPVOID arg);
struct jsdrv_thread_s {
    HANDLE thread;
    DWORD thread_id;
};
typedef struct jsdrv_thread_s jsdrv_thread_t;
#else  /* presume POSIX */
#define THREAD_RETURN_TYPE void *
#define THREAD_ARG_TYPE void *
typedef void * (*jsdrv_thread_fn)(void * arg);
#define THREAD_RETURN() return NULL
typedef pthread_t jsdrv_thread_t;
#endif

int32_t jsdrv_thread_create(jsdrv_thread_t * thread, jsdrv_thread_fn fn, THREAD_ARG_TYPE fn_arg);
int32_t jsdrv_thread_join(jsdrv_thread_t * thread, uint32_t timeout_ms);
void jsdrv_thread_sleep_ms(uint32_t duration_ms);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_EVENT_H__ */
