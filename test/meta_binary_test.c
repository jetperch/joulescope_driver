/*
 * Copyright 2025 Jetperch LLC
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "jsdrv_prv/meta_binary.h"
#include "jsdrv_prv/json.h"
#include "jsdrv/error_code.h"
#include <stdio.h>
#include <stdlib.h>
#include "test.inc"


static int topic_count_;

static void on_topic(void * user_data, const char * topic,
                     const char * json_meta) {
    (void) user_data;
    (void) topic;
    (void) json_meta;
    ++topic_count_;
}

static void test_null_blob(void ** state) {
    (void) state;
    assert_int_not_equal(0, meta_binary_parse(NULL, 100, on_topic, NULL));
}

static void test_too_small(void ** state) {
    (void) state;
    uint8_t buf[16] = {0};
    assert_int_not_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
}

static void test_bad_magic(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "XXXXXXXX", 8);  // wrong magic
    // total_size
    uint32_t sz = sizeof(buf);
    memcpy(buf + 12, &sz, 4);
    assert_int_not_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
}

static void test_erased_flash(void ** state) {
    (void) state;
    // Erased flash is all 0xFF — should fail on magic check
    uint8_t buf[256];
    memset(buf, 0xFF, sizeof(buf));
    assert_int_not_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
}

static void test_total_size_exceeds_blob(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = 9999;  // way larger than buf
    memcpy(buf + 12, &total, 4);
    assert_int_not_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
}

// Helper: compute check32 and store in last 4 bytes
static void compute_check32(uint8_t * buf, uint32_t total) {
    uint32_t * u32 = (uint32_t *) buf;
    uint32_t words = total / 4 - 1;
    uint32_t value = 0x9e3779b1U;
    for (uint32_t i = 0; i < words; ++i) {
        value += u32[i] * 0x85ebca6bU;
        value = (value << 13) | (value >> 19);
        value *= 0xc2b2ae35U;
    }
    memcpy(buf + total - 4, &value, 4);
}

static void test_zero_topics(void ** state) {
    (void) state;
    // Valid header, zero topics — should succeed with no callbacks
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    compute_check32(buf, total);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
    assert_int_equal(0, topic_count_);
}

static void test_entry_size_zero(void ** state) {
    (void) state;
    // Valid header, one topic with entry_size=0 — must not loop forever
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t topics = 1;
    memcpy(buf + 16, &topics, 2);
    // Entry at offset 32: all zeros including entry_size=0

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    assert_int_not_equal(0, rc);  // should detect bad entry_size
    assert_int_equal(0, topic_count_);  // should not have called back
}

static void test_entry_size_overflows(void ** state) {
    (void) state;
    // Entry with entry_size larger than remaining blob
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t topics = 1;
    memcpy(buf + 16, &topics, 2);
    // Entry at offset 32: set entry_size to 9999
    uint16_t entry_size = 9999;
    memcpy(buf + 32 + 10, &entry_size, 2);  // offset of entry_size in meta_entry_s

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    assert_int_not_equal(0, rc);
    assert_int_equal(0, topic_count_);
}

static void test_topic_count_too_large(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t topics = 5000;  // absurdly large
    memcpy(buf + 16, &topics, 2);

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    assert_int_not_equal(0, rc);
}

static void test_string_table_offset_out_of_range(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t str_off = 9999;
    memcpy(buf + 18, &str_off, 2);  // string_table_offset

    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    assert_int_not_equal(0, rc);
}

static void test_partially_written_blob(void ** state) {
    (void) state;
    // Header is valid but data area is 0xFF (partially written)
    uint8_t buf[128];
    memset(buf, 0xFF, sizeof(buf));
    // Write valid header
    memset(buf, 0, 32);
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t topics = 2;
    memcpy(buf + 16, &topics, 2);
    uint16_t str_off = 48;
    memcpy(buf + 18, &str_off, 2);
    // Entries at offset 32 are 0xFF garbage

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    // Should abort gracefully (bad entry_size from 0xFFFF)
    assert_int_not_equal(0, rc);
}

static void test_check32_mismatch(void ** state) {
    (void) state;
    // Valid header but corrupted data → check32 won't match
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    // The last 4 bytes are the check value.
    // With all zeros for data (except magic+total_size), the check won't match
    // unless we compute it. Set a wrong check value.
    uint32_t bad_check = 0xDEADBEEF;
    memcpy(buf + 60, &bad_check, 4);

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    assert_int_not_equal(0, rc);
    assert_int_equal(0, topic_count_);
}

static void test_check32_valid_empty(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    compute_check32(buf, total);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
    assert_int_equal(0, topic_count_);
}

static void test_check32_bit_flip(void ** state) {
    (void) state;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    compute_check32(buf, total);

    // Verify valid first
    assert_int_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));

    // Flip one bit → check32 mismatch
    buf[20] ^= 0x01;
    assert_int_not_equal(0, meta_binary_parse(buf, sizeof(buf), on_topic, NULL));
}

static void test_last_page_unpadded(void ** state) {
    (void) state;
    // Simulate a blob where last page wasn't padded with 0xFF.
    // Valid header + 1 entry with valid entry_size, but a second
    // entry falls in the unwritten region (garbage bytes).
    uint8_t buf[256];
    memset(buf, 0, 96);       // first 96 bytes: valid header + entry
    memset(buf + 96, 0xAB, sizeof(buf) - 96);  // rest: garbage (not 0xFF)

    memcpy(buf, "MBtm_1.0", 8);
    uint32_t total = sizeof(buf);
    memcpy(buf + 12, &total, 4);
    uint16_t topics = 5;  // claims 5 entries
    memcpy(buf + 16, &topics, 2);
    uint16_t str_off = 200;
    memcpy(buf + 18, &str_off, 2);

    // First entry at offset 32 with entry_size=48 (valid)
    uint16_t entry_size = 48;
    memcpy(buf + 32 + 10, &entry_size, 2);  // meta_entry_s.entry_size
    // topic_str_offset = 0xFFFF (STR_NONE) so it's skipped
    uint16_t none = 0xFFFF;
    memcpy(buf + 32, &none, 2);

    // Second entry at offset 80 has garbage entry_size (0xABAB)
    // which should trigger the bounds check

    topic_count_ = 0;
    int32_t rc = meta_binary_parse(buf, sizeof(buf), on_topic, NULL);
    // Should abort on garbage entry_size
    assert_int_not_equal(0, rc);
    assert_int_equal(0, topic_count_);
}


static char json_meta_[2048];

static void on_topic_capture(void * user_data, const char * topic,
                             const char * json_meta) {
    (void) user_data;
    (void) topic;
    snprintf(json_meta_, sizeof(json_meta_), "%s", json_meta);
    ++topic_count_;
}

// Mirrors the private layout constants in src/meta_binary.c.
#define META_HEADER_SIZE        (32U)
#define META_ENTRY_HEADER_SIZE  (16U)
#define META_STR_NONE           (0xFFFFU)
#define META_FLAG_RO            (0x01U)
#define META_FLAG_HAS_DEFAULT   (0x08U)
#define META_DTYPE_BOOL         (0x80U)

struct blob_spec_s {
    const char * topic;
    const char * brief;
    const char * detail;
    uint8_t flags;          // META_FLAG_*
    uint8_t dtype;          // 0 for MB_VALUE_U8
    uint8_t option_count;   // 0 for no options block
    const char * option_alt;  // alt string shared by every option
    uint16_t entry_size_override;  // 0 to compute
};

// Build a valid single-topic blob so that the emitted JSON can be inspected.
// Layout: header | entry | string table | check32.  Free the result.
static uint8_t * blob_build(const struct blob_spec_s * spec, uint32_t * blob_size) {
    const char * strings[4] = {spec->topic, spec->brief, spec->detail, spec->option_alt};
    uint16_t str_idx[4];
    uint16_t string_count = 0;
    uint32_t data_sz = 0;
    for (int i = 0; i < 4; ++i) {
        if (strings[i] != NULL) {
            str_idx[i] = string_count++;
            data_sz += (uint32_t) strlen(strings[i]) + 1;
        } else {
            str_idx[i] = META_STR_NONE;
        }
    }

    uint32_t has_default = (spec->flags & META_FLAG_HAS_DEFAULT) ? 8 : 0;
    uint32_t options_off = META_ENTRY_HEADER_SIZE + has_default;
    uint32_t options_sz = spec->option_count ? (4 + spec->option_count * 10) : 0;
    uint32_t entry_size = options_off + options_sz;
    uint32_t st_off = (META_HEADER_SIZE + entry_size + 3) & ~3u;
    uint32_t data_off = st_off + 8 + ((string_count * 2 + 3) & ~3u);
    uint32_t total = (data_off + data_sz + 4 + 3) & ~3u;  // + check32, 4-byte aligned

    uint8_t * blob = calloc(1, total);
    assert_non_null(blob);
    memcpy(blob, "MBtm_1.0", 8);
    memcpy(blob + 12, &total, 4);
    uint16_t u16 = 1;
    memcpy(blob + 16, &u16, 2);                // topic_count
    u16 = (uint16_t) st_off;
    memcpy(blob + 18, &u16, 2);                // string_table_offset

    uint8_t * entry = blob + META_HEADER_SIZE;
    memcpy(entry + 0, &str_idx[0], 2);         // topic_str_offset
    memcpy(entry + 2, &str_idx[1], 2);         // brief_str_offset
    memcpy(entry + 4, &str_idx[2], 2);         // detail_str_offset
    entry[6] = spec->dtype ? spec->dtype : 0x08;  // dtype, default MB_VALUE_U8
    entry[7] = spec->flags;
    u16 = META_STR_NONE;
    memcpy(entry + 8, &u16, 2);                // format_str_offset
    u16 = spec->entry_size_override ? spec->entry_size_override : (uint16_t) entry_size;
    memcpy(entry + 10, &u16, 2);               // entry_size
    if (spec->option_count) {
        u16 = (uint16_t) options_off;
        memcpy(entry + 12, &u16, 2);           // options_offset
        uint8_t * opts = entry + options_off;
        opts[0] = spec->option_count;          // count
        opts[1] = 1;                           // alts_per_option
        for (uint8_t j = 0; j < spec->option_count; ++j) {
            uint8_t * opt = opts + 4 + j * 10;
            opt[0] = j;                        // u8 value, remaining 7 bytes zero
            memcpy(opt + 8, &str_idx[3], 2);   // alt string index
        }
    }

    u16 = string_count;
    memcpy(blob + st_off, &u16, 2);            // string_count
    u16 = (uint16_t) data_sz;
    memcpy(blob + st_off + 2, &u16, 2);        // total_length
    uint16_t char_off = 0;
    for (int i = 0; i < 4; ++i) {
        if (strings[i] == NULL) {
            continue;
        }
        memcpy(blob + st_off + 8 + str_idx[i] * 2, &char_off, 2);
        memcpy(blob + data_off + char_off, strings[i], strlen(strings[i]) + 1);
        char_off = (uint16_t) (char_off + strlen(strings[i]) + 1);
    }

    compute_check32(blob, total);
    *blob_size = total;
    return blob;
}

static int32_t json_count_fn(void * user_data, const struct jsdrv_union_s * token) {
    (void) token;
    ++*((int *) user_data);
    return 0;
}

// The emitted metadata must survive a real JSON parse, not merely look right.
// This is the property the Python binding depends on: json.loads() failures
// are what turned device metadata into None.
static void assert_json_parses(const char * json) {
    int token_count = 0;
    assert_int_equal(0, jsdrv_json_parse(json, json_count_fn, &token_count));
    assert_true(token_count > 0);
}

static void test_control_char_escaping(void ** state) {
    (void) state;
    // One topic whose detail contains a raw newline (multi-line YAML
    // detail blocks store raw newlines in the blob string table).
    // The reconstructed JSON must escape it.
    struct blob_spec_s spec = {.topic = "t", .detail = "a\nb"};
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    json_meta_[0] = '\0';
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"u8\", \"detail\": \"a\\nb\"}", json_meta_);
    assert_null(strchr(json_meta_, '\n'));  // no raw control characters
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_escape_every_class(void ** state) {
    (void) state;
    // Quote and backslash need escaping per RFC 8259, as does every
    // character below 0x20 whether or not it has a short form.
    struct blob_spec_s spec = {
        .topic = "t",
        .brief = "q=\" b=\\",
        .detail = "\n\r\t\b\f\x01\x1f",
    };
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal(
        "{\"dtype\": \"u8\", \"brief\": \"q=\\\" b=\\\\\""
        ", \"detail\": \"\\n\\r\\t\\u0008\\u000c\\u0001\\u001f\"}",
        json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_escape_preserves_utf8(void ** state) {
    (void) state;
    // UTF-8 lead and continuation bytes are >= 0x80 and must pass through
    // unchanged; "180 uA" option labels use the micro sign.
    struct blob_spec_s spec = {.topic = "t", .brief = "180 \xc2\xb5""A"};
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"u8\", \"brief\": \"180 \xc2\xb5""A\"}", json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_option_alt_escaping(void ** state) {
    (void) state;
    // Option alt strings go through the same escape path as brief/detail.
    struct blob_spec_s spec = {
        .topic = "t", .option_count = 2, .option_alt = "a\"b",
    };
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal(
        "{\"dtype\": \"u8\", \"options\": [[0, \"a\\\"b\"], [1, \"a\\\"b\"]]}",
        json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_oversize_metadata_not_published(void ** state) {
    (void) state;
    // Enough options to overrun the 2048-byte JSON buffer.  Truncated JSON
    // is invalid JSON and reintroduces the None-metadata failure, so the
    // topic must be dropped rather than published malformed.
    struct blob_spec_s spec = {
        .topic = "t", .option_count = 200,
        .option_alt = "a reasonably long option label",
    };
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    json_meta_[0] = '\0';
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(0, topic_count_);
    free(blob);
}

static void test_options_overrun_entry(void ** state) {
    (void) state;
    // A corrupt blob claiming more options than the entry holds must not
    // walk the option array past the end of the entry.
    struct blob_spec_s spec = {
        .topic = "t", .option_count = 8, .option_alt = "opt",
    };
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);
    // Shrink entry_size so the declared 8 options no longer fit.
    uint16_t entry_size = META_ENTRY_HEADER_SIZE + 4 + 2 * 10;
    memcpy(blob + META_HEADER_SIZE + 10, &entry_size, 2);
    compute_check32(blob, blob_size);

    topic_count_ = 0;
    json_meta_[0] = '\0';
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    // Only the 2 options the shrunken entry can hold, not the claimed 8.
    assert_string_equal(
        "{\"dtype\": \"u8\", \"options\": [[0, \"opt\"], [1, \"opt\"]]}",
        json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_default_overruns_entry(void ** state) {
    (void) state;
    // HAS_DEFAULT with an entry too small to hold the 8-byte value must not
    // read past the entry.
    struct blob_spec_s spec = {
        .topic = "t",
        .flags = META_FLAG_HAS_DEFAULT,
        .entry_size_override = META_ENTRY_HEADER_SIZE,  // no room for the default
    };
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    json_meta_[0] = '\0';
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_null(strstr(json_meta_, "\"default\""));
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_flags_ro_emitted(void ** state) {
    (void) state;
    struct blob_spec_s spec = {.topic = "t", .flags = META_FLAG_RO};
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"u8\", \"flags\": [\"ro\"]}", json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_dtype_bool_emits_bool_dtype(void ** state) {
    (void) state;
    // bool is stored as u8 with dtype bit 7 set.
    struct blob_spec_s spec = {.topic = "t", .flags = META_FLAG_HAS_DEFAULT,
                               .dtype = 0x08 | META_DTYPE_BOOL};
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"bool\", \"default\": 0}", json_meta_);
    assert_json_parses(json_meta_);
    free(blob);
}

static void test_dtype_bool_bit_ignored_for_non_u8(void ** state) {
    (void) state;
    // Only u8 bools are defined; other base types keep their dtype.
    struct blob_spec_s spec = {.topic = "t", .dtype = 0x0A | META_DTYPE_BOOL};  // u32
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"u32\"}", json_meta_);
    free(blob);
}

static void test_unknown_flag_not_emitted(void ** state) {
    (void) state;
    struct blob_spec_s spec = {.topic = "t", .flags = 0x80};
    uint32_t blob_size = 0;
    uint8_t * blob = blob_build(&spec, &blob_size);

    topic_count_ = 0;
    assert_int_equal(0, meta_binary_parse(blob, blob_size, on_topic_capture, NULL));
    assert_int_equal(1, topic_count_);
    assert_string_equal("{\"dtype\": \"u8\"}", json_meta_);
    free(blob);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_null_blob),
        cmocka_unit_test(test_too_small),
        cmocka_unit_test(test_bad_magic),
        cmocka_unit_test(test_erased_flash),
        cmocka_unit_test(test_total_size_exceeds_blob),
        cmocka_unit_test(test_zero_topics),
        cmocka_unit_test(test_entry_size_zero),
        cmocka_unit_test(test_entry_size_overflows),
        cmocka_unit_test(test_topic_count_too_large),
        cmocka_unit_test(test_string_table_offset_out_of_range),
        cmocka_unit_test(test_check32_mismatch),
        cmocka_unit_test(test_check32_valid_empty),
        cmocka_unit_test(test_check32_bit_flip),
        cmocka_unit_test(test_partially_written_blob),
        cmocka_unit_test(test_last_page_unpadded),
        cmocka_unit_test(test_control_char_escaping),
        cmocka_unit_test(test_escape_every_class),
        cmocka_unit_test(test_escape_preserves_utf8),
        cmocka_unit_test(test_option_alt_escaping),
        cmocka_unit_test(test_oversize_metadata_not_published),
        cmocka_unit_test(test_options_overrun_entry),
        cmocka_unit_test(test_default_overruns_entry),
        cmocka_unit_test(test_flags_ro_emitted),
        cmocka_unit_test(test_dtype_bool_emits_bool_dtype),
        cmocka_unit_test(test_dtype_bool_bit_ignored_for_non_u8),
        cmocka_unit_test(test_unknown_flag_not_emitted),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
