/*
* Copyright 2022 Jetperch LLC
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
 * @brief Joulescope JS220 host API.
 */

#ifndef JS220_HOST_API_H_
#define JS220_HOST_API_H_

/**
 * Port assignments:
 * 0: overhead, administration, & maintenance
 * 1: PubSub
 * 2: Logging
 * 3: Memory: erase/read/write, firmware update (many regions only accessible by bootloader)
 * >=16: IN streaming
 */

#include "jsdrv/version.h"

#define JS220_TOPIC_LENGTH        (32U)
#define JS220_USB_FRAME_LENGTH    (512U)        // High-speed Bulk
#define JS220_PUBLISH_LENGTH_MAX  JS220_USB_FRAME_LENGTH

#define JS220_PROTOCOL_VERSION_MAJOR  1
#define JS220_PROTOCOL_VERSION_MINOR  0
#define JS220_PROTOCOL_VERSION_PATCH  0
#define JS220_PROTOCOL_VERSION_U32  JSDRV_VERSION_ENCODE_U32(JS220_PROTOCOL_VERSION_MAJOR, JS220_PROTOCOL_VERSION_MINOR, JS220_PROTOCOL_VERSION_PATCH)
#define JS220_PROTOCOL_VERSION_STR  JSDRV_VERSION_ENCODE_STR(JS220_PROTOCOL_VERSION_MAJOR, JS220_PROTOCOL_VERSION_MINOR, JS220_PROTOCOL_VERSION_PATCH)

#define JS220_USB_EP_BULK_IN  0x82
#define JS220_USB_EP_BULK_OUT 0x01

#define JS220_TOPIC_CONTROLLER_BASE "c"       // for the USB microcontroller
#define JS220_TOPIC_SENSOR_BASE "s"           // for the sensor FPGA
#define JS220_TOPIC_PING      "c/!ping"
#define JS220_TOPIC_PONG      "c/!pong"


/**
 * @brief Control message bRequest values for VENDOR.DEVICE.
 */
enum js220_ctrl_op_e {
    JS220_CTRL_OP_INVALID = 0,      ///< unsupported, NACK

    /**
     * @brief Host connect to JS220.
     *
     * CTRL IN: returns 1 byte uint8 status: 0 success or error code.
     *
     * Depending upon the JS220 configuration, this operation
     * may power on the sensor, start communication between
     * the sensor and controller microcontroller, and configure
     * the instrument.
     *
     * The wValue field contains several options:
     * - 0: reset the JS220 to default values
     * - 1: keep existing configuration as possible
     *
     * When this operation completes, the JS220 response with
     * JS220_PORT0_OP_CONNECT.  On success, the payload contains
     * the versions for hw, fw, and FPGA.
     *
     * To get the metadata, publish null to "$".
     * To get the retained values, publish null to "?".
     */
    JS220_CTRL_OP_CONNECT = 1,

    /**
     * @brief Gracefully indicate host disconnect.
     *
     * The host software should issue this before disconnecting.
     * The host software should also issue this command
     * just before JS220_CTRL_OP_CONNECT to clear any queued
     * or pending BULK IN data.
     */
    JS220_CTRL_OP_DISCONNECT = 2,
};

/**
 * @brief The available port0 operations over BULK IN and OUT.
 *
 * The first 4-bytes of BULK IN/OUT are js220_frame_hdr_s.
 * The second 4-bytes for port0 are js220_port0_s.
 * The operation determines the remaining payload.
 *
 */
enum js220_port0_op_e {
    /// Not allowed, error.
    JS220_PORT0_OP_INVALID = 0,

    /**
     * @brief Connection response to JS220_CTRL_OP_CONNECT.  [Bulk IN]
     *
     * On success, the payload contains js220_port0_connect_s.
     * On failure, the payload is empty and js220_port0_s.status
     * contains the error code.
     */
    JS220_PORT0_OP_CONNECT = 1,

    /**
     * @brief Echo data to/from the JS220.  [Bulk OUT/IN]
     *
     * When the JS220 receives a BULK OUT JS220_PORT0_OP_ECHO, it
     * immediately creates a BULK IN JS220_PORT0_OP_ECHO with the
     * same js220_port0_s and payload.
     */
    JS220_PORT0_OP_ECHO = 2,

    /**
     * @brief Synchronized UTC time.  [Bulk IN/OUT]
     *
     * The JS220 generates Bulk IN requests periodically.
     * The host should turn around BULK OUT response as quickly as
     * possible with the timestamps filled in.
     *
     * The payload contains 5 int64_t values:
     * - 0 Reserved - save for future use
     * - 1 The time the JS220 sent the TIMESYNC request message, in JS220 ticks.
     * - 2 The time that the host received the TIMESYNC request message, in UTC time.
     * - 3 The time that the host sent the TIMESYNC response message, in UTC time.
     * - 4 The time the JS220 received the TIMESYNC request message, in JS220 ticks.
     *
     * The request populates int64 1 and sets the remainder to 0.
     * The response copies 1 and fills in 2 & 3.
     * The JS220 lower-layers may optionally fill in 4 or leave it unused.
     */
    JS220_PORT0_OP_TIMESYNC = 3,    ///< IN/OUT: perform time sync.
};


