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
 * @brief OS Mutex abstraction.
 */

#ifndef JSDRV_OS_MUTEX_H__
#define JSDRV_OS_MUTEX_H__

#include "jsdrv/cmacro_inc.h"

#if _WIN32
#include <windows.h>
#else  /* presume POSIX */
#include <pthread.h>
#endif

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_mutex Mutex abstraction
 *
 * @brief Provide a simple mutex abstraction.
 *
 * @{
 */

#define JSDRV_CONFIG_OS_MUTEX_LOCK_TIMEOUT_MS (1000U)


JSDRV_CPP_GUARD_START

#if _WIN32
struct jsdrv_os_mutex_s {
    HANDLE mutex;
    char name[32];
};
typedef struct jsdrv_os_mutex_s * jsdrv_os_mutex_t;
#else  /* presume POSIX */
struct jsdrv_os_mutex_s {
    pthread_mutex_t mutex;
    char name[32];
};
typedef struct jsdrv_os_mutex_s * jsdrv_os_mutex_t;
#endif

/**
 * @brief Free an existing mutex.
 *
 * @param mutex The mutex to free, previous produced using jsdrv_os_mutex_alloc().
 */
void jsdrv_os_mutex_free(jsdrv_os_mutex_t mutex) ;

/**
 * @brief Allocate a new mutex.
 *
 * @param name The mutex name for platforms that support debug info.
 * @return The mutex or 0.
 */
JSDRV_COMPILER_ALLOC(jsdrv_os_mutex_free) jsdrv_os_mutex_t jsdrv_os_mutex_alloc(const char * name);

/**
 * @brief Lock a mutex.
 *
 * @param mutex The mutex to lock.  If NULL, then skip the lock.
 *
 * Be sure to call jsdrv_os_mutex_unlock() when done.
 *
 * This function will use the default platform timeout.
 * An lock that takes longer than the timeout indicates
 * a system failure.  In deployed embedded systems, this
 * should trip the watchdog timer.
 */
void jsdrv_os_mutex_lock(jsdrv_os_mutex_t mutex);

/**
 * @brief Unlock a mutex.
 *
 * @param mutex The mutex to unlock, which was previously locked
 *      with jsdrv_os_mutex_lock().  If NULL, then skip the unlock.
 */
void jsdrv_os_mutex_unlock(jsdrv_os_mutex_t mutex);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_MUTEX_H__ */
