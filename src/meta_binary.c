/*
 * Copyright 2026 Jetperch LLC
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
 * @brief Parse pubsub metadata binary blobs into per-topic JSON.
 */

#include "jsdrv_prv/meta_binary.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/log.h"
#include <string.h>
#include <stdio.h>

// Mirror of mb/pubsub_meta.h and mb/value.h for host-side parsing.
#define MB_PUBSUB_META_MAGIC            "MBtm_1.0"
#define MB_PUBSUB_META_STR_NONE         (0xFFFFU)
#define MB_PUBSUB_META_HEADER_SIZE      (32U)
#define MB_PUBSUB_META_ENTRY_HEADER_SIZE (16U)

#define MB_PUBSUB_META_FLAG_RO          0x01
#define MB_PUBSUB_META_FLAG_HIDE        0x02
#define MB_PUBSUB_META_FLAG_DEV         0x04
#define MB_PUBSUB_META_FLAG_HAS_DEFAULT 0x08

#define MB_VALUE_TYPE_MASK 0x0F
#define MB_VALUE_STR    0x01
#define MB_VALUE_JSON   0x02
#define MB_VALUE_BIN    0x03
#define MB_VALUE_STDMSG 0x04
#define MB_VALUE_FRAME  0x05
#define MB_VALUE_F32    0x06
#define MB_VALUE_F64    0x07
#define MB_VALUE_U8     0x08
#define MB_VALUE_U16    0x09
#define MB_VALUE_U32    0x0A
#define MB_VALUE_U64    0x0B
#define MB_VALUE_I8     0x0C
#define MB_VALUE_I16    0x0D
#define MB_VALUE_I32    0x0E
#define MB_VALUE_I64    0x0F

#pragma pack(push, 1)
struct meta_header_s {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t flags;
    uint32_t total_size;
    uint16_t topic_count;
    uint16_t string_table_offset;
    uint16_t dtype_table_offset;
    uint16_t rsv_u16;
    uint32_t header_check;
    uint32_t rsv_pad;
};

struct meta_entry_s {
    uint16_t topic_str_offset;
    uint16_t brief_str_offset;
    uint16_t detail_str_offset;
    uint8_t  dtype;
    uint8_t  flags;
    uint16_t format_str_offset;
    uint16_t entry_size;
    uint16_t options_offset;
    uint16_t range_offset;
};

struct meta_options_header_s {
    uint8_t  count;
    uint8_t  alts_per_option;
    uint16_t rsv_u16;
};

struct meta_range_s {
    uint8_t v_min[8];
    uint8_t v_max[8];
    uint8_t v_step[8];
};

struct meta_string_table_s {
    uint16_t string_count;
    uint16_t total_length;
    uint32_t rsv_u32;
};
#pragma pack(pop)


// String table accessor: returns NULL for STR_NONE
static const char * str_get(const uint8_t * blob, uint32_t blob_size,
                             const struct meta_header_s * hdr,
                             uint16_t str_offset) {
    if (str_offset == MB_PUBSUB_META_STR_NONE) {
        return NULL;
    }
    uint32_t st_off = hdr->string_table_offset;
    if (st_off + sizeof(struct meta_string_table_s) > blob_size) {
        return NULL;
    }
    const struct meta_string_table_s * st =
        (const struct meta_string_table_s *)(blob + st_off);
    if (str_offset >= st->string_count) {
        return NULL;
    }
    // offsets array starts after the header, aligned to 4 bytes
    const uint16_t * offsets = (const uint16_t *)(st + 1);
    uint32_t offsets_end = st_off + sizeof(*st) + ((st->string_count * 2 + 3) & ~3);
    if (offsets_end > blob_size) {
        return NULL;
    }
    uint32_t data_start = offsets_end;
    uint32_t char_off = offsets[str_offset];
    if (data_start + char_off >= blob_size) {
        return NULL;
    }
    return (const char *)(blob + data_start + char_off);
}