/**
 * @brief Port0 payload for JS220_PORT0_OP_CONNECT. [Bulk IN]
 */
struct js220_port0_connect_s {
    uint32_t protocol_version;  // major8.minor8.patch16  (only major should really be used)
    uint32_t app_id;
    uint32_t fw_version;        // major8.minor8.patch16
    uint32_t hw_version;        // major8.minor8.patch16
    uint32_t fpga_version;      // major8.minor8.patch16 (0 if sensor is not powered yet)
};

/**
 * @brief Port0 payload for JS220_PORT0_OP_TIMESYNC. [Bulk IN]
 */
struct js220_port0_timesync_s {
    int64_t rsv_i64;
    uint64_t start_count;
    int64_t utc_recv;
    int64_t utc_send;
    uint64_t end_count;
};

/**
 * @brief The 32-bit frame header.
 *
 * [15:0]   frame_id[15:0], increments with each frame.
 * [24:16]  payload_length[8:0] in bytes, excluding these 4 bytes, 511 bytes max.
 * [29:25]  port_id[4:0]
 * [31:30]  reserved[1:0]
 */
struct js220_frame_hdr_s {
    uint32_t frame_id:16;       ///< The frame identifier that increments with each frame.
    uint32_t length:9;          ///< The length of the payload in bytes.
    uint32_t port_id:5;         ///< The port_id for this message.
    uint32_t rsv:2;             ///< Reserved.  Set to 0.
};

union js220_frame_hdr_u {
    struct js220_frame_hdr_s h;
    uint32_t u32;
};

static inline uint32_t js220_frame_hdr_pack(uint16_t frame_id, uint16_t payload_length, uint8_t port_id) {
    return ((uint32_t) frame_id)
           | ((((uint32_t) payload_length) & 0x1ff) << 16)
           | ((((uint32_t) port_id) & 0x1f) << 25);
}

static inline uint16_t js220_frame_hdr_extract_frame_id(uint32_t hdr) {
    return ((uint16_t) (hdr & 0xffff));
}

static inline uint16_t js220_frame_hdr_extract_length(uint32_t hdr) {
    return ((uint16_t) ((hdr >> 16) & 0x1ff));
}

static inline uint8_t js220_frame_hdr_extract_port_id(uint32_t hdr) {
    return ((uint8_t) ((hdr >> 25) & 0x1f));
}

union js220_port0_payload_u {
    struct js220_port0_connect_s connect;
    struct js220_port0_timesync_s timesync;
};

/// Data structure header for port_id=0 messages
struct js220_port0_header_s {
    uint8_t op;      ///< js220_port0_op_e
    uint8_t status;  ///< 0 or error code
    uint16_t arg;    ///< u16 argument.  Set to 0 if unused.
};

struct js220_port0_msg_s {
    union js220_frame_hdr_u frame_hdr;
    struct js220_port0_header_s port0_hdr;
    union js220_port0_payload_u payload;
};

#define JS220_PORT0_CONNECT_LENGTH (sizeof(struct js220_port0_header_s) + sizeof(struct js220_port0_connect_s))
#define JS220_PORT0_TIMESYNC_LENGTH (sizeof(struct js220_port0_header_s) + sizeof(struct js220_port0_timesync_s))

/// Data structure for port_id=1 messages.
struct js220_publish_s {
    char topic[JS220_TOPIC_LENGTH];
    uint8_t type;   ///< The fbp_union_e data format indicator.
    uint8_t flags;  ///< The fbp_union_flag_e flags.
    uint8_t op;     ///< The application-specific operation.
    uint8_t app;    ///< Application specific data.  If unused, write to 0.
    uint8_t data[]; ///< data follows directly for bin, str.  fbp_union_inner_u for other types.
};

#define JS220_PUBSUB_DATA_LENGTH_MAX (JS220_USB_FRAME_LENGTH - (sizeof(uint32_t) + sizeof(struct js220_publish_s)))

enum js220_port3_op_e {
    JS220_PORT3_OP_NONE             = 0,
    JS220_PORT3_OP_ACK              = 1, // INOUT arg is op, OUT only for READ_DATA.
    JS220_PORT3_OP_ERASE            = 2, // OUT
    JS220_PORT3_OP_WRITE_START      = 3, // OUT
    JS220_PORT3_OP_WRITE_DATA       = 4, // OUT
    JS220_PORT3_OP_WRITE_FINALIZE   = 5, // OUT
    JS220_PORT3_OP_READ_REQ         = 6, // OUT
    JS220_PORT3_OP_READ_DATA        = 7, // IN
    JS220_PORT3_OP_BOOT             = 15, // OUT, arg contains boot_target_e
};

