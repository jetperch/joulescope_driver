/*
 * Copyright 2020-2025 Jetperch LLC
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
 * @brief Union value type.
 */

#ifndef MB_VALUE_TYPE_H__
#define MB_VALUE_TYPE_H__

#include "mb/cdef.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup mb
 * @defgroup mb_value Union value type.
 *
 * @brief A generic union value type.
 *
 * @{
 */

MB_CPP_GUARD_START


/**
 * @brief The mb_value_e mask for pointer values.
 */
#define MB_VALUE_PTR_MASK (0x10)

/**
 * @brief The mb_value_e mask for underlying data type ignoring pointer.
 */
#define MB_VALUE_TYPE_MASK (0x0F)

/**
 * @brief The recognized value types.
 *
 * 3:0 : The data type
 * 4   : 1 for pointer types
 */
enum mb_value_e {
    MB_VALUE_NULL = 0x00,       ///< NULL value.  Also used to clear existing value.
    MB_VALUE_STR  = 0x01,       ///< UTF-8 constant string value, null terminated.
    MB_VALUE_JSON = 0x02,       ///< UTF-8 constant JSON string value, null terminated.
    MB_VALUE_BIN  = 0x03,       ///< Raw binary constant value.
    MB_VALUE_STDMSG = 0x04,     ///< mb_stdmsg_s constant binary message, see mb/stdmsg.h
    MB_VALUE_FRAME = 0x05,      ///< Comm frame binary message, see doc/comm/frame.md and mb/comm/frame.h.
    MB_VALUE_F32 = 0x06,        ///< 32-bit IEEE 754 floating point
    MB_VALUE_F64 = 0x07,        ///< 64-bit IEEE 754 floating point
    MB_VALUE_U8 = 0x08,         ///< Unsigned 8-bit integer value.
    MB_VALUE_U16 = 0x09,        ///< Unsigned 16-bit integer value.
    MB_VALUE_U32 = 0x0A,        ///< Unsigned 32-bit integer value.
    MB_VALUE_U64 = 0x0B,        ///< Unsigned 64-bit integer value.
    MB_VALUE_I8 = 0x0C,         ///< Signed 8-bit integer value.
    MB_VALUE_I16 = 0x0D,        ///< Signed 16-bit integer value.
    MB_VALUE_I32 = 0x0E,        ///< Signed 32-bit integer value.
    MB_VALUE_I64 = 0x0F,        ///< Signed 64-bit integer value.

    // const data pointers.  Follow pointer to access data.
    MB_VALUE_RSV10 = 0x10,      ///< Reserved, do not use
    MB_VALUE_STR_PTR  = 0x11,   ///< UTF-8 constant string pointer value, null terminated.
    MB_VALUE_JSON_PTR = 0x12,   ///< UTF-8 constant JSON string pointer value, null terminated.
    MB_VALUE_BIN_PTR  = 0x13,   ///< Raw binary constant pointer value
    MB_VALUE_STDMSG_PTR = 0x14, ///< mb_stdmsg_s pointer value
    MB_VALUE_FRAME_PTR = 0x15,  ///< Comm frame pointer value
};

/// The actual value holder.
union mb_value_u {
    char str[8];               ///< MB_VALUE_STR, size may be larger if dynamically allocated.
    char json[8];              ///< MB_VALUE_JSON, size may be larger if dynamically allocated.
    uint8_t bin[8];            ///< MB_VALUE_BIN, size may be larger if dynamically allocated.
    float f32;                 ///< MB_VALUE_F32
    double f64;                ///< MB_VALUE_F64
    uint8_t u8;                ///< MB_VALUE_UINT 8-bit, size=1
    uint16_t u16;              ///< MB_VALUE_UINT 16-bit, size=2
    uint32_t u32;              ///< MB_VALUE_UINT 32-bit, size=4
    uint64_t u64;              ///< MB_VALUE_UINT 64-bit, size=8
    int8_t i8;                 ///< MB_VALUE_INT 8-bit, size=1
    int16_t i16;               ///< MB_VALUE_INT 16-bit, size=2
    int32_t i32;               ///< MB_VALUE_INT 32-bit, size=4
    int64_t i64;               ///< MB_VALUE_INT 64-bit, size=8
    const char * str_ptr;      ///< MB_VALUE_STR_PTR
    const char * json_ptr;     ///< MB_VALUE_JSON_PTR
    const uint8_t * bin_ptr;   ///< MB_VALUE_BIN_PTR

    uint16_t view_u16[4];      // convenient 16-bit access
    uint32_t view_u32[2];      // convenient 32-bit access
};

#define mb_value_null(_value) ((union mb_value_u){.str_ptr = NULL})
#define mb_value_false() ((union mb_value_u){.u64 = 0})
#define mb_value_true() ((union mb_value_u){.u64 = 1})
#define mb_value_u8(_value) ((union mb_value_u){.u64 = (uint64_t) _value})
#define mb_value_u16(_value) ((union mb_value_u){.u64 = (uint64_t) _value})
#define mb_value_u32(_value) ((union mb_value_u){.u64 = (uint64_t) _value})
#define mb_value_u64(_value) ((union mb_value_u){.u64 = (uint64_t) _value})
#define mb_value_i8(_value) ((union mb_value_u){.i64 = (int64_t) _value})
#define mb_value_i16(_value) ((union mb_value_u){.i64 = (int64_t) _value})
#define mb_value_i32(_value) ((union mb_value_u){.i64 = (int64_t) _value})
#define mb_value_i64(_value) ((union mb_value_u){.i64 = (int64_t) _value})
#define mb_value_f32(_value) ((union mb_value_u){.f32 = _value})
#define mb_value_f64(_value) ((union mb_value_u){.f64 = _value})
#define mb_value_str_ptr(_value) ((union mb_value_u){.str_ptr = _value})
#define mb_value_json_ptr(_value) ((union mb_value_u){.json_ptr = _value})
#define mb_value_bin_ptr(_value) ((union mb_value_u){.bin_ptr = _value})

/**
 * @brief Check if two values are equal.
 *
 * @param t1 The mb_value_e type for the first value.
 * @param v1 The first value.
 * @param s1 The size of the first value.
 * @param t2 The mb_value_e type for the second value.
 * @param v2 The second value.
 * @param s2 The size of the second value.
 * @return True if equal, false if not equal.
 *
 * This check only compares the type and value.  The types and values must match
 * exactly.  However, if ignores the additional fields
 * [flags, op, app].  Use mb_value_eq_strict() to also compare these fields.
 * Use mb_value_equiv() to more loosely check the value.
 */
MB_API bool mb_value_eq(uint8_t t1, const union mb_value_u * v1, uint16_t s1,
                        uint8_t t2, const union mb_value_u * v2, uint16_t s2);

/**
 * @brief Widen a value.
 *
 * @param type The mb_value_e type.
 * @param value The value, which is modified in place.
 * @return The new mb_value_e type.
 */
MB_API uint8_t mb_value_widen(uint8_t type, union mb_value_u * value);

/**
 * @brief Check if two values are equivalent.
 *
 * @param t1 The mb_value_e type for the first value.
 * @param v1 The first value.
 * @param s1 The size of the first value.
 * @param t2 The mb_value_e type for the second value.
 * @param v2 The second value.
 * @param s2 The size of the second value.
 * @return True if equal, false if not equal.
 *
 * This check performs type up-conversions to attempt to match the
 * two fields.
 */
MB_API bool mb_value_equiv(uint8_t t1, const union mb_value_u * v1, uint16_t s1,
                           uint8_t t2, const union mb_value_u * v2, uint16_t s2);

/**
 * @brief Convert a value to a boolean.
 *
 * @param type The mb_value_e type for value.
 * @param value The value to convert.  For numeric types, and non-zero value is
 *      presumed true.  String and JSON compare against a case insensitive
 *      string list using mb_cstr_to_bool.  True is ["true", "on", "enable", "enabled", "yes"] and
 *      False is ["false", "off", "disable", "disabled", "no"].  All other values return
 *      an error.
 * @param rv The resulting boolean value.
 * @return 0 or error code.
 */
MB_API int32_t mb_value_to_bool(uint8_t type, const union mb_value_u * value, bool * rv);

/**
 * @brief Check if the value contains a pointer type.
 *
 * @param value The mb_value_e type.
 * @return True is the type represents a pointer type, false otherwise.
 */
static inline bool mb_value_is_ptr(uint8_t type) {
    switch (type) {
        case MB_VALUE_STR_PTR:  // intentional fall-through
        case MB_VALUE_JSON_PTR:  // intentional fall-through
        case MB_VALUE_BIN_PTR:  // intentional fall-through
            return true;
        default:
            return false;
    }
}

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_VALUE_TYPE_H__ */