static const char * dtype_to_str(uint8_t dtype) {
    switch (dtype & MB_VALUE_TYPE_MASK) {
        case MB_VALUE_STR:    return "str";
        case MB_VALUE_JSON:   return "json";
        case MB_VALUE_BIN:    return "bin";
        case MB_VALUE_STDMSG: return "stdmsg";
        case MB_VALUE_FRAME:  return "frame";
        case MB_VALUE_F32:    return "f32";
        case MB_VALUE_F64:    return "f64";
        case MB_VALUE_U8:     return "u8";
        case MB_VALUE_U16:    return "u16";
        case MB_VALUE_U32:    return "u32";
        case MB_VALUE_U64:    return "u64";
        case MB_VALUE_I8:     return "i8";
        case MB_VALUE_I16:    return "i16";
        case MB_VALUE_I32:    return "i32";
        case MB_VALUE_I64:    return "i64";
        default:              return "bin";
    }
}

// Escape a string for JSON output (minimal: just escape quotes and backslashes)
static int json_escape_str(char * dst, int dst_len, const char * src) {
    int pos = 0;
    for (; *src && pos < dst_len - 2; ++src) {
        if (*src == '"' || *src == '\\') {
            if (pos + 2 >= dst_len - 1) break;
            dst[pos++] = '\\';
        }
        dst[pos++] = *src;
    }
    dst[pos] = '\0';
    return pos;
}

// Format a default value as JSON based on dtype
static int format_default(char * dst, int dst_len, uint8_t dtype, const uint8_t * value_bytes) {
    union {
        uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
        int8_t i8; int16_t i16; int32_t i32; int64_t i64;
        float f32; double f64;
    } v;
    memcpy(&v, value_bytes, 8);

    switch (dtype & MB_VALUE_TYPE_MASK) {
        case MB_VALUE_U8:  return snprintf(dst, dst_len, "%u", (unsigned) v.u8);
        case MB_VALUE_U16: return snprintf(dst, dst_len, "%u", (unsigned) v.u16);
        case MB_VALUE_U32: return snprintf(dst, dst_len, "%u", (unsigned) v.u32);
        case MB_VALUE_U64: return snprintf(dst, dst_len, "%llu", (unsigned long long) v.u64);
        case MB_VALUE_I8:  return snprintf(dst, dst_len, "%d", (int) v.i8);
        case MB_VALUE_I16: return snprintf(dst, dst_len, "%d", (int) v.i16);
        case MB_VALUE_I32: return snprintf(dst, dst_len, "%d", (int) v.i32);
        case MB_VALUE_I64: return snprintf(dst, dst_len, "%lld", (long long) v.i64);
        case MB_VALUE_F32: return snprintf(dst, dst_len, "%g", (double) v.f32);
        case MB_VALUE_F64: return snprintf(dst, dst_len, "%g", v.f64);
        default:           return snprintf(dst, dst_len, "0");
    }
}

// Format a value for options based on dtype
static int format_option_value(char * dst, int dst_len, uint8_t dtype, const uint8_t * value_bytes) {
    return format_default(dst, dst_len, dtype, value_bytes);
}

