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
 * @brief Cross-platform event (manual-reset) abstraction.
 */

#ifndef JSDRV_OS_EVENT_H_
#define JSDRV_OS_EVENT_H_

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

#if _WIN32
#include <windows.h>
#else  /* presume POSIX */
#include <pthread.h>
#endif

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_os_event Event abstraction
 *
 * @brief Provide a cross-platform manual-reset event.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

#if _WIN32
typedef HANDLE jsdrv_os_event_t;
#else  /* presume POSIX */
struct jsdrv_os_event_s {
    int fd_poll;
    int events;
    int fd_signal;
};
typedef struct jsdrv_os_event_s * jsdrv_os_event_t;
#endif

/**
 * @brief Allocate a new event in the unsignaled state.
 *
 * @return The event or NULL on failure.
 */
JSDRV_COMPILER_ALLOC(jsdrv_os_event_free) JSDRV_API jsdrv_os_event_t jsdrv_os_event_alloc(void);

/**
 * @brief Free an existing event.
 *
 * @param ev The event to free.
 */
JSDRV_API void jsdrv_os_event_free(jsdrv_os_event_t ev);

/**
 * @brief Signal an event.
 *
 * @param ev The event to signal.
 *
 * The event remains signaled until jsdrv_os_event_reset() is called.
 */
JSDRV_API void jsdrv_os_event_signal(jsdrv_os_event_t ev);

/**
 * @brief Reset an event to the unsignaled state.
 *
 * @param ev The event to reset.
 */
JSDRV_API void jsdrv_os_event_reset(jsdrv_os_event_t ev);

/**
 * @brief Wait for an event to become signaled.
 *
 * @param ev The event to wait on.
 * @param timeout_ms The maximum time to wait in milliseconds.
 *      Use 0 to poll without blocking.
 * @return 0 if the event was signaled, or
 *      JSDRV_ERROR_TIMED_OUT if the timeout elapsed.
 *
 * This function does NOT reset the event after returning.
 * Call jsdrv_os_event_reset() explicitly if auto-reset
 * behavior is desired.
 */
JSDRV_API int32_t jsdrv_os_event_wait(jsdrv_os_event_t ev,
                                       uint32_t timeout_ms);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_EVENT_H_ */
