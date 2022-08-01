/*
 * Copyright 2020-2022 Jetperch LLC
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
 * @brief Union type.
 */

#ifndef JSDRV_UNION_TYPE_H__
#define JSDRV_UNION_TYPE_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_union Union value type
 *
 * @brief A generic union value type.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/// The allowed data types.
enum jsdrv_union_e {
    JSDRV_UNION_NULL = 0,   ///< NULL value.  Also used to clear existing value.
    JSDRV_UNION_STR  = 1,   ///< UTF-8 string value, null terminated.
    JSDRV_UNION_JSON = 2,   ///< UTF-8 JSON string value, null terminated.
    JSDRV_UNION_BIN  = 3,   ///< Raw binary value
    JSDRV_UNION_RSV0 = 4,   ///< Reserved, do not use
    JSDRV_UNION_RSV1 = 5,   ///< Reserved, do not use
    JSDRV_UNION_F32  = 6,   ///< 32-bit IEEE 754 floating point
    JSDRV_UNION_F64  = 7,   ///< 64-bit IEEE 754 floating point
    JSDRV_UNION_U8   = 8,   ///< Unsigned 8-bit integer value.
    JSDRV_UNION_U16  = 9,   ///< Unsigned 16-bit integer value.
    JSDRV_UNION_U32  = 10,  ///< Unsigned 32-bit integer value.
    JSDRV_UNION_U64  = 11,  ///< Unsigned 64-bit integer value.
    JSDRV_UNION_I8   = 12,  ///< Signed 8-bit integer value.
    JSDRV_UNION_I16  = 13,  ///< Signed 16-bit integer value.
    JSDRV_UNION_I32  = 14,  ///< Signed 32-bit integer value.
    JSDRV_UNION_I64  = 15,  ///< Signed 64-bit integer value.
};

/**
 * @brief The standardized Joulescope driver union flags.
 *
 * Applications may define custom flags in jsdrv_union_s.app.
 */
enum jsdrv_union_flag_e {
    /// No flags specified.
    JSDRV_UNION_FLAG_NONE = 0,

    /// The PubSub instance should retain this value.
    JSDRV_UNION_FLAG_RETAIN = (1 << 0),

    /// The value points to a const that will remain valid indefinitely.
    JSDRV_UNION_FLAG_CONST = (1 << 1),

    /// The value uses dynamically allocated heap memory that must be freed.
    JSDRV_UNION_FLAG_HEAP_MEMORY = (1 << 7),
};

/// The actual value holder for jsdrv_union_s.
union jsdrv_union_inner_u {
    const char * str;      ///< JSDRV_UNION_STR, JSDRV_UNION_JSON
    const uint8_t * bin;   ///< JSDRV_UNION_BIN
    float f32;             ///< JSDRV_UNION_F32
    double f64;            ///< JSDRV_UNION_F64
    uint8_t u8;            ///< JSDRV_UNION_U8
    uint16_t u16;          ///< JSDRV_UNION_U16
    uint32_t u32;          ///< JSDRV_UNION_U32
    uint64_t u64;          ///< JSDRV_UNION_U64
    int8_t i8;             ///< JSDRV_UNION_I8
    int16_t i16;           ///< JSDRV_UNION_I16
    int32_t i32;           ///< JSDRV_UNION_I32
    int64_t i64;           ///< JSDRV_UNION_I64
};

/// The value holder for all types.
struct jsdrv_union_s {
    uint8_t type;   ///< The jsdrv_union_e data format indicator.
    uint8_t flags;  ///< The jsdrv_union_flag_e flags.
    uint8_t op;     ///< The application-specific operation.
    uint8_t app;    ///< Application specific data.  If unused, write to 0.

    uint32_t size;  ///< payload size for pointer types, including null terminator for strings.

    /// The actual value.
    union jsdrv_union_inner_u value;
};