enum js220_port3_region_e {
    JS220_PORT3_REGION_CTRL_UNKNOWN             = 0x00, //
    JS220_PORT3_REGION_CTRL_APP                 = 0x01, // ew
    JS220_PORT3_REGION_CTRL_UPDATER1            = 0x02, // ew
    JS220_PORT3_REGION_CTRL_UPDATER2            = 0x03, // ew
    JS220_PORT3_REGION_CTRL_STORAGE             = 0x04, // ewr
    JS220_PORT3_REGION_CTRL_LOGGING             = 0x05, // r
    JS220_PORT3_REGION_CTRL_APP_CONFIG          = 0x06, // ewr
    JS220_PORT3_REGION_CTRL_BOOTLOADER_CONFIG   = 0x07, // ewr
    JS220_PORT3_REGION_CTRL_PERSONALITY         = 0x08, // r (first 256 bytes only)
    JS220_PORT3_REGION_SENSOR_UNKNOWN           = 0x80, //
    JS220_PORT3_REGION_SENSOR_APP1              = 0x81, // ew
    JS220_PORT3_REGION_SENSOR_APP2              = 0x82, // ew
    JS220_PORT3_REGION_SENSOR_CAL_TRIM          = 0x83, // ewr
    JS220_PORT3_REGION_SENSOR_CAL_ACTIVE        = 0x84, // ewr
    JS220_PORT3_REGION_SENSOR_CAL_FACTORY       = 0x85, // r
    JS220_PORT3_REGION_SENSOR_PERSONALITY       = 0x86, // r (first 256 bytes only)
    JS220_PORT3_REGION_SENSOR_ID                = 0x8f, // r
};

struct js220_port3_header_s {
    uint8_t op;         ///< js220_port3_op_e.
    uint8_t region;     ///< The target region.
    uint8_t status;     ///< 0 or error code.
    uint8_t arg;        ///< The u8 argument.  Set to 0 if unused.

    /**
     * @brief The offset in bytes for data[].
     *
     * For JS220_PORT3_OP_ACK to JS220_PORT3_OP_WRITE_DATA, this
     * holds the highest offset processed by the receiver.
     */
    uint32_t offset;

    /**
     * @brief Length of data in bytes
     *
     * This length is only for the data[] field, and it excludes
     * the header bytes.
     *
     * For JS220_PORT3_OP_WRITE_START, the length of data[] is zero
     * and this field specifies the total write data length.
     *
     * For JS220_PORT3_OP_READ_REQ, the
     * length of data[] is zero and this field specifies the
     * desired maximum read length.
     */
    uint32_t length;
};

#define JS220_PORT3_BUFFER_SIZE (2048U)
#define JS220_PAYLOAD_SIZE_MAX (JS220_USB_FRAME_LENGTH - sizeof(union js220_frame_hdr_u))
#define JS220_PORT3_DATA_SIZE_MAX (JS220_PAYLOAD_SIZE_MAX - sizeof(struct js220_port3_header_s))

struct js220_port3_msg_s {
    union js220_frame_hdr_u frame_hdr;
    struct js220_port3_header_s hdr;
    uint8_t data[JS220_PORT3_DATA_SIZE_MAX];
};

typedef union js220_i128_u {
#if defined(__clang__) || defined(__GNUC__)
    __extension__ __int128 i128;
#endif
    int64_t i64[2];
    uint64_t u64[2];
    uint32_t u32[4];
    uint16_t u16[8];
    uint8_t u8[16];
} js220_i128;

struct js220_statistics_raw_s {
    /**
     * @brief The statistics message identifier.
     *
     * 31:      raw = 1, js220_statistics_api_s uses 0.
     * 30:28    version, only 1 currently supported
     * 27:24    The decimate factor from sample_id to calculated samples = 2.
     * 23:0     block_sample_count - in decimated samples
     */
    uint32_t header;
    uint32_t sample_freq;       ///< The samples per second for *_sample_id (undecimated)
    uint64_t block_sample_id;   ///< First sample in this block's statistics computation.
    uint64_t accum_sample_id;   ///< First sample in the integration statistics computation
    int64_t i_x1;               ///< sum(x)         33Q31 (25Q31 used)
    int64_t i_min;              ///<                33Q31 (5Q31 used)
    int64_t i_max;              ///<                33Q31 (5Q31 used)
    int64_t v_x1;               ///< sum(x)         33Q31 (25Q31 used)
    int64_t v_min;              ///<                33Q31 (5Q31 used)
    int64_t v_max;              ///<                33Q31 (5Q31 used)
    int64_t p_x1;               ///< sum(x)         33Q31 (29Q31 used)
    int64_t p_min;              ///<                33Q31 (9Q27 used)
    int64_t p_max;              ///<                33Q31 (9Q27 used)
    js220_i128 i_x2;            ///< sum(x*x)       66Q62 (29Q62 used)
    js220_i128 i_int;           ///< sum(i)         97Q31 (53Q31 used)
    js220_i128 v_x2;            ///< sum(x*x)       66Q62 (29Q62 used)
    js220_i128 v_int;           ///< reserved = 0
    js220_i128 p_x2;            ///< sum(x*x)       74Q54 (37Q54 used)
    js220_i128 p_int;           ///< sum(p)         97Q31 (58Q31 used)
};


#endif  /* JS220_HOST_API_H_ */
