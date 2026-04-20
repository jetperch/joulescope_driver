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
#include "jsdrv/error_code.h"
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
