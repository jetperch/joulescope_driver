/*
 * Copyright 2014-2025 Jetperch LLC
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
#define MB_FRAME_SOF1 ((uint8_t) 0x55)
/// The value for the second start of frame nibble.
#define MB_FRAME_SOF2 ((uint8_t) 0x00)
/// The frame header size in total_bytes.
#define MB_FRAME_HEADER_SIZE (8)
/// The maximum payload length in words.
#define MB_FRAME_PAYLOAD_WORDS_MAX (256)
/// The maximum payload length in words.
#define MB_FRAME_PAYLOAD_BYTES_MAX (MB_FRAME_PAYLOAD_WORDS_MAX * 4)
/// The frame footer size in total_bytes.
#define MB_FRAME_FOOTER_SIZE (4)
/// The frame total maximum data size in bytes
#define MB_FRAME_MAX_SIZE (\
    MB_FRAME_HEADER_SIZE + \
    MB_FRAME_PAYLOAD_MAX_SIZE + \
    MB_FRAME_FOOTER_SIZE)
/// The frame link message (ACK) size in bytes
#define MB_FRAME_LINK_SIZE (8)
#define MB_FRAME_OVERHEAD_SIZE (MB_FRAME_HEADER_SIZE + MB_FRAME_FOOTER_SIZE)
#define MB_FRAME_FRAME_ID_MAX ((1U << 11) - 1U)
#define MB_FRAME_LENGTH_TO_CHECK_LENGTH(x)  (3 + 1 + (uint16_t) (x))
#define MB_FRAME_METADATA_MASK (0x0fffU)

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
    MB_FRAME_FT_NACK = 0x1B,                // nack just frame_id
    MB_FRAME_FT_RESERVED = 0x1D,            // reserved for future use
    MB_FRAME_FT_CONTROL = 0x1E,             // frame_id contains details
};

/**
 * @brief The subtypes for `MB_FRAME_FT_CONTROL`.
 *
 * The sender populates the `frame_id` field with these subtype values.
 * The link layer uses these controls to resync, connect, and gracefully
 * disconnect.
 */
enum mb_frame_control_e {
    /**
     * @brief Invalid operation.
     */
    MB_FRAME_CTRL_INVALID = 0x00,

    /// Reserved control 1.
    MB_FRAME_CTRL_RESERVED_1 = 0x01,

    /**
     * @brief Request link connection.
     *
     * The local instance uses this message to establish or reset
     * a connection with the remote.
     * On success, the receiver moves to the disconnected state and
     * discards all queued messages.  It then replies with
     * MB_FRAME_CTRL_CONNECT_ACK and IDENTITY.
     *
     * The transmitter expects to receive MB_FRAME_CTRL_CONNECT_ACK.
     * It then expects an IDENTITY data frame with MB_FRAME_ST_LINK
     * service type and MB_LINK_MSG_IDENTITY in metadata[7:0]
     * as the first data message with frame_id 0.
     */
    MB_FRAME_CTRL_CONNECT_REQ = 0x02,

    /**
     * @brief Acknowledge link connect.
     *
     * The transmitter expects an IDENTITY data frame with MB_FRAME_ST_LINK
     * service type and MB_LINK_MSG_IDENTITY in metadata[7:0]
     * as the first data message with frame_id 0.
     */
    MB_FRAME_CTRL_CONNECT_ACK = 0x03,

    /**
     * @brief Request a link disconnect.
     *
     * The receiver should reply with MB_FRAME_CTRL_DISCONNECT_ACK.
     * While each side of a connection must handle when the other party
     * becomes unresponsive, this explicit disconnect allows for a graceful
     * disconnection free from warnings or errors.
     *
     * The use of REQ and ACK allows the disconnect requestor to ensure that the
     * receiver has processed the disconnection before tearing down the link.
     * This "flush" functionality is especially useful for communication links
     * where the sender has weak guarantees on message flushing, such as USB.
     *
     * If the requestor does not receive MB_FRAME_CTRL_DISCONNECT_ACK by
     * timeout, it should simply transition to closed with a warning.
     * The requestion should NOT retry as the system is designed to
     * fully recover even without DISCONNECT messages.
     */
    MB_FRAME_CTRL_DISCONNECT_REQ = 0x04,

    /**
     * @brief Acknowledge link disconnect.
     *
     * Upon receiving MB_FRAME_CTRL_DISCONNECT_REQ, it replies with
     * MB_FRAME_CTRL_DISCONNECT_ACK.  This should purge the message queue
     * and prevent new message transmission until a successful
     * MB_FRAME_CTRL_CONNECT_REQ / MB_FRAME_CTRL_RESET_ACK handshake.
     */
    MB_FRAME_CTRL_DISCONNECT_ACK = 0x05,

    // reserved controls 6 through 255
};

/**
 * @brief The service type for data frames.
 *
 * Service types usually use the metadata field for additional
 * payload identification.
 */
