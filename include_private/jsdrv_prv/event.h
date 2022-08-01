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
 * @brief OS event abstraction.
 */

#ifndef JSDRV_OS_EVENT_H__
#define JSDRV_OS_EVENT_H__

#include "jsdrv/cmacro_inc.h"

#if _WIN32
#include <windows.h>
#else  /* presume POSIX */
#include <pthread.h>
#endif

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_event Event abstraction
 *
 * @brief Provide a simple event abstraction.
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
 * @brief Free an existing event.
 *
 * @param mutex The event to free, previous produced using jsdrv_os_event_alloc().
 */
void jsdrv_os_event_free(jsdrv_os_event_t ev) ;

/**
 * @brief Allocate a new event.
 *
 * @return The event or NULL.
 */
JSDRV_COMPILER_ALLOC(jsdrv_os_event_free) jsdrv_os_event_t jsdrv_os_event_alloc();

/**
 * @brief Signal an event.
 *
 * @param ev The event to signal.
 */
void jsdrv_os_event_signal(jsdrv_os_event_t ev);

/**
 * @brief Reset an event.
 *
 * @param ev The event to reset to the unsignaled state.
 */
void jsdrv_os_event_reset(jsdrv_os_event_t ev);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_EVENT_H__ */
