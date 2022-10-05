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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <math.h>
#include "jsdrv_prv/js220_i128.h"


#define assert_i128_equal(expect, actual) \
    assert_int_equal(expect.u64[0], actual.u64[0]); \
    assert_int_equal(expect.u64[1], actual.u64[1]) \

static void test_add(void ** state) {
    (void) state;
    js220_i128 r;
    r = js220_i128_add((js220_i128) {.u64 = {10, 0}}, (js220_i128) {.u64 = {11, 0}});
    assert_i128_equal(((js220_i128) {.u64 = {21, 0}}), r);

    r = js220_i128_add((js220_i128) {.u64 = {1LLU << 63, 0}}, (js220_i128) {.u64 = {1LLU << 63, 0}});
    assert_i128_equal(((js220_i128) {.u64 = {0, 1}}), r);

    r = js220_i128_add((js220_i128) {.i64 = {-1, -1}}, (js220_i128) {.u64 = {-1, -1}});
    assert_i128_equal(((js220_i128) {.i64 = {-2, -1}}), r);
}

static void test_sub(void ** state) {
    (void) state;
    js220_i128 r;
    r = js220_i128_sub((js220_i128) {.u64 = {10, 0}}, (js220_i128) {.u64 = {11, 0}});
    assert_i128_equal(((js220_i128) {.i64 = {-1, -1}}), r);
}

static void test_to_f64(void ** state) {
    (void) state;
    assert_float_equal(0.0, js220_i128_to_f64((js220_i128) {.u64 = {0, 0}}, 0), 0.0);
    assert_float_equal(0.0, js220_i128_to_f64((js220_i128) {.u64 = {0, 0}}, 31), 0.0);
    assert_float_equal(1.0, js220_i128_to_f64((js220_i128) {.u64 = {0, 0x4000000000000000LLU}}, 126), 0.0);
    assert_float_equal(2.0, js220_i128_to_f64((js220_i128) {.u64 = {0, 0x4000000000000000LLU}}, 125), 0.0);
    assert_float_equal(-1.0, js220_i128_to_f64((js220_i128) {.u64 = {0, 0xC000000000000000LLU}}, 126), 0.0);
    assert_float_equal(1.0, js220_i128_to_f64((js220_i128) {.u64 = {1, 0}}, 0), 0.0);
    assert_float_equal(1.0, js220_i128_to_f64((js220_i128) {.u64 = {2, 0}}, 1), 0.0);
    assert_float_equal(2.0, js220_i128_to_f64((js220_i128) {.u64 = {8, 0}}, 2), 0.0);
    assert_float_equal(1.0, js220_i128_to_f64((js220_i128) {.u64 = {1LLU << 31, 0}}, 31), 0.0);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_add),
            cmocka_unit_test(test_sub),
            cmocka_unit_test(test_to_f64),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
