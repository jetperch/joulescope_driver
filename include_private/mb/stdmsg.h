/*
 * Copyright 2024-2025 Jetperch LLC
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
 * @brief Standard MiniBitty binary message format.
 */

#ifndef MB_STDMSG_H_
#define MB_STDMSG_H_

#include "mb/cdef.h"
#include "mb/version.h"
#include "mb/topic.h"
#include "mb/value.h"

/**
 * @ingroup mb
 * @defgroup mb_stdmsg Standard message format.
 *
 * @brief The standard binary message format used for MiniBitty communications.
 *
 * Defining binary messages formats involves many choices.
 * See doc/dev_choices.md for a discussion.
 *
 * @{
 */

MB_CPP_GUARD_START

/**
 * @brief The standardized message types for Minibitty binary payloads.
 *
 * The payload is defined by the type field in the header.
 * The type field may further use the metadata field to define the
 * contents.  The size of the message should always leave room for the
 * frame pad and 4 byte frame_check values.
 *
 * The stdmsg format is often conveyed inside a comm frame using
 * the MB_FRAME_ST_STDMSG service type.  The stdmsg implementation does not
 * use the comm frame metadata u16.  This allows stdmsgs to be conveyed
 * in other means, such as a published value.  The MiniBitty authors
 * recommend not using the frame metadata.  If you do decide to define the
 * frame metadata for a stdmsg type, you MUST ensure that it is ONLY conveyed
 * using a comm frame.
 *
 * When contained in a mb_msg_s locally, the following fields are defined:
 * - msg->task is the source task
 * - msg->type is MB_VALUE_FRAME, so the message starts with the
 *   comm frame header (8 bytes) and the mb_stdmsg_header_s (8 bytes)
 */
enum mb_stdmsg_type_e {
    MB_STDMSG_INVALID = 0x00,
    MB_STDMSG_PUBLISH = 0x01,         // see mb_stdmsg_publish_s
    MB_STDMSG_PUBSUB_RESPONSE = 0x02, // see mb_stdmsg_response_s
    MB_STDMSG_TIMESYNC_SYNC = 0x03,   // see mb_timesync_sync_v1_s in mb/timesync.h
    MB_STDMSG_TIMESYNC_MAP = 0x04,    // see mb_timesync_map_v1_s in mb/timesync.h
    MB_STDMSG_COMM_STATS = 0x05,      // see mb_comm_stats_v1_s in mb/comm/stats.h
    MB_STDMSG_THROUGHPUT = 0x06,      // raw data

    // todo firmware update

    MB_STDMSG_USER = 0x80,
    MB_STDMSG_USER_LAST = 0xff,
};

/**
 * @brief The standardized message header for Minibitty binary payloads.
 *
 * Provide robust, future-proof communication.
 * The version field provides a simple, generic mechanism to
 * detect version incompatibilities.  The metadata field
 * can often provide an explict way to communicate the exact message format.
 *
 * Size: 8 bytes.
 */
struct mb_stdmsg_header_s {
    uint16_t version;          ///< major.minor version: Minibitty version for type < MB_STDMSG_USER, else app version
    uint8_t type;              ///< The mb_stdmsg_type_e.  Apps define >= MB_STDMSG_USER.
    uint8_t rsv_u8;            ///< Reserved for future use.  Set to 0.
    uint32_t metadata;         ///< Arbitrary metadata defined by type.
};

/**
 * @brief The PubSub publish message.
 *
 * Some subscribers want the full topic and value, particularly comm
 * implementations that support distributed pubsub.
 * When a subscriber subscribes with MB_PUBSUB_SFLAG_MSG, it receives
 * this message.  The message is
 * guaranteed to have room for the frame checksum for zero copy operation.
 * The frame payload is this structure.
 *
 * The frame metadata field is reserved and must be set to 0.
 *
 * The mb_stdmsg_header_s.metadata field is defined as:
 * - metadata[31:24]: Reserved, set to 0
 * - metadata[23:16]: The source pubsub prefix, used for confirmations and errors.
 * - metadata[15:8]: tracking id for confirmation responses, or zero for unconfirmed.
 * - metadata[7:6]: size LSB
 * - metadata[5:4]: reserved, set to 0.
 * - metadata[3:0]: the published value type (pointer values not allowed)
 *
 * This metadata field provides for confirmed delivery and error reporting
 * back to the origin.
 *
 * On error, the target pubsub instance publishes a mb_stdmsg_pubsub_response_s
 * to the {origin_prefix}/!err.
 *
 * Confirmed delivery works similarly.  Whenever the metadata tracking id
 * field is nonzero, the target pubsub instance publishes a
 * mb_stdmsg_pubsub_response_s to {origin_prefix}/!rsp
 *
 * Since the PubSub instance completes the publish operation before
 * publishing the response, the system guarantees that all subscribers
 * with higher task priority than the pubsub instance will have processed
 * the published value.
 */
struct mb_stdmsg_publish_s {
    char topic[MB_TOPIC_LENGTH_MAX];    ///< The null terminated topic string.
    union mb_value_u value;             ///< The published value for topic of type hdr.meta.
};

/**
 * PubSub response message.
 *
 * This stdmsg normally occurs as the value for
 * a MB_STDMSG_PUBLISH stdmsg, since the response may go
 * to another pubsub instance.
 */
struct mb_stdmsg_pubsub_response_s {
    char topic[MB_TOPIC_LENGTH_MAX];    ///< The publish topic from the request.
    uint16_t publish_metadata;          ///< The metadata field from the publish message.
    uint8_t return_code;
};

/**
 * @brief Format a standard message header.
 *
 * @param header The pointer the u32 aligned header.
 * @param type The mb_stdmsg_type_e for this message.
 * @param meta The u32 metadata defined by the type.
 * @return The pointer to the u32 body.
 */
static inline uint32_t * mb_stdmsg_header_init(struct mb_stdmsg_header_s * header, uint8_t type, uint32_t meta) {
    if (type < MB_STDMSG_USER) {
        header->version = MB_VERSION_U16;
    } else {
        header->version = 0;  // todo
    }
    header->type = type;
    header->rsv_u8 = 0;
    header->metadata = meta;
    return (uint32_t *) (header + 1);
}

/**
 * @brief Get the published value size given a publish stdmsg.
 *
 * @param msg The publish stdmsg.
 * @param[out] size The value size in bytes.
 * @return 0 or error code.
 */
uint8_t mb_stdmsg_pubsub_value_size(struct mb_msg_s * msg, uint16_t * size);

MB_CPP_GUARD_END

/** @} */

#endif /* MB_STDMSG_H_ */
