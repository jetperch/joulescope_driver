/*
 * SPDX-FileCopyrightText: Copyright 2014-2025 Jetperch LLC
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
 * @brief Communication link layer.
 */

#ifndef MB_COMM_LINK_H_
#define MB_COMM_LINK_H_

#include "mb/cdef.h"
#include "mb/comm/link_sm.h"

MB_CPP_GUARD_START

/**
 * @ingroup mb_comm
 * @defgroup mb_comm_link Link
 *
 * @brief Communication link layer.
*/


/**
 * @brief The message type for link message frames.
 *
 * Put MB_FRAME_DT_LINK into payload_type.
 * Put this value into metadata[7:0] to indicate the payload.
 */
enum mb_link_msg_e {
    MB_LINK_MSG_INVALID = 0,

    /**
     * @brief Identity information exchanged during link connection.
     *
     * @see mb_link_identity_s
     *
     * This message is exchanged during link connection
     * to identify the far end.  A host may then use the identity
     * to connect the correct "driver" for the device.
     */
    MB_LINK_MSG_IDENTITY = 1,

    MB_LINK_MSG_PING = 2,           ///< ping message, respond with pong
    MB_LINK_MSG_PONG = 3,           ///< pong message, response to ping
};

/**
 * @brief The internal comm link task events.
 */
enum mb_comm_link_events_e {
    MB_COMM_LINK_EV_REQ = MB_COMM_LINK_SM_EV_NEXT,  // for task !req subscription
    MB_COMM_LINK_EV_TX_PEND,    // when tx frame_check complete
    MB_COMM_LINK_EV_TX_DONE,    // when io transmit done
    MB_COMM_LINK_EV_RX_POST,    // when on rx frame_check complete
};

/**
 * @brief The link identity information sent on connection.
 */
struct mb_link_identity_s {
    uint32_t mb_version;        ///< The major.minor.patch MiniBitty version.
    uint32_t app_version;       ///< The major.minor.patch application version.
    uint16_t vendor_id;         ///< The personality vendor_id.
    uint16_t product_id;        ///< The personality product_id.
    uint8_t subtype;            ///< The personality subtype.
    char pubsub_prefix;         ///< The pubsub character prefix.
    uint16_t rsv1_u16;          ///< Reserved for future use, write 0.
};

/**
 * @brief Compute the data frame frame_check for this message.
 *
 * @param task_id The task ID for the target comm link.
 * @param msg The message containing the data frame with type MB_VALUE_FRAME.
 *      The caller passes ownership.  This function does not need to call mb_msg_ref_incr().
 *      The msg->event field determines the operation.
 *      See details.
 * @note This function is called from the comm link task.
 *
 * The link implementation uses this function prototype for both transmit
 * and receive.  The operation of this function varies based on the direction.
 * In both cases, this function computes the frame_check value over the
 * msg payload.
 *
 * For transmit, the msg->event MUST be MB_EV_TRANSMIT.  This function then
 * modifies the message to add the frame_check.
 * It MUST set msg->event to MB_COMM_LINK_EV_TX_PEND.
 * For asynchronous completion, send the modified msg to task_id.
 * For synchronous completion, call mb_comm_link_tx_pend().
 *
 * For receive, the msg->event MUST be MB_EV_RECEIVE.  This function
 * compares the computed frame_check value
 * with the existing frame_check value in msg.  If they match, set
 * msg->app = 1.  If they do not match, set msg->app = 0.  It MUST
 * set msg->event to MB_COMM_LINK_EV_RX_POST.
 * For asynchronous completion, send the modified msg to task_id.
 * For synchronous completion, call mb_comm_link_rx_post().
 */
typedef void (*mb_comm_link_frame_check_fn)(uint8_t task_id, struct mb_msg_s * msg);

/**
 * @brief The configuration for mb_comm_link_task.
 */
struct mb_comm_link_task_config_s {
    struct mb_io_s * rx;  ///< Receive IO instance (can be same as TX instance)
    struct mb_io_s * tx;  ///< Transmit IO instance (can be same as RX instance)
    uint8_t rx_task;      ///< task for non-standard receive messages, 0 to drop.
    uint8_t rx_event;     ///< event for non-standard receive messages to rx_task, 0 to drop.
    uint8_t flags;        ///< flags, reserved for now
    uint8_t is_initiator; ///< 1 if initiator role enabled, 0 if listener only role
    mb_comm_link_frame_check_fn tx_frame_check_fn;  ///< tx data frame frame_check computation function
    mb_comm_link_frame_check_fn rx_frame_check_fn;  ///< rx data frame frame_check computation function
    uint8_t tx_window;    ///< maximum number of outstanding transmit data frames, must be power of 2
    uint8_t rx_window;    ///< maximum number of outstanding receive data frames, must be power of 2
    uint32_t tx_timeout;  ///< The transmit timeout in MiniBitty counts.
};

/**
 * @brief The comm link task.
 *
 * Provide mb_comm_link_task_config_s to mb_task_initialize.
 */
void * mb_comm_link_task(void * arg, struct mb_msg_s * msg);

/**
 * @brief Pend a transmit comm link message after frame_check completed.
 *
 * @param task_id The task ID for the target comm link.
 * @param msg The message to pend for transmission.
 *      The caller passes ownership.  This function does not call mb_msg_ref_incr().
 *      msg->event MUST be MB_COMM_LINK_EV_TX_PEND.
 * @note This function MUST be called from the comm link task, usually
 *      by tx_frame_check_fn directly or the MB_COMM_LINK_EV_TX_PEND event.
 */
void mb_comm_link_tx_pend(uint8_t task_id, struct mb_msg_s * msg);

/**
 * @brief Post a received comm link message after frame_check completed.
 *
 * @param task_id The task ID for the target comm link.
 * @param msg The message to post for reception.
 *      The caller passes ownership.  This function does not call mb_msg_ref_incr().
 *      msg->event MUST be MB_COMM_LINK_EV_RX_POST.
 *      msg->app MUST be 1 for frame_check pass or 0 for mismatched.
 * @note This function MUST be called from the comm link task, usually
 *      by rx_frame_check_fn directly or the MB_COMM_LINK_EV_RX_POST event.
 */
void mb_comm_link_rx_post(uint8_t task_id, struct mb_msg_s * msg);

/**
 * @brief Construct the identity message frame for this instance.
 *
 * @param msg The optional message to populate.  If not provided, allocates
 *      a new message of minimal size.
 * @return The identity message.
 * @note This function does not compute frame check, since the algorithm
 *      varies based on the link.  Each link must compute and populate
 *      frame check as needed.
 */
struct mb_msg_s * mb_comm_link_identity(struct mb_msg_s * msg);

// for unit testing only
void mb_comm_link_topic_setup(uint8_t task_id, const char * topic_prefix);

// for unit testing only
void mb_comm_link_topic_teardown(uint8_t task_id, const char * topic_prefix);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_LINK_H_ */
