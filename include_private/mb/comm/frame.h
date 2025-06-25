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

#ifndef MB_COMM_FRAME_H_
#define MB_COMM_FRAME_H_

#include "mb/cdef.h"
#include <stdint.h>

MB_CPP_GUARD_START

/// The value for the first start of frame byte.
#define MB_FRAMER_SOF1 ((uint8_t) 0x55)
/// The value for the second start of frame nibble.
#define MB_FRAMER_SOF2 ((uint8_t) 0x00)
/// The mask for SOF2
#define MB_FRAMER_SOF2_MASK ((uint8_t) 0xF0)
/// The framer header size in total_bytes.
#define MB_FRAMER_HEADER_SIZE (8)
/// The maximum payload length in words.
#define MB_FRAMER_PAYLOAD_WORDS_MAX (256)
/// The maximum payload length in words.
#define MB_FRAMER_PAYLOAD_BYTES_MAX (MB_FRAMER_PAYLOAD_WORDS_MAX * 4)
/// The framer footer size in total_bytes.
#define MB_FRAMER_FOOTER_SIZE (4)
/// The framer total maximum data size in bytes
#define MB_FRAMER_MAX_SIZE (\
    MB_FRAMER_HEADER_SIZE + \
    MB_FRAMER_PAYLOAD_MAX_SIZE + \
    MB_FRAMER_FOOTER_SIZE)
/// The framer link message (ACK) size in bytes
#define MB_FRAMER_LINK_SIZE (8)
#define MB_FRAMER_OVERHEAD_SIZE (MB_FRAMER_HEADER_SIZE + MB_FRAMER_FOOTER_SIZE)
#define MB_FRAMER_FRAME_ID_MAX ((1U << 11) - 1U)

// forward declaration
struct mb_msg_s;

/**
 * @brief The frame types.
 *
 * The 5-bit frame type values are carefully selected to ensure minimum
 * likelihood that a data frame is detected as an ACK frame.
 */
enum mb_frame_type_e {
    MB_FRAME_FT_DATA = 0x00,                // data frame
    MB_FRAME_FT_ACK_ALL = 0x0F,             // ack all frames through frame_id
    MB_FRAME_FT_ACK_ONE = 0x17,             // ack just frame_id
    MB_FRAME_FT_NACK_FRAME_ID = 0x1B,       // nack just frame_id
    MB_FRAME_FT_RESERVED = 0x1D,            // reserved for future use
    MB_FRAME_FT_CONTROL = 0x1E,             // frame_id contains details
};

/**
 * @brief The subtypes for `MB_FRAME_FT_CONTROL`.
 *
 * The sender populates the `frame_id` field with these subtype values.
 *
 */
enum mb_frame_control_e {
    /**
     * @brief Request link reset and connection.
     *
     * The transmitter uses this message to establish a connection.
     * On success, the receiver discards all queued messages and
     * replies with MB_FRAME_CTRL_RESET_ACK.
     */
    MB_FRAME_CTRL_RESET_REQ = 0x00,

    /**
     * @brief Acknowledge link reset and establish connection.
     *
     * When the receiver is disconnected and receives MB_FRAME_CTRL_RESET_REQ,
     * message, it replies with MB_FRAME_CTRL_RESET_ACK.  After reset
     * acknowledgement, communications begin.
     */
    MB_FRAME_CTRL_RESET_ACK = 0x01,

    /**
     * @brief Request a link disconnect.
     *
     * The receiver should reply with MB_FRAME_CTRL_DISCONNECT_ACK.
     * While each side of a connection must handle when the other party
     * becomes unresponsive, this explicit disconnect allows for a graceful
     * disconnection free from warnings or errors.
     */
    MB_FRAME_CTRL_DISCONNECT_REQ = 0x02,

    /**
     * @brief Acknowledge link disconnect.
     *
     * Upon receiving MB_FRAME_CTRL_DISCONNECT_REQ, it replies with
     * MB_FRAME_CTRL_DISCONNECT_ACK.  This should purge the message queue
     * and prevent new message transmission until a successful
     * MB_FRAME_CTRL_RESET_REQ / MB_FRAME_CTRL_RESET_ACK handshake.
     */
    MB_FRAME_CTRL_DISCONNECT_ACK = 0x03,
};

/**
 * @brief The service type for data frames.
 *
 * Service types usually use the metadata field for additional
 * payload identification.
 */
/// RTOS trace events for performance monitoring.
enum mb_frame_service_type_e {
    MB_FRAME_ST_INVALID = 0,             ///< reserved, additional differentiation from link frames
    MB_FRAME_ST_LINK = 1,                ///< Link-layer message: see os/comm/link.h
    MB_FRAME_ST_TRACE = 2,               ///< Trace messages, see trace documentation

    /**
     * @brief PubSub publish message.
     *
     * This message defines the fields as follows:
     * - metadata[7:0]: mb_value_e
     * - metadata[9:8]: size LSB
     * - metadata[15:10]: reserved, set to 0
     * - payload:
     *   - topic: 32 bytes
     *   - value: N bytes
     */
    MB_FRAME_ST_PUBSUB = 3,

    MB_FRAME_ST_COMM_THROUGHPUT = 4,
};

/**
* @brief Compute the length check field.
*
* @param length The length field value, which is ((size + 3) >> 2) - 1.
* @return The value for the length_check field.
*/
static inline uint8_t mb_frame_length_check(uint8_t length) {
    return (uint8_t) ((length * (uint32_t) 0xd8d9) >> 11);
}

/**
* @brief Compute the link check field.
*
* @param link_msg The link message bytes 2 & 3 (frame_id and frame_type).
* @return The value for the link_check field.
*/
static inline uint32_t mb_frame_link_check(uint16_t link_msg) {
    return 0xcba9U * (uint32_t) link_msg;
}

/**
 * @brief Initialize a frame message header and footer.
 *
 * @param msg The message to initialize.
 * @param service_type The mb_frame_service_type_e.
 * @param payload_size The length of the payload in bytes.
 * @param metadata The value for the metadata field.
 * @return The payload to populate which is guaranteed 64-bit aligned.
 */
MB_API uint32_t * mb_frame_init(struct mb_msg_s * msg, uint8_t service_type, size_t payload_size, uint16_t metadata);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_FRAME_H_ */