int32_t meta_binary_parse(
        const uint8_t * blob, uint32_t blob_size,
        meta_binary_on_topic_fn on_topic,
        void * user_data) {
    if (!blob || blob_size < MB_PUBSUB_META_HEADER_SIZE) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }

    const struct meta_header_s * hdr =
        (const struct meta_header_s *) blob;

    if (memcmp(hdr->magic, MB_PUBSUB_META_MAGIC, 8) != 0) {
        JSDRV_LOGW("meta_binary: bad magic");
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    if (hdr->total_size > blob_size) {
        JSDRV_LOGW("meta_binary: total_size %u > blob_size %u",
                    hdr->total_size, blob_size);
        return JSDRV_ERROR_TOO_SMALL;
    }

    uint32_t offset = MB_PUBSUB_META_HEADER_SIZE;
    char json[2048];
    int json_sz = (int) sizeof(json);

    for (uint16_t i = 0; i < hdr->topic_count; ++i) {
        if (offset + MB_PUBSUB_META_ENTRY_HEADER_SIZE > hdr->total_size) {
            break;
        }
        const struct meta_entry_s * entry =
            (const struct meta_entry_s *)(blob + offset);

        const char * topic = str_get(blob, blob_size, hdr, entry->topic_str_offset);
        if (!topic) {
            offset += entry->entry_size;
            continue;
        }

        // Build JSON metadata
        int pos = 0;
        pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "{\"dtype\": \"%s\"",
                        dtype_to_str(entry->dtype));

        const char * brief = str_get(blob, blob_size, hdr, entry->brief_str_offset);
        if (brief) {
            char escaped[512];
            json_escape_str(escaped, sizeof(escaped), brief);
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"brief\": \"%s\"", escaped);
        }

        const char * detail = str_get(blob, blob_size, hdr, entry->detail_str_offset);
        if (detail) {
            char escaped[512];
            json_escape_str(escaped, sizeof(escaped), detail);
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"detail\": \"%s\"", escaped);
        }

        // Default value
        if (entry->flags & MB_PUBSUB_META_FLAG_HAS_DEFAULT) {
            const uint8_t * def_bytes = (const uint8_t *)(entry + 1);
            char def_str[64];
            format_default(def_str, sizeof(def_str), entry->dtype, def_bytes);
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"default\": %s", def_str);
        }

        // Options
        if (entry->options_offset && entry->options_offset < entry->entry_size) {
            const struct meta_options_header_s * opts =
                (const struct meta_options_header_s *)
                ((const uint8_t *)entry + entry->options_offset);
            const uint8_t * opt_data = (const uint8_t *)(opts + 1);
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"options\": [");
            for (uint8_t j = 0; j < opts->count; ++j) {
                if (j > 0) {
                    pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", ");
                }
                // value (8 bytes) + alts_per_option * 2 bytes
                char val_str[64];
                format_option_value(val_str, sizeof(val_str), entry->dtype, opt_data);
                opt_data += 8;

                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "[%s", val_str);
                for (uint8_t a = 0; a < opts->alts_per_option; ++a) {
                    uint16_t alt_idx;
                    memcpy(&alt_idx, opt_data, 2);
                    opt_data += 2;
                    const char * alt = str_get(blob, blob_size, hdr, alt_idx);
                    if (alt) {
                        char escaped[128];
                        json_escape_str(escaped, sizeof(escaped), alt);
                        pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"%s\"", escaped);
                    }
                }
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "]");
            }
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "]");
        }

        // Range
        if (entry->range_offset && entry->range_offset + sizeof(struct meta_range_s) <= entry->entry_size) {
            const struct meta_range_s * range =
                (const struct meta_range_s *)
                ((const uint8_t *)entry + entry->range_offset);
            char min_str[64], max_str[64], step_str[64];
            format_default(min_str, sizeof(min_str), entry->dtype, (const uint8_t *)&range->v_min);
            format_default(max_str, sizeof(max_str), entry->dtype, (const uint8_t *)&range->v_max);
            format_default(step_str, sizeof(step_str), entry->dtype, (const uint8_t *)&range->v_step);
            // Check if step is non-default (not 0 and not 1)
            uint64_t step_u64;
            memcpy(&step_u64, &range->v_step, 8);
            if (step_u64 > 1) {
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0),
                                ", \"range\": [%s, %s, %s]", min_str, max_str, step_str);
            } else {
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0),
                                ", \"range\": [%s, %s]", min_str, max_str);
            }
        }

        // Format hint
        const char * format = str_get(blob, blob_size, hdr, entry->format_str_offset);
        if (format) {
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"format\": \"%s\"", format);
        }

        // Flags
        uint8_t flags = entry->flags & ~MB_PUBSUB_META_FLAG_HAS_DEFAULT;
        if (flags) {
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), ", \"flags\": [");
            int first = 1;
            if (flags & MB_PUBSUB_META_FLAG_RO) {
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "\"ro\"");
                first = 0;
            }
            if (flags & MB_PUBSUB_META_FLAG_HIDE) {
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "%s\"hide\"", first ? "" : ", ");
                first = 0;
            }
            if (flags & MB_PUBSUB_META_FLAG_DEV) {
                pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "%s\"dev\"", first ? "" : ", ");
            }
            pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "]");
        }

        pos += snprintf(json + pos, (json_sz - pos > 0 ? (size_t)(json_sz - pos) : 0), "}");

        if (on_topic) {
            on_topic(user_data, topic, json);
        }

        offset += entry->entry_size;
    }

    return 0;
}
