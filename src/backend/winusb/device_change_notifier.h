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

/**
 * \file
 * \brief Call a function whenever devices are inserted or removed.
 *
 * This module starts a private thread and a Window which then registers for
 * the WM_DEVICECHANGE message:
 * http://msdn.microsoft.com/en-us/library/windows/desktop/aa363480(v=vs.85).aspx
 * Upon receipt of the message, the window with start a timer.  When the timer
 * expires, it calls the registered callback.
 */

#include "jsdrv/cmacro_inc.h"

JSDRV_CPP_GUARD_START

/**
 * @brief Function called when devices have changed.
 *
 * @param cookie The associated data provided to 
 *      device_change_notifier_initialize().
 *
 * For asynchronous systems, the callback can set a manual reset event.  The 
 * event loop can WaitForMultipleObjects and later use WaitForSingleObject with
 * no timeout to query the event.  When set, the event loop should clear the
 * event with ResetEvent and then scan for new devices.
 */
typedef void (*device_change_notifier_callback)(void* cookie);


/**
 * @brief Initialize the device change system.
 *
 * @param callback Function to call when devices change.  The callback is
 *      called from an internal thread without any synchronization.  Any
 *      required synchronization is the responsibility of the callback.
 *      The callback must remain valid until device_change_notifier_finalize().
 * @param cookie The associated data to pass to callback.
 * @return 0 on success, 1 if already initialized or other error code on
 *      failure.
 *
 * This module only services a single callback.  Attempting to register another
 * callback without first calling device_change_notifier_finalize() is an 
 * error, and the new callback will not be registered.
 */
int device_change_notifier_initialize(
        device_change_notifier_callback callback,
        void * cookie);


/**
 * @brief Finalize the device change system and free all resources.
 *
 * Multiple, repeated calls to device_change_notifier_finalize are allowed.
 * The subsequent calls do nothing since all resources were previously freed.
 *
 * @return 0.
 */
int device_change_notifier_finalize();

JSDRV_CPP_GUARD_END
