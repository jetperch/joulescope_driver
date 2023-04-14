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
 * @file
 *
 * @brief Joulescope host driver.
 */

#ifndef JSDRV_INCLUDE_H_
#define JSDRV_INCLUDE_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/union.h"
#include "jsdrv/time.h"
#include <stdint.h>

/**
 * @defgroup jsdrv jsdrv
 *
 * @brief The Joulescope user-space driver
 */

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_api API
 *
 * @brief The Joulescope driver host application interface.
 *
 * This API is based upon a distributed publish-subscribe (PubSub)
 * implementation.  The system is arranged as a hierarchical set
 * of topics which have values.  Devices expose topics that control
 * and configure operation.  Devices also publish the measured data
 * to topics for consumption by the host application.
 *
 * The host code can set topic values using jsdrv_publish().
 * Host code can subscribe to topic value changes using
 * jsdrv_subscribe().  This driver will then call the registered
 * subscriber callback for each change on a matching topic.
 * When the host code calls jsdrv_subscribe() with flags
 * (JSDRV_SFLAG_RETAIN | JSDRV_SFLAG_PUB), then the driver
 * immediately provides the retained values to synchronize
 * the host state.
 *
 * This implementation provides several nice features:
 * - Multiple value data types including integers, floats, str, json, binary
 * - Retained values
 * - Topic metadata for automatically populating user interfaces.
 * - Thread-safe, in-order operation.  The driver provides thread-safe
 *   synchronous publish with timeout.  However, subscriber callbacks
 *   are always invoked from the internal PubSub thread, and the host code
 *   is responsible for resynchronization.
 * - Guaranteed in-order topic traversal for retained messages based
 *   upon creation order.
 * - Device connection options:
 *   - connect and restore default settings
 *   - connect and resume using existing device settings.
 *
 * Topic names use a hierarchical naming convention.  Topics associated
 * with a device  start with a prefix:
 * {p} = {backend}/{model}/{serial_number}
 * - {backend}: 1 character backend identifier 0-9, a-z, A-Z.
 * - {model}: The device model number, such as js220.
 * - {serial_number}: The device serial number string.
 *
 * The rest of the topic is also hierarchical, and the following
 * characters are reserved:
 *
 *     /?#$'"`&@%
 *
 * Topics that start with '_' or '@' are directed only to the
 * central PubSub instance within the driver itself.
 *
 * Topics are presumed to have retained values unless the subtopic
 * starts with the character '!'.
 *
 * Topics with the following suffix characters have special meanings:
 * - '%': metadata request (value should be NULL)
 * - '$': metadata response
 * - '?': query request (value should be NULL)
 * - '&': query response
 * - '#': return code.  Value is i32 return code. 0=success.
 *
 * Note that the topic owner will respond to jsdrv_publish
 * operations with a return code to allow for fully synchronous operation.
 *
 * The metadata values are JSON-formatted strings with the following
 * data structure:
 * - dtype: one of [str, json, bin, f32, f64, u8, u16, u32, u64, i8, i16, i32, i64]
 * - brief: A brief string description (recommended).
 * - detail: A more detailed string description (optional).
 * - default: The recommended default value (optional).
 * - options: A list of options, where each option is each a flat list of:
 *   [value [, alt1 [, ...]]]
 *   The alternates must be given in preference order.  The first value
 *   must be the value as dtype.  The second value alt1
 *   (when provided) is used to automatically populate user
 *   interfaces, and it can be the same as value.  Additional
 *   values will be interpreted as equivalents.
 * - range: The list of [v_min, v_max] or [v_min, v_max, v_step].  Both
 *   v_min and v_max are *inclusive*.  v_step defaults to 1 if omitted.
 * - format: Formatting hints string:
 *   - version: The u32 dtype should be interpreted as major8.minor8.patch16.
 * - flags: A list of flags for this topic.  Options include:
 *   - ro: This topic cannot be updated.
 *   - hide: This topic should not appear in the user interface.
 *   - dev: Developer option that should not be used in production.
 *
 * @{
 */

/// The maximum size for normal PubSub messages
#define JSDRV_PAYLOAD_LENGTH_MAX        (1024U)
/// The header size of jsdrv_stream_signal_s before the data field.
#define JSDRV_STREAM_HEADER_SIZE        (48U)
/// The size of data in jsdrv_stream_signal_s.
#define JSDRV_STREAM_DATA_SIZE          (1024 * 64)    // 64 kB max

/**
 * @defgroup jsdrv_topis Topics
 *
 * @brief Topic characters, commands, suffixes, and prefixes.
 *
 * @{
 */

/**
 * @brief The maximum topic length in bytes.
 *
 * This maximum topic length includes the device prefix.  The firmware on
 * the target devices may have a smaller maximum topic length.
 *
 * The actual maximum topic string length (JSDRV_TOPIC_LENGTH_MAX - 2).
 * This reserves one byte for a suffix character and one byte
 * for the NULL '\\x00' string termination character.
 */
#define JSDRV_TOPIC_LENGTH_MAX  (64U)

/// The maximum string length for each hierarchical topic level.
#define JSDRV_TOPIC_LENGTH_PER_LEVEL (8U)


#define JSDRV_TOPIC_SUFFIX_METADATA_REQ   '%'       ///< Topic suffix for metadata requests (value NULL).
#define JSDRV_TOPIC_SUFFIX_METADATA_RSP   '$'       ///< Topic suffix for metadata responses (value JSON).
#define JSDRV_TOPIC_SUFFIX_QUERY_REQ      '&'       ///< Topic suffix for query requests (value NULL).
#define JSDRV_TOPIC_SUFFIX_QUERY_RSP      '?'       ///< Topic suffix for query responses.
#define JSDRV_TOPIC_SUFFIX_RETURN_CODE    '#'       ///< Topic suffix for return codes (value i32).

/**
 * @brief The prefix for all topics not using retained values.
 *
 * By convention, all topic values are retained unless the
 * subtopic starts with this character.
 */
#define JSDRV_SUBTOPIC_PREFIX_COMMAND     '!'

/// The local topic prefix (handled by host, no distributed pubsub).
#define JSDRV_TOPIC_PREFIX_LOCAL          '_'

/// The driver commad prefix (handled by host, no distributed pubsub).
#define JSDRV_TOPIC_PREFIX_COMMAND        '@'

// driver-level commands
#define JSDRV_MSG_COMMAND_PREFIX_CHAR   '@'             ///< Topic command prefix character
#define JSDRV_MSG_DEVICE_ADD            "@/!add"        ///< Device added: subscribe only (published automatically)
#define JSDRV_MSG_DEVICE_REMOVE         "@/!remove"     ///< Device removed: subscribe only (published automatically)
#define JSDRV_MSG_DEVICE_LIST           "@/list"        ///< Device list: subscribe only comma-separated device list, updated with each add & remove
#define JSDRV_MSG_INITIALIZE            "@/!init"       // CAUTION: internal use only
#define JSDRV_MSG_FINALIZE              "@/!final"      // CAUTION: internal use only
#define JSDRV_MSG_VERSION               "@/version"     ///< Driver version: subscribe only JSDRV version (u32)
#define JSDRV_MSG_TIMEOUT               "@/timeout"     ///< UnhandledDriver version: subscribe only JSDRV version (u32)


// device-specific commands in format {device}/{command}
#define JSDRV_MSG_OPEN                  "@/!open"       ///< Device open: use only with device prefix
#define JSDRV_MSG_CLOSE                 "@/!close"      ///< Device close: use only with device prefix

/** @} */

#define JSDRV_TIMEOUT_MS_DEFAULT       1000             ///< The recommended default timeout
#define JSDRV_TIMEOUT_MS_INIT          5000             ///< The recommended default jsdrv_initialize() timeout.


JSDRV_CPP_GUARD_START


/// opaque context instance.
struct jsdrv_context_s;

/**
 * @brief Function called on topic updates.
 *
 * @param user_data The arbitrary user data.
 * @param topic The topic for this update.
 * @param value The value for this update.
 *
 * This function will be called from the Joulescope driver frontend thread.
 * The function is responsible for performing any resynchronization to
 * an application target thread, if needed.  However, the value only
 * remains valid for the duration of the callback.  Any binary or string
 * data must be copied!
 */
typedef void (*jsdrv_subscribe_fn)(void * user_data, const char * topic, const struct jsdrv_union_s * value);

/**
 * @brief The payload type for jsdrv_union_s.app.
 */
enum jsdrv_payload_type_e {
    JSDRV_PAYLOAD_TYPE_UNION        = 0,    // standard jsdrv_union including string, JSON, and raw binary.
    JSDRV_PAYLOAD_TYPE_STREAM       = 1,    // bin with jsdrv_stream_signal_s
    JSDRV_PAYLOAD_TYPE_STATISTICS   = 2,    // bin with jsdrv_statistics_s
    JSDRV_PAYLOAD_TYPE_BUFFER_INFO  = 3,    // bin with jsdrv_buffer_info_s
    JSDRV_PAYLOAD_TYPE_BUFFER_REQ   = 4,    // bin with jsdrv_buffer_request_s
    JSDRV_PAYLOAD_TYPE_BUFFER_RSP   = 5,    // bin with jsdrv_buffer_response_s
};

/**
 * @brief The element base type for streaming data.
 *
 * @see jsdrv_stream_signal_s
 */
enum jsdrv_element_type_e {
    JSDRV_DATA_TYPE_UNDEFINED = 0,
    JSDRV_DATA_TYPE_INT = 2,
    JSDRV_DATA_TYPE_UINT = 3,
    JSDRV_DATA_TYPE_FLOAT = 4,
};

/**
 * @brief The signal field type for streaming data.
 *
 * @see jsdrv_stream_signal_s
 */
enum jsdrv_field_e {
    JSDRV_FIELD_UNDEFINED = 0,
    JSDRV_FIELD_CURRENT   = 1,
    JSDRV_FIELD_VOLTAGE   = 2,
    JSDRV_FIELD_POWER     = 3,
    JSDRV_FIELD_RANGE     = 4, // 0=current, 1=voltage
    JSDRV_FIELD_GPI       = 5,
    JSDRV_FIELD_UART      = 6,
    JSDRV_FIELD_RAW       = 7,
};

/**
 * @brief A contiguous, uncompressed sample block for a channel.
 */
struct jsdrv_stream_signal_s {
    uint64_t sample_id;                     ///< the starting sample id, which increments by decimate_factor.
    uint8_t field_id;                       ///< jsdrv_field_e
    uint8_t index;                          ///< The channel index within the field.
    uint8_t element_type;                   ///< jsdrv_element_type_e
    uint8_t element_size_bits;              ///< The element size in bits
    uint32_t element_count;                 ///< size of data in elements
    uint32_t sample_rate;                   ///< The frequency for sample_id.
    uint32_t decimate_factor;               ///< The decimation factor from sample_id to data samples.
    struct jsdrv_time_map_s time_map;       ///< The time map between sample_id (before decimate_factor) and UTC.
    uint8_t data[JSDRV_STREAM_DATA_SIZE];   ///< The channel data.
};

/**
 * @brief The payload data structure for statistics updates.
 */
struct jsdrv_statistics_s {
    uint8_t version;             ///< The version, only 1 currently supported
    uint8_t rsv1_u8;             ///< Reserved = 0
    uint8_t rsv2_u8;             ///< Reserved = 0
    uint8_t decimate_factor;     ///< The decimate factor from sample_id to calculated samples = 2.
    uint32_t block_sample_count; ///< Samples used to compute this block, in decimated samples.
    uint32_t sample_freq;        ///< The samples per second for *_sample_id (undecimated)
    uint32_t rsv3_u8;            ///< Reserved = 0
    uint64_t block_sample_id;    ///< First sample in this block's statistics computation.
    uint64_t accum_sample_id;    ///< First sample in the integration statistics computation.
    double i_avg;                ///< The average current over the block.
    double i_std;                ///< The standard deviation of current over the block.
    double i_min;                ///< The minimum current value in the block.
    double i_max;                ///< The maximum current value in the block.
    double v_avg;                ///< The average voltage over the block.
    double v_std;                ///< The standard deviation of voltage over the block.
    double v_min;                ///< The minimum voltage value in the block.
    double v_max;                ///< The maximum voltage value in the block.
    double p_avg;                ///< The average power over the block.
    double p_std;                ///< The standard deviation of power over the block.
    double p_min;                ///< The minimum power value in the block.
    double p_max;                ///< The maximum power value in the block.
    double charge_f64;           ///< The charge (integral of current) from accum_sample_id as a 64-bit float.
    double energy_f64;           ///< The energy (integral of power) from accum_sample_id as a 64-bit float.
    uint64_t charge_i128[2];     ///< The charge (integral of current) from accum_sample_id as a 128-bit signed integer with 2**-31 scale.
    uint64_t energy_i128[2];     ///< The charge (integral of current) from accum_sample_id as a 128-bit signed integer with 2**-31 scale.
    struct jsdrv_time_map_s time_map;  ///< The time map between sample_id and UTC.
};

/**
 * @brief The time specification type.
 */
enum jsdrv_time_type_e {
    JSDRV_TIME_UTC = 0,        ///< Time in i64 34Q30 UTC.  See jsdrv/time.h.
    JSDRV_TIME_SAMPLES = 1,    ///< Time in sample_ids for the corresponding channel.
};

/**
 * @brief A UTC-defined time range.
 *
 * Times are int64 34Q30 in UTC from the epoch.  See jsdrv/time.h.
 */
struct jsdrv_time_range_utc_s {
    int64_t start;                   ///< The time for data[0] (inclusive).
    int64_t end;                     ///< The time for data[-1] (inclusive).
    uint64_t length;                 ///< The number of evenly-spaced entries.
};

/**
 * @brief A sample_id-defined time range.
 *
 * Times are in uint64_t sample_ids for the corresponding channel.
 */
struct jsdrv_time_range_samples_s {
    uint64_t start;                   ///< The time for data[0] (inclusive).
    uint64_t end;                     ///< The time for data[-1] (inclusive).
    uint64_t length;                  ///< The number of evenly-spaced entries.
};

/**
 * @brief The signal buffer information.
 *
 * This structure contains both the size and range.  The size is
 * populated immediately, even if the buffer is still empty.
 * As the buffer contents grows, the buffer range will approach
 * the buffer size.  When the buffer is full, the range will be
 * close to the size.  The range when full is not required to be the
 * entire size to allow for buffering optimizations.
 */
struct jsdrv_buffer_info_s {
    uint8_t version;                        ///< The response format version == 1.
    uint8_t rsv1_u8;                        ///< Reserved, set to 0.
    uint8_t rsv2_u8;                        ///< Reserved, set to 0.
    uint8_t rsv3_u8;                        ///< Reserved, set to 0.
    uint8_t field_id;                       ///< jsdrv_field_e
    uint8_t index;                          ///< The channel index within the field.
    uint8_t element_type;                   ///< jsdrv_element_type_e
    uint8_t element_size_bits;              ///< The element size in bits
    char topic[JSDRV_TOPIC_LENGTH_MAX];     ///< The source topic that provides jsdrv_stream_signal_s.
    int64_t size_in_utc;                    ///< The total buffer size in UTC time.
    uint64_t size_in_samples;               ///< The total buffer size in samples.
    struct jsdrv_time_range_utc_s time_range_utc;          ///< In UTC time.
    struct jsdrv_time_range_samples_s time_range_samples;  ///< In sample time.
    struct jsdrv_time_map_s time_map;       ///< The map between samples and utc time.
};

/**
 * @brief The time range union for buffer requests
 */
union jsdrv_buffer_request_time_range_u {
    struct jsdrv_time_range_utc_s utc;
    struct jsdrv_time_range_samples_s samples;
};

/**
 * @brief Request data from the streaming sample buffer.
 *
 * To make a sample request that returns jsdrv_buffer_sample_response_s,
 * only specify end or length.  Set the unused value to zero.
 *
 * If both end and length are specified, then the request may return
 * jsdrv_buffer_summary_response_s.  However, if the specified increment
 * is less than or equal to one sample duration, then the request
 * will return jsdrv_buffer_sample_response_s.
 *
 * For JSDRV_TIME_UTC with end and length > 1,
 * the time increment between samples is:
 *       time_incr = (time_end - time_start) / (length - 1)
 *
 * Using python, the x-axis time is then:
 *      x = np.linspace(time_start, time_end, length, dtype=np.int64)
 *
 * For JSDRV_TIME_SAMPLES with end and length > 1,
 * the sample increment between samples is:
 *       sample_id_incr = (sample_id_end - sample_id_start) / (length - 1)
 *
 * The buffer implementation may deduplicate requests using
 * the combination rsp_topic and rsp_id.
 */
struct jsdrv_buffer_request_s {
    uint8_t version;                     ///< The request format version == 1.
    int8_t time_type;                    ///< jsdrv_time_type_e
    uint8_t rsv1_u8;                     ///< Reserved, set to 0.
    uint8_t rsv2_u8;                     ///< Reserved, set to 0.
    uint32_t rsv3_u32;                   ///< Reserved, set to 0.
    union jsdrv_buffer_request_time_range_u time;
    char rsp_topic[JSDRV_TOPIC_LENGTH_MAX]; ///< The topic for this response.
    int64_t rsp_id;                         ///< The additional identifier to include in the response.
};

/**
 * @brief The buffer response type.
 */
enum jsdrv_buffer_response_type_e {
    JSDRV_BUFFER_RESPONSE_SAMPLES = 1,   ///< Data contains samples.
    JSDRV_BUFFER_RESPONSE_SUMMARY = 2,   ///< Data contains summary statistics.
};

/**
 * @brief A single summary statistics entry.
 */
struct jsdrv_summary_entry_s {
    float avg;                  ///< The average (mean) over the window.
    float std;                  ///< The standard deviation over the window.
    float min;                  ///< The maximum value over the window.
    float max;                  ///< The minimum value over the window.
};

/**
 * @brief The response to jsdrv_buffer_request_s produced by the memory buffer.
 *
 * The response populates both info.time_range_utc and info.time_range_samples.
 * Both length values are equal, and specify the number of returned data samples.
 * For response_type JSDRV_BUFFER_RESPONSE_SUMMARY, the data is
 * jsdrv_summary_entry_s[info.time_range_utc.length].
 * info.element_type is JSDRV_DATA_TYPE_UNDEFINED and
 * info.element_size_bits is sizeof(jsdrv_summary_entry_s) * 8.
 *
 * For response_type JSDRV_BUFFER_RESPONSE_SAMPLES, the data type depends
 * upon info.element_type and info.element_size_bits.
 */
struct jsdrv_buffer_response_s {
    uint8_t version;                        ///< The response format version == 1.
    uint8_t response_type;                  ///< jsdrv_buffer_response_type_e
    uint8_t rsv1_u8;                        ///< Reserved, set to 0.
    uint8_t rsv2_u8;                        ///< Reserved, set to 0.
    uint32_t rsv3_u32;                      ///< Reserved, set to 0.
    int64_t rsp_id;                         ///< The value provided to jsdrv_buffer_request_s.
    struct jsdrv_buffer_info_s info;        ///< The response information.

    /**
     * @brief The response data.
     *
     * The data type for the response varies.
     * Unfortunately, C does not support flexible arrays in union
     * types.  Your code should cast the data to the appropriate type.
     * Use info.time_range_samples.length for the number of data
     * elements in this response data.
     *
     * For response_type JSDRV_BUFFER_RESPONSE_SUMMARY, the
     * data is jsdrv_summary_entry_s[info.time_range_samples.length].
     *
     * For response_type JSDRV_BUFFER_RESPONSE_SAMPLES, the data
     * is defined by info.element_type and info.element_size_bits.
     */
    uint64_t data[];
};

/// The subscriber flags for jsdrv_subscribe().
enum jsdrv_subscribe_flag_e {
    /// No flags (always 0).
    JSDRV_SFLAG_NONE = 0,
    /// Immediately forward retained PUB and/or METADATA, depending upon JSDRV_PUBSUB_SFLAG_PUB and JSDRV_PUBSUB_SFLAG_METADATA_RSP.
    JSDRV_SFLAG_RETAIN = (1 << 0),
    /// Receive normal topic publish.
    JSDRV_SFLAG_PUB = (1 << 1),
    /// Subscribe to receive metadata requests like "$" and "a/b/$".
    JSDRV_SFLAG_METADATA_REQ = (1 << 2),
    /// Subscribe to receive metadata responses like "a/b/c$.
    JSDRV_SFLAG_METADATA_RSP = (1 << 3),
    /// Subscribe to receive query requests like "?" and "a/b/?".
    JSDRV_SFLAG_QUERY_REQ = (1 << 4),
    /// Subscribe to receive query responses like "a/b/c?".
    JSDRV_SFLAG_QUERY_RSP = (1 << 5),
    /// Subscribe to receive return code messages like "a/b/c#".
    JSDRV_SFLAG_RETURN_CODE = (1 << 6),
};

/// The driver mode for device open.
enum jsdrv_device_open_mode_e {
    /// Restore the device to its default, power-on state.
    JSDRV_DEVICE_OPEN_MODE_DEFAULTS = 0,
    /// Update the driver with the device's existing state.
    JSDRV_DEVICE_OPEN_MODE_RESUME = 1,
    /// Low-level open only, for use by internal tools.
    JSDRV_DEVICE_OPEN_MODE_RAW = 0xFF,
};

/**
 * @brief The initialization argument structure.
 */
struct jsdrv_arg_s {
    const char * topic;          ///< The argument name.
    struct jsdrv_union_s value;  ///< The argument value.
};

/**
 * @brief Initialize the Joulescope driver (synchronous).
 *
 * @param[out] context The Joulescope driver context for future API calls.
 * @param args The initialization arguments or NULL.  The argument list is
 *      terminated with an argument with an empty string for topic.
 * @param timeout_ms This function is always blocking and waits for up to
 *      timeout_ms for the operation to complete.
 *      When 0, use the default timeout [recommended].
 *      When nonzero, override the default timeout.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_initialize(struct jsdrv_context_s ** context,
        const struct jsdrv_arg_s * args, uint32_t timeout_ms);

/**
 * @brief Finalize the Joulescope driver (synchronous).
 *
 * @param context The context from jsdrv_initialize().
 * @param timeout_ms This function is always blocking and waits for up to
 *      timeout_ms for the operation to complete.
 *      When 0, use the default timeout [recommended].
 *      When nonzero, override the default timeout.
 *
 * This function releases all Joulescope driver resources including
 * memory and threads.
 *
 * This function must not be called from the driver frontend thread.
 * Therefore, do not call this function directly from a subscriber
 * callback.
 */
JSDRV_API void jsdrv_finalize(struct jsdrv_context_s * context, uint32_t timeout_ms);

/**
 * @brief Publish a new value.
 *
 * @param context The Joulescope driver context.
 * @param topic The topic to publish.
 * @param value The new topic value.
 * @param timeout_ms When 0, publish asynchronously without awaiting
 *      the result.  When nonzero, block awaiting the return code message.
 * @return 0 or error code.  All calls may return #JSDRV_ERROR_PARAMETER_INVALID.
 *      Each topic may return other error codes.
 */
JSDRV_API int32_t jsdrv_publish(struct jsdrv_context_s * context,
        const char * topic, const struct jsdrv_union_s * value,
        uint32_t timeout_ms);

/**
 * @brief Query a retained value.
 *
 * @param context The Joulescope driver context.
 * @param topic The topic to query.
 * @param[inout] value The topic value.  For string, JSON, and binary values,
 *      the provided value must be initialized with an allocated buffer.
 *      The value will be copied into this buffer or return
 *      #JSDRV_ERROR_TOO_SMALL.  If provided, this buffer will be ignored
 *      for other value types.
 * @param timeout_ms This function is always blocking and waits for up to
 *      timeout_ms for the operation to complete.
 *      When 0, use the default timeout [recommended].
 *      When nonzero, override the default timeout.
 * @return 0 or error code.  All calls may return #JSDRV_ERROR_PARAMETER_INVALID
 *      and #JSDRV_ERROR_TIMED_OUT.
 */
JSDRV_API int32_t jsdrv_query(struct jsdrv_context_s * context,
                              const char * topic, struct jsdrv_union_s * value,
                              uint32_t timeout_ms);

/**
 * @brief Subscribe to topic updates.
 *
 * @param context The Joulescope driver context.
 * @param topic The subscription topic.  The cbk_fn will be called whenever
 *      this topic or a child is updated.
 * @param flags The #jsdrv_subscribe_flag_e bitmap. 0 (#JSDRV_SFLAG_NONE) for no flags.
 * @param cbk_fn The function to call with topic updates.  When called, this function
 *      will be invoked from the Joulescope driver thread.  The function must NOT
 *      call jsdrv_finalize() or jsdrv_wait().
 * @param cbk_user_data The arbitrary data provided to cbk_fn.
 * @param timeout_ms When 0, subscribe asynchronously.  When nonzero, block awaiting
 *      the subscription operation to complete.
 * @return 0 or error code.
 *
 * Synchronous, blocking subscription is most useful when flags contains
 * #JSDRV_SFLAG_RETAIN.  The operation will not complete until cbk_fn()
 * is called with all retained values.
 */
JSDRV_API int32_t jsdrv_subscribe(struct jsdrv_context_s * context, const char * topic, uint8_t flags,
        jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
        uint32_t timeout_ms);

/**
 * @brief Unsubscribe to topic updates.
 *
 * @param context The Joulescope driver context.
 * @param topic The subscription topic for unsubscription.
 * @param cbk_fn The previous subscribed function.
 * @param cbk_user_data The arbitrary data provided to cbk_fn which must match
 *      the value provided to jsdrv_subscribe().
 * @param timeout_ms When 0, unsubscribe asynchronously.
 *      When nonzero, block awaiting the unsubscribe operation to complete.
 * @return 0 or error code.
 * @see jsdrv_subscribe
 * @see jsdrv_unsubscribe_all
 *
 * Most callers will want to use blocking, synchronous unsubscribe.
 * Blocking unsubscribe ensures that cbk_fn() will not be called when this
 * function returns.  With asynchronous unsubscribe, cbk_fn() may be called
 * for some indefinite number of times and duration until the unsubscribe
 * request is handled internally.
 */
JSDRV_API int32_t jsdrv_unsubscribe(struct jsdrv_context_s * context, const char * topic,
        jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
        uint32_t timeout_ms);

/**
 * @brief Unsubscribe from all topic updates (asynchronous).
 *
 * @param context The Joulescope driver context.
 * @param cbk_fn The previous subscribed function.
 * @param cbk_user_data The arbitrary data provided to cbk_fn which must match
 *      the value provided to jsdrv_subscribe().
 * @param timeout_ms When 0, unsubscribe asynchronously.
 *      When nonzero, block awaiting the unsubscribe operation to complete.
 * @return 0 or error code.
 * @see jsdrv_unsubscribe
 *
 * Most callers will want to use blocking, synchronous unsubscribe.
 * See jsdrv_unsubscribe() for details.
 */
JSDRV_API int32_t jsdrv_unsubscribe_all(struct jsdrv_context_s * context,
                                        jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
                                        uint32_t timeout_ms);

/**
 * @brief Open a device.
 *
 * @param context The Joulescope driver context.
 * @param device_prefix The device prefix string.
 * @param mode The #jsdrv_device_open_mode_e.
 * @return 0 or error code.
 *
 * This is a convenience function that wraps a single call to
 * jsdrv_publish() with an i32 value.
 * Language wrappers should not wrap this function and instead
 * call jsdrv_publish() directly.
 */
JSDRV_API int32_t jsdrv_open(struct jsdrv_context_s * context, const char * device_prefix, int32_t mode);

/**
 * @brief Close a device.
 *
 * @param context The Joulescope driver context.
 * @param device_prefix The device prefix string.
 * @return 0 or error code.
 *
 * This is a convenience function that wraps a single call to
 * jsdrv_publish().
 * Language wrappers should not wrap this function and instead
 * call jsdrv_publish() directly.
 */
JSDRV_API int32_t jsdrv_close(struct jsdrv_context_s * context, const char * device_prefix);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_INCLUDE_H_ */