enum mb_frame_service_type_e {
    MB_FRAME_ST_INVALID = 0,             ///< reserved, additional differentiation from link frames
    MB_FRAME_ST_LINK = 1,                ///< Link-layer message: see mb/comm/link.h.
    MB_FRAME_ST_TRACE = 2,               ///< Trace messages, see trace documentation.
    MB_FRAME_ST_STDMSG = 3,              ///< Standard message, see mb/stdmsg.h.

    /**
     * @brief Application-specific service type.
     *
     * The application can use metadata to further distinguish the
     * contents of this message.
     */
    MB_FRAME_ST_APP = 0x0f,
};

/**
 * @brief Length check computation constant.
 *
 * This 16-bit constant provides Hamming Distance (HD) of 4 when used
 * for length validation. The algorithm computes:
 * length_check = (length * MB_FRAME_LENGTH_CHECK_CONSTANT) >> MB_FRAME_LENGTH_CHECK_SHIFT
 *
 * This detects all 1, 2, and 3 bit errors, and 97.4% of 4 bit errors.
 * See doc/comm/frame.md for detailed analysis.
 */
#define MB_FRAME_LENGTH_CHECK_CONSTANT ((uint16_t) 0xd8d9)

/**
 * @brief Length check right shift amount.
 *
 * Used with MB_FRAME_LENGTH_CHECK_CONSTANT to extract bits [18:11]
 * of the multiplication result for optimal error detection.
 */
#define MB_FRAME_LENGTH_CHECK_SHIFT (11)

/**
 * @brief Link check computation constant.
 *
 * This 16-bit constant provides Hamming Distance (HD) of 9 when used
 * for link frame validation. The algorithm computes:
 * link_check = link_msg * MB_FRAME_LINK_CHECK_CONSTANT
 *
 * Where link_msg contains bytes 2 & 3 (frame_id and frame_type).
 * See doc/comm/frame.md for detailed analysis.
 */
#define MB_FRAME_LINK_CHECK_CONSTANT ((uint16_t) 0xcba9)

/**
 * @brief The frame header structure.
 *
 * @see doc/frame.md for details.
 */
struct mb_frame_header_s {
    uint8_t sof1;               ///< MB_FRAME_SOF1
    uint8_t sof2;               ///< MB_FRAME_SOF2
    uint8_t frame_id0;          ///< frame_id[7:0]
    uint8_t frame_id1;          ///< 7:3 mb_frame_type_e, 2:0 frame_id[10:8]
    uint8_t length;             ///< The length in u32 - 1
    uint8_t length_check;       ///< The check value for the length field
    uint16_t metadata;          ///< 11:0 arbitrary 12-bit metadata, 15:12 service_type
};

/**
* @brief Compute the length check field.
*
* @param length The length field value, which is ((size + 3) >> 2) - 1.
* @return The value for the length_check field.
*/
static inline uint8_t mb_frame_length_check(uint8_t length) {
    return (uint8_t) ((length * (uint32_t) MB_FRAME_LENGTH_CHECK_CONSTANT) >> MB_FRAME_LENGTH_CHECK_SHIFT);
}

/**
* @brief Compute the link check field.
*
* @param link_msg The link message bytes 2 & 3 (frame_id and frame_type).
* @return The value for the link_check field.
*/
static inline uint32_t mb_frame_link_check(uint16_t link_msg) {
    return MB_FRAME_LINK_CHECK_CONSTANT * (uint32_t) link_msg;
}

/**
 * @brief Construct a new link frame in the provided memory
 *
 * @param frame_type The mb_frame_type_e for the link frame.
 * @param frame_id The frame ID (associated data) for the link frame
 * @param mem[inout]  The u32 memory pointer, which must be at least two works
 */
MB_API void mb_frame_link_construct_mem(uint8_t frame_type, uint16_t frame_id, uint32_t * mem);

/**
 * @brief Construct a new link frame.
 *
 * @param frame_type The mb_frame_type_e for the link frame.
 * @param frame_id The frame ID (associated data) for the link frame
 * @return The newly constructed link frame message.
 */
MB_API struct mb_msg_s * mb_frame_link_construct(uint8_t frame_type, uint16_t frame_id);

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


/**
 * @brief Validate the start of a frame.
 *
 * @param msg The message containing the frame to validate.
 * @return True if msg contains a valid start of frame, false otherwise.
 *
 * This function checks the following:
 *   - SOF1
 *   - SOF2
 *   - for data frames
 *     - validate length_check against length
 *     - ensure frame has room for length, including frame overhead
 *   - for all other frames, link_check
 *
 * This function does NOT do any of these additional checks
 *   - frame_type validation
 *   - frame_check validation for data frames
 *   - msg->size validation against data frame length
 */
MB_API bool mb_frame_is_valid(struct mb_msg_s * msg);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_FRAME_H_ */
