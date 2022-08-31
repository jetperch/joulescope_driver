# Copyright 2022 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, \
    int8_t, int16_t, int32_t, int64_t


DEF JSDRV_TOPIC_LENGTH_MAX = 64
DEF JSDRV_PAYLOAD_LENGTH_MAX    = (1024U)
DEF JSDRV_STREAM_DATA_SIZE      = (1024 * 64)
DEF JSDRV_STREAM_PAYLOAD_LENGTH_MAX = (JSDRV_STREAM_DATA_SIZE - 16)


cdef extern from "jsdrv/union.h":
    enum jsdrv_union_e:
        JSDRV_UNION_NULL = 0  # NULL value.  Also used to clear existing value.
        JSDRV_UNION_STR = 1   # UTF-8 string value, null terminated.
        JSDRV_UNION_JSON = 2  # UTF-8 JSON string value, null terminated.
        JSDRV_UNION_BIN = 3   # Raw binary value
        JSDRV_UNION_RSV0 = 4  # Reserved, do not use
        JSDRV_UNION_RSV1 = 5  # Reserved, do not use
        JSDRV_UNION_F32 = 6   # 32-bit IEEE 754 floating point
        JSDRV_UNION_F64 = 7   # 64-bit IEEE 754 floating point
        JSDRV_UNION_U8 = 8    # Unsigned 8-bit integer value.
        JSDRV_UNION_U16 = 9   # Unsigned 16-bit integer value.
        JSDRV_UNION_U32 = 10  # Unsigned 32-bit integer value.
        JSDRV_UNION_U64 = 11  # Unsigned 64-bit integer value.
        JSDRV_UNION_I8 = 12   # Signed 8-bit integer value.
        JSDRV_UNION_I16 = 13  # Signed 16-bit integer value.
        JSDRV_UNION_I32 = 14  # Signed 32-bit integer value.
        JSDRV_UNION_I64 = 15  # Signed 64-bit integer value.

    enum jsdrv_union_flag_e:
        JSDRV_UNION_FLAG_NONE = 0
        JSDRV_UNION_FLAG_RETAIN = (1 << 0)
        JSDRV_UNION_FLAG_CONST = (1 << 1)

    union jsdrv_union_inner_u:
        const char * str      # JSDRV_UNION_STR, JSDRV_UNION_JSON
        const uint8_t * bin   # JSDRV_UNION_BIN
        float f32             # JSDRV_UNION_F32
        double f64            # JSDRV_UNION_F64
        uint8_t u8            # JSDRV_UNION_U8
        uint16_t u16          # JSDRV_UNION_U16
        uint32_t u32          # JSDRV_UNION_U32
        uint64_t u64          # JSDRV_UNION_U64
        int8_t i8             # JSDRV_UNION_I8
        int16_t i16           # JSDRV_UNION_I16
        int32_t i32           # JSDRV_UNION_I32
        int64_t i64           # JSDRV_UNION_I64

    struct jsdrv_union_s:
        uint8_t type
        uint8_t flags
        uint8_t op
        uint8_t app
        uint32_t size
        jsdrv_union_inner_u value


