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
#include "jsdrv_prv/sample_buffer_f32.h"
#include "jsdrv_prv/cdef.h"

static void test_add_one(void **state) {
    (void) state;
    struct sbuf_f32_s b;
    sbuf_f32_clear(&b);
    float data[1] = {1.0f};
    sbuf_f32_add(&b, 0, data, 1);
    assert_int_equal(1, sbuf_f32_length(&b));
    assert_float_equal(1.0f, b.buffer[0], 1e-7);
    assert_int_equal(2, b.head_sample_id);
}

static void test_add_one_skip(void **state) {
    (void) state;
    struct sbuf_f32_s b;
    sbuf_f32_clear(&b);
    float data[1] = {1.0f};
    sbuf_f32_add(&b, 2, data, 1);
    assert_int_equal(2, sbuf_f32_length(&b));
    assert_true(isnan(b.buffer[0]));
    assert_float_equal(1.0f, b.buffer[1], 1e-7);
    assert_int_equal(4, b.head_sample_id);
}

static void test_add_one_duplicate(void **state) {
    (void) state;
    struct sbuf_f32_s b;
    sbuf_f32_clear(&b);
    float data[1] = {1.0f};
    sbuf_f32_add(&b, 0, data, 1);
    sbuf_f32_add(&b, 0, data, 1);
    assert_int_equal(1, sbuf_f32_length(&b));
    assert_float_equal(1.0f, b.buffer[0], 1e-7);
    assert_int_equal(2, b.head_sample_id);
}

static void test_add_wrap(void **state) {
    (void) state;
    struct sbuf_f32_s b;
    sbuf_f32_clear(&b);
    float data[SAMPLE_BUFFER_LENGTH / 2];
    size_t k = 0;
    for (int j = 0; j < 3; ++j) {
        size_t sample_id = k * 2;
        for (size_t i = 0; i < JSDRV_ARRAY_SIZE(data); ++i) {
            data[i] = (float) k++;
        }
        sbuf_f32_add(&b, sample_id, data, JSDRV_ARRAY_SIZE(data));
    }
    assert_int_equal(SAMPLE_BUFFER_LENGTH - 1, sbuf_f32_length(&b));
    uint32_t p = b.head;
    for (size_t i = 0; i < (SAMPLE_BUFFER_LENGTH - 1); i++) {
        p = (p - 1) & SAMPLE_BUFFER_MASK;
        assert_float_equal(--k, b.buffer[p], 1e-7);
    }
}

static void test_mult(void **state) {
    (void) state;
    struct sbuf_f32_s r;
    struct sbuf_f32_s s1;
    struct sbuf_f32_s s2;
    sbuf_f32_clear(&s1);
    sbuf_f32_clear(&s2);
    sbuf_f32_clear(&r);
    float f1[SAMPLE_BUFFER_LENGTH / 2];
    float f2[SAMPLE_BUFFER_LENGTH / 2];
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(f1); ++i) {
        f1[i] = (float) i;
        f2[i] = (float) (2 * i + 1);
    }
    sbuf_f32_add(&s1, 0, f1, JSDRV_ARRAY_SIZE(f1));
    sbuf_f32_add(&s2, 0, f2, JSDRV_ARRAY_SIZE(f2));
    sbuf_f32_mult(&r, &s1, &s2);
    assert_int_equal(JSDRV_ARRAY_SIZE(f1), sbuf_f32_length(&r));
    assert_int_equal(0, sbuf_f32_length(&s1));
    assert_int_equal(0, sbuf_f32_length(&s2));
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(f1); i++) {
        assert_float_equal(i + 2 * i * i, r.buffer[i], 1e-7);
    }
    assert_int_equal(0, r.msg_sample_id);
}

static void test_mult_no_overlap(void **state) {
    (void) state;
    struct sbuf_f32_s r;
    struct sbuf_f32_s s1;
    struct sbuf_f32_s s2;
    float f1[] = {10.0f};
    float f2[] = {11.0f};
    sbuf_f32_clear(&s1);
    sbuf_f32_clear(&s2);
    sbuf_f32_clear(&r);
    sbuf_f32_add(&s1, 0, f1, 1);
    sbuf_f32_add(&s2, s2.sample_id_decimate * (SAMPLE_BUFFER_LENGTH - 1), f2, 1);
    sbuf_f32_mult(&r, &s1, &s2);
    assert_int_equal(0, sbuf_f32_length(&r));
}

static void test_mult_some_overlap(void **state) {
    (void) state;
    struct sbuf_f32_s r;
    struct sbuf_f32_s s1;
    struct sbuf_f32_s s2;
    float f1[] = {10.0f, 11.0f};
    float f2[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
    sbuf_f32_clear(&s1);
    sbuf_f32_clear(&s2);
    sbuf_f32_clear(&r);
    sbuf_f32_add(&s1, 10008, f1, JSDRV_ARRAY_SIZE(f1));
    sbuf_f32_add(&s2, 10000, f2, JSDRV_ARRAY_SIZE(f2));
    assert_int_equal(SAMPLE_BUFFER_LENGTH - 1, sbuf_f32_length(&s1));
    assert_int_equal(SAMPLE_BUFFER_LENGTH - 1, sbuf_f32_length(&s2));
    sbuf_f32_mult(&r, &s1, &s2);
    assert_int_equal(SAMPLE_BUFFER_LENGTH - 5, sbuf_f32_length(&r));
    assert_int_equal(7974, r.msg_sample_id);
    assert_float_equal(40.0f, r.buffer[SAMPLE_BUFFER_LENGTH - 7], 1e-7);
    assert_float_equal(55.0f, r.buffer[SAMPLE_BUFFER_LENGTH - 6], 1e-7);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_add_one),
            cmocka_unit_test(test_add_one_skip),
            cmocka_unit_test(test_add_one_duplicate),
            cmocka_unit_test(test_add_wrap),
            cmocka_unit_test(test_mult),
            cmocka_unit_test(test_mult_no_overlap),
            cmocka_unit_test(test_mult_some_overlap),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
