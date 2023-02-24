/*
 * Copyright 2014-2023 Jetperch LLC
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
#include "jsdrv_prv/statistics.h"


const float F32_0[] = {0.0f, 1.0f, 2.0f, 7.7f, -2.0f, 3.1f, -3.1f, 4.2f, -4.2f, -1.0f, 5.4f, -5.4f, 6.3f, -6.3f, -7.7f};
const double F64_0[] = {0.0, 1.0, 2.0, 3.0, 4.0, 3.4, 9.8, -0.2, 0.5, 5.4, 9.9, 0.3, 0.1, -2.0, -10};



static void test_initialize(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s;
    jsdrv_statistics_reset(&s);
    assert_int_equal(0, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_zero_once(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s;
    jsdrv_statistics_reset(&s);
    jsdrv_statistics_add(&s, 0.0);
    assert_int_equal(1, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.min, 0.0);
    assert_float_equal(0.0, s.max, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_zero_twice(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s;
    jsdrv_statistics_reset(&s);
    jsdrv_statistics_add(&s, 0.0);
    jsdrv_statistics_add(&s, 0.0);
    assert_int_equal(2, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.min, 0.0);
    assert_float_equal(0.0, s.max, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_f64_data(void **state) {
    (void) state;
    double data[] = {0.0, 1.0, 2.0};
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    jsdrv_statistics_reset(&s1);
    for (int i = 0; i < 3; ++i) {
        jsdrv_statistics_add(&s1, data[i]);
    }
    jsdrv_statistics_compute_f64(&s2, data, 3);
    assert_int_equal(3, s1.k);
    assert_int_equal(3, s2.k);
    assert_float_equal(s1.mean, s2.mean, 0.0);
    assert_float_equal(s1.min, s2.min, 0.0);
    assert_float_equal(s1.max, s2.max, 0.0);
    assert_float_equal(s1.s, s2.s, 0.0);
}

static void test_combine_empty(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    struct jsdrv_statistics_accum_s t;
    jsdrv_statistics_reset(&s1);
    jsdrv_statistics_reset(&s2);
    jsdrv_statistics_combine(&t, &s1, &s2);
    assert_int_equal(0, t.k);
}

static void test_combine_a_empty(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    struct jsdrv_statistics_accum_s t;
    jsdrv_statistics_reset(&s1);
    jsdrv_statistics_reset(&s2);
    jsdrv_statistics_add(&s2, 1.0);
    jsdrv_statistics_combine(&t, &s1, &s2);
    assert_int_equal(1, t.k);
    assert_float_equal(1.0, t.mean, 0.0);
}

static void test_combine_b_empty(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    struct jsdrv_statistics_accum_s t;
    jsdrv_statistics_reset(&s1);
    jsdrv_statistics_reset(&s2);
    jsdrv_statistics_add(&s1, 1.0);
    jsdrv_statistics_combine(&t, &s1, &s2);
    assert_int_equal(1, t.k);
    assert_float_equal(1.0, t.mean, 0.0);
}

#define assert_stats_equal(s1, s2) \
    assert_int_equal(s1.k, s2.k); \
    assert_float_equal(s1.mean, s2.mean, 1e-12); \
    assert_float_equal(s1.min, s2.min, 0.0); \
    assert_float_equal(s1.max, s2.max, 0.0); \
    assert_float_equal(s1.s, s2.s, 1e-12);

static void test_combine_f32_run(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s t;
    struct jsdrv_statistics_accum_s ref;
    size_t data_len = sizeof(F32_0) / sizeof(F32_0[0]);
    jsdrv_statistics_compute_f32(&ref, F32_0, data_len);

    jsdrv_statistics_reset(&t);
    for (size_t i = 0; i < data_len; ++i) {
        jsdrv_statistics_add(&t, F32_0[i]);
    }
    assert_int_equal(data_len, ref.k);
    assert_float_equal(0.0f, t.mean, 1e-12);
    assert_float_equal(-7.7f, t.min, 0.0);
    assert_float_equal(7.7f, t.max, 0.0);
    assert_float_equal(320.78f, t.s, 0.0);

    assert_int_equal(ref.k, t.k);
    assert_float_equal(ref.mean, t.mean, 1e-12);
    assert_float_equal(ref.min, t.min, 0.0);
    assert_float_equal(ref.max, t.max, 0.0);
    assert_float_equal(ref.s, t.s, 0.0);
}

static void test_combine_f32_in_two_parts(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    struct jsdrv_statistics_accum_s ref;
    struct jsdrv_statistics_accum_s t;
    size_t data_len = sizeof(F32_0) / sizeof(F32_0[0]);
    jsdrv_statistics_compute_f32(&ref, F32_0, data_len);

    for (size_t i = 0; i < data_len; ++i) {
        jsdrv_statistics_reset(&s1);
        jsdrv_statistics_reset(&s2);
        for (size_t k = 0; k < data_len; ++k) {
            jsdrv_statistics_add((k < i) ? &s1 : &s2, F32_0[k]);
        }
        jsdrv_statistics_combine(&t, &s1, &s2);
        assert_int_equal(data_len, t.k);
        assert_float_equal(ref.mean, t.mean, 1e-12);
        assert_float_equal(ref.min, t.min, 0.0);
        assert_float_equal(ref.max, t.max, 0.0);
        assert_float_equal(ref.s, t.s, 0.0);
    }

    jsdrv_statistics_reset(&s1);
    for (size_t i = 0; i < data_len; ++i) {

    }
}

static void test_combine_f64_in_two_parts(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1;
    struct jsdrv_statistics_accum_s s2;
    struct jsdrv_statistics_accum_s ref;
    struct jsdrv_statistics_accum_s t;
    size_t data_len = sizeof(F64_0) / sizeof(F64_0[0]);
    jsdrv_statistics_compute_f64(&ref, F64_0, data_len);

    for (size_t i = 0; i < data_len; ++i) {
        jsdrv_statistics_reset(&s1);
        jsdrv_statistics_reset(&s2);
        for (size_t k = 0; k < data_len; ++k) {
            jsdrv_statistics_add((k < i) ? &s1 : &s2, F64_0[k]);
        }
        jsdrv_statistics_combine(&t, &s1, &s2);
        assert_int_equal(data_len, t.k);
        assert_float_equal(ref.mean, t.mean, 0.0);
        assert_float_equal(ref.min, t.min, 0.0);
        assert_float_equal(ref.max, t.max, 0.0);
        assert_float_equal(ref.s, t.s, 0.0);
    }
}

static void test_combine_in_place(void **state) {
    (void) state;
    struct jsdrv_statistics_accum_s s1 = {.k=10, .mean=1.0, .s=0.5, .min=-2, .max=4};
    struct jsdrv_statistics_accum_s s2 = {.k=12, .mean=2.0, .s=1.5, .min=-1, .max=5};
    struct jsdrv_statistics_accum_s t1;
    struct jsdrv_statistics_accum_s t2;
    struct jsdrv_statistics_accum_s ref;
    jsdrv_statistics_combine(&ref, &s1, &s2);
    t1 = s1;
    t2 = s2;
    jsdrv_statistics_combine(&t1, &t1, &t2);
    assert_stats_equal(ref, t1);
    t1 = s1;
    t2 = s2;
    jsdrv_statistics_combine(&t2, &t1, &t2);
    assert_stats_equal(ref, t2);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize),
            cmocka_unit_test(test_add_zero_once),
            cmocka_unit_test(test_add_zero_twice),
            cmocka_unit_test(test_add_f64_data),
            cmocka_unit_test(test_combine_empty),
            cmocka_unit_test(test_combine_a_empty),
            cmocka_unit_test(test_combine_b_empty),
            cmocka_unit_test(test_combine_f32_run),
            cmocka_unit_test(test_combine_f32_in_two_parts),
            cmocka_unit_test(test_combine_f64_in_two_parts),
            cmocka_unit_test(test_combine_in_place),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