// Convenience value creation macros
#define jsdrv_union_null() ((struct jsdrv_union_s){.type=JSDRV_UNION_NULL, .op=0, .flags=0, .app=0, .value={.u64=0}, .size=0})
#define jsdrv_union_null_r() ((struct jsdrv_union_s){.type=JSDRV_UNION_NULL, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.u32=0}, .size=0})
#define jsdrv_union_f32(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_F32, .op=0, .flags=0, .app=0, .value={.f32=_value}, .size=0})
#define jsdrv_union_f32_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_F32, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.f32=_value}, .size=0})
#define jsdrv_union_f64(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_F64, .op=0, .flags=0, .app=0, .value={.f64=_value}, .size=0})
#define jsdrv_union_f64_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_F64, .op=0, .app=0, .flags=JSDRV_UNION_FLAG_RETAIN, .value={.f64=_value}, .size=0})
#define jsdrv_union_u8(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U8, .op=0, .flags=0, .app=0, .value={.u8=_value}, .size=0})
#define jsdrv_union_u8_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U8, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.u64=_value}, .size=0})
#define jsdrv_union_u16(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U16, .op=0, .flags=0, .app=0, .value={.u16=_value}, .size=0})
#define jsdrv_union_u16_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U16, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.u64=_value}, .size=0})
#define jsdrv_union_u32(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U32, .op=0, .flags=0, .app=0, .value={.u32=_value}, .size=0})
#define jsdrv_union_u32_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U32, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.u64=_value}, .size=0})
#define jsdrv_union_u64(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U64, .op=0, .flags=0, .app=0, .value={.u64=_value}, .size=0})
#define jsdrv_union_u64_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_U64, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.u64=_value}, .size=0})

#define jsdrv_union_i8(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I8, .op=0, .flags=0, .app=0, .value={.i8=_value}, .size=0})
#define jsdrv_union_i8_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I8, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.i64=_value}, .size=0})
#define jsdrv_union_i16(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I16, .op=0, .flags=0, .app=0, .value={.i16=_value}, .size=0})
#define jsdrv_union_i16_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I16, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.i64=_value}, .size=0})
#define jsdrv_union_i32(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I32, .op=0, .flags=0, .app=0, .value={.i32=_value}, .size=0})
#define jsdrv_union_i32_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I32, .op=0, .flags= JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.i64=_value}, .size=0})
#define jsdrv_union_i64(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I64, .op=0, .flags=0, .app=0, .value={.i64=_value}, .size=0})
#define jsdrv_union_i64_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_I64, .op=0, .flags=JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.i64=_value}, .size=0})

#define jsdrv_union_str(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=0, .flags=0, .app=0, .value={.str=_value}, .size=0})
#define jsdrv_union_cstr(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=0, .flags=JSDRV_UNION_FLAG_CONST, .app=0, .value={.str=_value}, .size=0})
#define jsdrv_union_cstr_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=0, .flags=JSDRV_UNION_FLAG_CONST | JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.str=_value}, .size=0})

#define jsdrv_union_json(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_JSON, .op=0, .flags=0, .app=0, .value={.str=_value}, .size=0})
#define jsdrv_union_cjson(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_JSON, .op=0, .flags=JSDRV_UNION_FLAG_CONST, .app=0, .value={.str=_value}, .size=0})
#define jsdrv_union_cjson_r(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_JSON, .op=0, .flags=JSDRV_UNION_FLAG_CONST | JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.str=_value}, .size=0})

#define jsdrv_union_bin(_value, _size) ((struct jsdrv_union_s){.type=JSDRV_UNION_BIN, .op=0, .flags=0, .app=0, .value={.bin=_value}, .size=_size})
#define jsdrv_union_cbin(_value, _size) ((struct jsdrv_union_s){.type=JSDRV_UNION_BIN, .op=0, .flags=JSDRV_UNION_FLAG_CONST, .app=0, .value={.bin=_value}, .size=_size})
#define jsdrv_union_cbin_r(_value, _size) ((struct jsdrv_union_s){.type=JSDRV_UNION_BIN, .op=0, .flags=JSDRV_UNION_FLAG_CONST | JSDRV_UNION_FLAG_RETAIN, .app=0, .value={.bin=_value}, .size=_size})

