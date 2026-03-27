/*
 * Copyright 2026 Jetperch LLC
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
 * @brief Cross-platform counting semaphore abstraction.
 */

#ifndef JSDRV_OS_SEM_H__
#define JSDRV_OS_SEM_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_os_sem Semaphore abstraction
 *
 * @brief Provide a cross-platform counting semaphore with timeout.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

struct jsdrv_os_sem_s;
typedef struct jsdrv_os_sem_s * jsdrv_os_sem_t;

/**
 * @brief Allocate a new semaphore.
 *
 * @param initial_count The initial semaphore count.
 * @param max_count The maximum semaphore count.
 * @return The semaphore or NULL on failure.
 */
JSDRV_COMPILER_ALLOC(jsdrv_os_sem_free)
JSDRV_API jsdrv_os_sem_t jsdrv_os_sem_alloc(int32_t initial_count,
                                              int32_t max_count);

/**
 * @brief Free an existing semaphore.
 *
 * @param sem The semaphore to free.
 */
JSDRV_API void jsdrv_os_sem_free(jsdrv_os_sem_t sem);

/**
 * @brief Wait (decrement) the semaphore.
 *
 * @param sem The semaphore.
 * @param timeout_ms The maximum time to wait in milliseconds.
 *      Use 0 to try without blocking.
 * @return 0 on success, or JSDRV_ERROR_TIMED_OUT.
 */
JSDRV_API int32_t jsdrv_os_sem_wait(jsdrv_os_sem_t sem,
                                     uint32_t timeout_ms);

/**
 * @brief Release (increment) the semaphore.
 *
 * @param sem The semaphore.
 */
JSDRV_API void jsdrv_os_sem_release(jsdrv_os_sem_t sem);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_SEM_H__ */
