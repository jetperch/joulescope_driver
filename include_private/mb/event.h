/*
 * SPDX-FileCopyrightText: Copyright 2021-2024 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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

#ifndef MB_EVENT_H__
#define MB_EVENT_H__

#include "mb/cdef.h"


MB_CPP_GUARD_START

/**
 * @ingroup mb_core Core
 *
 * @{
 */


/**
 * @brief The standardized OS events.
 */
enum mb_ev_e {
    MB_EV_NULL = 0x00,              ///< Null or undefined event.
    MB_EV_RESET = 0x01,             ///< Reset the task state.
    MB_EV_INITIALIZE = 0x02,        ///< Guaranteed first task event.  Used to allocate resources.
    MB_EV_FINALIZE = 0x03,          ///< Guaranteed last task event.  Used to free resources.  Not normally used in production embedded code.
    MB_EV_TIMER_EXPIRED = 0x04,     ///< Default event for timer expiration.  The task may use user-defined events.
    MB_EV_MESSAGE = 0x05,           ///< Used internally in event queue to indicate message.  The actual event for the message is in msg->event.
    MB_EV_ENTER = 0x06,             ///< Enter action, usually for state machine enter state.
    MB_EV_EXIT = 0x07,              ///< Stop action, usually for state machine leave state.
    MB_EV_TRANSMIT = 0x08,          ///< Transmit data from this microcontroller to the outside.
    MB_EV_RECEIVE = 0x09,           ///< Receive data from the outside to this microcontroller.
    MB_EV_TRANSACT = 0x0A,          ///< Simultaneously transmit and receive data.
    MB_EV_RSVD_0B = 0x0B,           ///< Reserved 0x0B for future standard OS event.
    MB_EV_OPEN_REQ = 0x0C,          ///< Request to open the resource associated with a task.
    MB_EV_CLOSE_REQ = 0x0D,         ///< Request to close the resource associated with a task.
    MB_EV_STATISTICS_TICK = 0x0E,   ///< Default event for statistics generation on mb_sys_topic_stick.
    MB_EV_RSVF = 0x0F,              ///< Reserved 0x0F for a future standard OS event.

    MB_EV_USER_FIRST = 0x10,        ///< The first user-defined event.
};

/** @} */

MB_CPP_GUARD_END

#endif /* MB_EVENT_H__ */