/**
 * @brief Check if two values are equal.
 *
 * @param v1 The first value.
 * @param v2 The second value.
 * @return True if equal, false if not equal.
 *
 * This check only compares the type and value.  The types and values must match
 * exactly.  However, if ignores the additional fields
 * [flags, op, app].  Use jsdrv_union_eq_strict() to also compare these fields.
 * Use jsdrv_union_equiv() to more loosely check the value.
 */
JSDRV_API bool jsdrv_union_eq(const struct jsdrv_union_s * v1, const struct jsdrv_union_s * v2);

/**
 * @brief Check if two values are equal.
 *
 * @param v1 The first value.
 * @param v2 The second value.
 * @return True if equal, false if not equal.
 *
 * This check strictly compares every field.
 */
JSDRV_API bool jsdrv_union_eq_exact(const struct jsdrv_union_s * v1, const struct jsdrv_union_s * v2);

/**
 * @brief Check if two values are equivalent.
 *
 * @param v1 The first value.
 * @param v2 The second value.
 * @return True if equal, false if not equal.
 *
 * This check performs type up-conversions to attempt to match the
 * two fields.
 */
JSDRV_API bool jsdrv_union_equiv(const struct jsdrv_union_s * v1, const struct jsdrv_union_s * v2);

/**
 * @brief Widen to the largest-size, compatible numeric type.
 *
 * @param x The value to widen in place.
 * @see jsdrv_union_as_type()
 *
 * This widening conversion preserves float, signed, and unsigned characteristics.
 * This check performs type up-conversions to attempt to match the
 * two fields.
 */
JSDRV_API void jsdrv_union_widen(struct jsdrv_union_s * x);

/**
 * @brief Convert value to a specific type.
 *
 * @param x The value to convert in place.
 * @param type The target type.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_union_as_type(struct jsdrv_union_s * x, uint8_t type);

/**
 * @brief Convert a value to a boolean.
 *
 * @param value The value to convert.  For numeric types, and non-zero value is
 *      presumed true.  String and JSON compare against a case insensitive
 *      string list using jsdrv_cstr_to_bool..  True is ["true", "on", "enable", "enabled", "yes"] and
 *      False is ["false", "off", "disable", "disabled", "no"].  All other values return
 *      an error.
 * @param rv The resulting boolean value.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_union_to_bool(const struct jsdrv_union_s * value, bool * rv);

/**
 * @brief Check if the union contains a pointer type.
 *
 * @param value The union value.
 * @return True is the union contains a pointer type, false otherwise.
 */
static inline bool jsdrv_union_is_type_ptr(const struct jsdrv_union_s * value) {
    switch (value->type) {
        case JSDRV_UNION_STR:  // intentional fall-through
        case JSDRV_UNION_JSON:  // intentional fall-through
        case JSDRV_UNION_BIN:  // intentional fall-through
            return true;
        default:
            return false;
    }
}

/**
 * @brief Convert the type to a user-meaningful string.
 *
 * @param type The jsdrv_union_e type.
 * @return The user-meaningful string representation for the type.
 */
JSDRV_API const char * jsdrv_union_type_to_str(uint8_t type);

/**
 * @brief Convert the value to a user-meaningful string.
 *
 * @param value The value.
 * @param[out] str The string to hold the value.
 * @param str_len The maximum length of str, in bytes.
 * @param opts The formatting options. 0=value only, 1=verbose with type and flags.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_union_value_to_str(const struct jsdrv_union_s * value, char * str, uint32_t str_len, uint32_t opts);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_UNION_TYPE_H__ */
