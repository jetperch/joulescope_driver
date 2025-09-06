/*
 * Copyright 2014-2024 Jetperch LLC
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
 * @brief Message communication frame format.
 */

#ifndef MB_COMM_LINK_H_
#define MB_COMM_LINK_H_

#include "mb/cdef.h"

MB_CPP_GUARD_START

/**
 * @ingroup mb_comm
 * @defgroup mb_comm_link Link
 *
 * @brief OS communication link layer.
*/


/**
 * @brief The message type for link message frames.
 *
 * Put MB_FRAME_DT_LINK into payload_type.
 * Put this value into metadata[7:0] to indicate the payload.
 */
enum mb_link_msg_e {
    MB_LINK_MSG_INVALID = 0,
    MB_LINK_MSG_PING = 1,           // ping test, response with pong
    MB_LINK_MSG_PONG = 2,           // response to ping
};

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_LINK_H_ */
