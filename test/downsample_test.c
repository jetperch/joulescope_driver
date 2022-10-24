/*
 * Copyright 2014-2021 Jetperch LLC
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
#include <math.h>
#include "jsdrv_prv/downsample.h"


static void test_float_const(void **state) {
    (void) state;
    assert_float_equal((float) (1 << 30), 0x1p30f, 1);
    assert_float_equal(1.0f / ((float) (1 << 30)), 0x1p-30f, 1);
}

static void test_passthrough_f32(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    float y = 0.0f;
    d = jsdrv_downsample_alloc(1000000, 1000000);
    assert_non_null(d);
    assert_int_equal(1, jsdrv_downsample_decimate_factor(d));
    assert_true(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_float_equal(y, 1.0f, 1e-5);
    assert_true(jsdrv_downsample_add_f32(d, 1001, 2.0f, &y));
    assert_float_equal(y, 2.0, 1e-5);
    jsdrv_downsample_free(d);
}

static void test_basic_x2_f32(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    float y = 0.0f;
    d = jsdrv_downsample_alloc(1000000, 500000);
    assert_non_null(d);
    assert_int_equal(2, jsdrv_downsample_decimate_factor(d));
    assert_false(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_true(jsdrv_downsample_add_f32(d, 1001, 1.0f, &y));
    assert_float_equal(y, 1.0, 1e-5);
    jsdrv_downsample_free(d);
}

static void test_basic_x5_f32(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    float y = 0.0f;
    d = jsdrv_downsample_alloc(1000000, 200000);
    assert_non_null(d);
    assert_int_equal(5, jsdrv_downsample_decimate_factor(d));
    assert_false(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_false(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_false(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_false(jsdrv_downsample_add_f32(d, 1000, 1.0f, &y));
    assert_true(jsdrv_downsample_add_f32(d, 1001, 1.0f, &y));
    assert_float_equal(y, 1.0, 1e-5);
    jsdrv_downsample_free(d);
}

static void test_filt1_f32(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    float y = 0.0f;
    int count = 0;
    uint32_t sample_rate_out = 20000;
    uint32_t decimate = 50;
    d = jsdrv_downsample_alloc(decimate * sample_rate_out, sample_rate_out);
    assert_non_null(d);
    assert_int_equal(decimate, jsdrv_downsample_decimate_factor(d));
    for (uint32_t i = 0; i < (128 * decimate); ++i) {
        if (jsdrv_downsample_add_f32(d, 1000, (float) ((i & 1) + 1), &y)) {
            ++count;
        }
    }
    assert_int_equal(128, count);
    assert_float_equal(y, 1.5, 1e-5);
    jsdrv_downsample_free(d);
}

static void test_filt1_f32_nan(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    float y[128];
    int count = 0;
    uint32_t sample_rate_out = 20000;
    uint32_t decimate = 50;
    d = jsdrv_downsample_alloc(decimate * sample_rate_out, sample_rate_out);
    assert_non_null(d);
    assert_int_equal(decimate, jsdrv_downsample_decimate_factor(d));
    for (uint32_t i = 0; i < (128 * decimate); ++i) {
        if (i == 7) {
            if (jsdrv_downsample_add_f32(d, 1000, NAN, &y[count])) {
                ++count;
            }
        } else if (jsdrv_downsample_add_f32(d, 1000, (float) ((i & 1) + 1), &y[count])) {
            ++count;
        }
    }
    assert_int_equal(128, count);
    assert_true(isnan(y[0]));
    assert_false(isnan(y[32]));
    jsdrv_downsample_free(d);
}

static void test_filt1_u8(void **state) {
    (void) state;
    struct jsdrv_downsample_s * d;
    uint8_t y = 0;
    int count = 0;
    uint32_t sample_rate_out = 20000;
    uint32_t decimate = 50;
    d = jsdrv_downsample_alloc(decimate * sample_rate_out, sample_rate_out);
    assert_non_null(d);
    for (uint32_t i = 0; i < (128 * decimate); ++i) {
        if (jsdrv_downsample_add_u8(d, 1000, (i & 1) << 7, &y)) {
            ++count;
        }
    }
    assert_int_equal(128, count);
    assert_int_equal(y, 0x40);
    jsdrv_downsample_free(d);
}

static void test_invalid_args(void **state) {
    (void) state;
    assert_null(jsdrv_downsample_alloc(1000000, 2000000));
    assert_null(jsdrv_downsample_alloc(1000000, 800000));
    assert_null(jsdrv_downsample_alloc(12000, 1000));  // only factors of 2 & 5 allowed
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_float_const),
            cmocka_unit_test(test_passthrough_f32),
            cmocka_unit_test(test_basic_x2_f32),
            cmocka_unit_test(test_basic_x5_f32),
            cmocka_unit_test(test_filt1_f32),
            cmocka_unit_test(test_filt1_f32_nan),
            cmocka_unit_test(test_filt1_u8),
            cmocka_unit_test(test_invalid_args),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
