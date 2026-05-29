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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "jsdrv_prv/check32.h"


static void test_empty(void ** state) {
    (void) state;
    // The hash of an empty buffer is the initial value.
    assert_int_equal(JSDRV_CHECK32_INITIAL_VALUE,
                     jsdrv_check32_xxhash(NULL, 0));
    assert_int_equal(0x9e3779b1U,
                     jsdrv_check32_xxhash(NULL, 0));
}

static void test_single_zero(void ** state) {
    (void) state;
    uint32_t zero = 0;
    // Pinned value — must stay bit-identical to mb_check32_xxhash().
    assert_int_equal(0x69154bfeU, jsdrv_check32_xxhash(&zero, 1));
}

static void test_single_one(void ** state) {
    (void) state;
    uint32_t one = 1;
    // Pinned reference for a known non-zero input.  Computed by running
    // the algorithm directly; any change here means the algorithm
    // diverged from mb_check32_xxhash.
    uint32_t v = jsdrv_check32_xxhash(&one, 1);
    // Re-run via a local reimplementation to confirm the body matches.
    uint32_t expected = JSDRV_CHECK32_INITIAL_VALUE + 1 * 0x85ebca6bU;
    expected = (expected << 13) | (expected >> 19);
    expected *= 0xc2b2ae35U;
    assert_int_equal(expected, v);
}

static void test_two_words(void ** state) {
    (void) state;
    uint32_t buf[2] = { 0x12345678U, 0xdeadbeefU };
    uint32_t v = jsdrv_check32_xxhash(buf, 2);

    // Hand-rolled reference, identical body.
    uint32_t e = JSDRV_CHECK32_INITIAL_VALUE;
    for (int i = 0; i < 2; ++i) {
        e += buf[i] * 0x85ebca6bU;
        e = (e << 13) | (e >> 19);
        e *= 0xc2b2ae35U;
    }
    assert_int_equal(e, v);
}

static void test_initial_value_constant(void ** state) {
    (void) state;
    // The constant is derived from floor((phi - 1) * 2^32); pin it.
    assert_int_equal(0x9e3779b1U, JSDRV_CHECK32_INITIAL_VALUE);
}

static void test_length_zero_with_data(void ** state) {
    (void) state;
    uint32_t buf[4] = { 1, 2, 3, 4 };
    // Length 0 must ignore the data and return the initial value.
    assert_int_equal(JSDRV_CHECK32_INITIAL_VALUE,
                     jsdrv_check32_xxhash(buf, 0));
}

static void test_record_round_trip(void ** state) {
    (void) state;
    // Simulate the JS320 calibration record pattern: hash 255 words,
    // store the result, then re-hash and compare.
    uint32_t rec[256];
    for (uint32_t i = 0; i < 255; ++i) {
        rec[i] = 0xa5a5a5a5U ^ (i * 0x12345U);
    }
    rec[255] = jsdrv_check32_xxhash(rec, 255);
    assert_int_equal(rec[255], jsdrv_check32_xxhash(rec, 255));
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty),
        cmocka_unit_test(test_single_zero),
        cmocka_unit_test(test_single_one),
        cmocka_unit_test(test_two_words),
        cmocka_unit_test(test_initial_value_constant),
        cmocka_unit_test(test_length_zero_with_data),
        cmocka_unit_test(test_record_round_trip),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
