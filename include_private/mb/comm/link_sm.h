/*
 * SPDX-FileCopyrightText: Copyright 2025 Jetperch LLC
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

/**
 * @file
 *
 * @brief Communication link layer connection state machine.
 */

#ifndef MB_COMM_LINK_SM_H_
#define MB_COMM_LINK_SM_H_

#include "mb/cdef.h"
#include "mb/state_machine.h"

MB_CPP_GUARD_START

/**
 * @ingroup mb_comm
 * @defgroup mb_comm_link Link
 *
 * @brief Communication link layer.
*/

enum mb_comm_link_sm_event_e {
    // incoming events
    MB_COMM_LINK_SM_EV_CONNECT_REQ = MB_EV_USER_FIRST,
    MB_COMM_LINK_SM_EV_CONNECT_ACK,
    MB_COMM_LINK_SM_EV_IDENTITY_RECEIVED,
    MB_COMM_LINK_SM_EV_DISCONNECT_REQ,
    MB_COMM_LINK_SM_EV_DISCONNECT_ACK,
    MB_COMM_LINK_SM_EV_TIMEOUT,    // keep MB_EV_TIMEOUT for other purposes
    MB_COMM_LINK_SM_EV_LINK_LOSS,  // frame_id sync lost, remote unresponsive

    // outgoing actions
    MB_COMM_LINK_SM_EV_RESET_BUFFERS,       // rx_frame_id = tx_frame_id = 0, clear data link buffers
    MB_COMM_LINK_SM_EV_SEND_FRAME_CONTROL,
    MB_COMM_LINK_SM_EV_SEND_IDENTITY,
    MB_COMM_LINK_SM_EV_PUBLISH_CONNECTED,

    // for next set of events
    MB_COMM_LINK_SM_EV_NEXT,
};


#if MB_DEF_STATE_MACHINE
#else
struct mb_sm_s * mb_comm_link_sm_factory(void);

enum mb_comm_link_sm_st_e {
    MB_COMM_LINK_SM_ST___GLOBAL__ = 0,
    MB_COMM_LINK_SM_ST_CLOSED = 1,
    MB_COMM_LINK_SM_ST_AWAIT_CONNECT = 2,
    MB_COMM_LINK_SM_ST_AWAIT_IDENTITY = 3,
    MB_COMM_LINK_SM_ST_CONNECTED = 4,
    MB_COMM_LINK_SM_ST_AWAIT_CLOSE = 5,
};

struct mb_comm_link_sm_context_s {
    uint8_t is_initiator;
    uint8_t timeout_timer_id;
};

#endif


MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_LINK_SM_H_ */