cdef extern from "jsdrv.h":

    struct jsdrv_context_s
    ctypedef void (*jsdrv_subscribe_fn)(void * user_data, const char * topic, const jsdrv_union_s * value) nogil
    enum jsdrv_payload_type_e:
        JSDRV_PAYLOAD_TYPE_UNION  = 0
        JSDRV_PAYLOAD_TYPE_STREAM = 1
        JSDRV_PAYLOAD_TYPE_STATISTICS = 2
    enum jsdrv_element_type_e:
        JSDRV_DATA_TYPE_UNDEFINED = 0
        JSDRV_DATA_TYPE_FLOAT = 1
        JSDRV_DATA_TYPE_INT = 2
        JSDRV_DATA_TYPE_UINT = 3
    enum jsdrv_field_e:
        JSDRV_FIELD_UNDEFINED = 0
        JSDRV_FIELD_CURRENT = 1
        JSDRV_FIELD_VOLTAGE = 2
        JSDRV_FIELD_POWER = 3
        JSDRV_FIELD_RANGE = 4
        JSDRV_FIELD_GPI = 5
        JSDRV_FIELD_UART = 6
        JSDRV_FIELD_RAW = 7
    struct jsdrv_stream_signal_s:
        uint64_t sample_id
        uint8_t field_id
        uint8_t index
        uint8_t element_type
        uint8_t element_bit_size_pow2
        uint32_t element_count
        uint8_t data[JSDRV_STREAM_PAYLOAD_LENGTH_MAX]
    struct jsdrv_statistics_s:
        uint8_t version
        uint8_t rsv1_u8
        uint8_t rsv2_u8
        uint8_t decimate_factor
        uint32_t block_sample_count
        uint32_t sample_freq
        uint32_t rsv3_u8
        uint64_t block_sample_id
        uint64_t accum_sample_id
        double i_avg
        double i_std
        double i_min
        double i_max
        double v_avg
        double v_std
        double v_min
        double v_max
        double p_avg
        double p_std
        double p_min
        double p_max
        double charge_f64
        double energy_f64
        uint64_t charge_i128[2]
        uint64_t energy_i128[2]
    enum jsdrv_subscribe_flag_e:
        JSDRV_SFLAG_NONE = 0                    # No flags (always 0).
        JSDRV_SFLAG_RETAIN = (1 << 0)           # Immediately forward retained PUB and/or METADATA, depending upon JSDRV_PUBSUB_SFLAG_PUB and JSDRV_PUBSUB_SFLAG_METADATA_RSP.
        JSDRV_SFLAG_PUB = (1 << 1)              # Do not receive normal topic publish.
        JSDRV_SFLAG_METADATA_REQ = (1 << 2)     # Subscribe to receive metadata requests like "$" and "a/b/$".
        JSDRV_SFLAG_METADATA_RSP = (1 << 3)     # Subscribe to receive metadata responses like "a/b/c$.
        JSDRV_SFLAG_QUERY_REQ = (1 << 4)        # Subscribe to receive query requests like "?" and "a/b/?".
        JSDRV_SFLAG_QUERY_RSP = (1 << 5)        # Subscribe to receive query responses like "a/b/c?".
        JSDRV_SFLAG_RETURN_CODE = (1 << 6)      # Subscribe to receive return code messages like "a/b/c#".
    enum jsdrv_device_open_mode_e:
        JSDRV_DEVICE_OPEN_MODE_DEFAULTS = 0
        JSDRV_DEVICE_OPEN_MODE_RESUME = 1
        JSDRV_DEVICE_OPEN_MODE_RAW = 0xFF
    struct jsdrv_arg_s:
        const char * topic  #< The argument name.
        jsdrv_union_s value   #< The argument value.
    int32_t jsdrv_initialize(jsdrv_context_s ** context, const jsdrv_arg_s * args, uint32_t timeout_ms) nogil
    void jsdrv_finalize(jsdrv_context_s * context, uint32_t timeout_ms) nogil
    int32_t jsdrv_publish(jsdrv_context_s * context, const char * topic, const jsdrv_union_s * value, uint32_t timeout_ms) nogil
    int32_t jsdrv_query(jsdrv_context_s * context, const char * topic, jsdrv_union_s * value, uint32_t timeout_ms) nogil
    int32_t jsdrv_subscribe(jsdrv_context_s * context, const char * topic, uint8_t flags, jsdrv_subscribe_fn cbk_fn, void * cbk_user_data, uint32_t timeout_ms) nogil
    int32_t jsdrv_unsubscribe(jsdrv_context_s * context, const char * topic, jsdrv_subscribe_fn cbk_fn, void * cbk_user_data, uint32_t timeout_ms) nogil
    int32_t jsdrv_unsubscribe_all(jsdrv_context_s * context, jsdrv_subscribe_fn cbk_fn, void * cbk_user_data, uint32_t timeout_ms) nogil


cdef extern from "jsdrv/log.h":
    struct jsdrv_log_header_s:
        uint8_t version
        uint8_t level
        uint8_t rsvu8_1
        uint8_t rsvu8_2
        uint32_t line
        uint64_t timestamp

    ctypedef void (*jsdrv_log_recv)(void * user_data, const jsdrv_log_header_s * header,
                                    const char * filename, const char * message) nogil
    void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...) nogil
    int32_t jsdrv_log_register(jsdrv_log_recv fn, void * user_data) nogil
    int32_t jsdrv_log_unregister(jsdrv_log_recv fn, void * user_data) nogil
    void jsdrv_log_level_set(int8_t level) nogil
    int8_t jsdrv_log_level_get() nogil
    void jsdrv_log_initialize() nogil
    void jsdrv_log_finalize() nogil
