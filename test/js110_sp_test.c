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
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/js110_sample_processor.h"
#include <stdio.h>


#define SETUP()                             \
    (void) state;                           \
    struct js110_sp_s s;                    \
    js110_sp_initialize(&s);                \
    for (int i = 0; i < 9; ++i) {           \
        s.cal[0][0][i] = (i + 1) * 100.0;   \
        s.cal[0][1][i] = pow(10, -3 - i);   \
    }                                       \
    for (int i = 0; i < 2; ++i) {           \
        s.cal[1][0][i] = (i + 1) * -100.0;  \
        s.cal[1][1][i] = pow(10, -4 - i);   \
    }


typedef void (*generate_cbk)(void * user_data, size_t i, struct js110_sample_s sample);


static void generate(struct js110_sp_s * s, uint8_t current_range, uint8_t gap, generate_cbk cbk, void * user_data) {
    uint16_t i_step = 10;
    uint16_t v_step = 12;
    for (size_t k = 0; k < 128; ++k) {
        uint8_t i_range = (k < 32) ? current_range : current_range + 1;
        uint32_t current = (uint32_t) (2000 + k * i_step);
        uint32_t voltage = (uint32_t) (3000 + k * v_step);
        uint32_t sample_in =
                ((current & 0x3fff) << 2)
                | ((voltage & 0x3fff) << 18)
                | (i_range & 3)
                | ((i_range & 4) << (16 - 2))
                | ((k & 1) ? 0x20000 : 0);
        struct js110_sample_s sample = js110_sp_process(s, sample_in, 0);
        size_t z = k - JS110_SUPPRESS_SAMPLES_MAX + 1;
        if (k < (JS110_SUPPRESS_SAMPLES_MAX - 1)) {
            assert_true(isnan(sample.i));
            assert_true(isnan(sample.v));
            assert_true(isnan(sample.p));
            assert_int_equal(sample.current_range, JS110_I_RANGE_MISSING);
        } else if (k < (JS110_SUPPRESS_SAMPLES_MAX - 1 + 32)) {
            assert_float_equal((2000 + z * i_step + 100 * (current_range + 1)) * pow(10, -3 - current_range), sample.i, 1e-10);
            assert_float_equal((3000 + z * v_step - 100) * 0.0001, sample.v, 1e-10);
            assert_float_equal(sample.i * sample.v, sample.p, 1e-6);
            assert_int_equal(sample.current_range, current_range);
        } else if (k < (JS110_SUPPRESS_SAMPLES_MAX - 1 + 32 + gap)) {
            cbk(user_data, z, sample);
        } else {
            assert_float_equal((2000 + z * i_step + 100 * (current_range + 2)) * pow(10, -3 - current_range - 1), sample.i, 1e-10);
            assert_float_equal((3000 + z * v_step - 100) * 0.0001, sample.v, 1e-10);
            assert_int_equal(sample.current_range, current_range + 1);
        }
    }
}

static void expect_nan(void * user_data, size_t i, struct js110_sample_s sample) {
    (void) user_data;
    (void) i;
    assert_true(isnan(sample.i));
    assert_true(isnan(sample.v));
    assert_true(isnan(sample.p));
}

static void test_off(void ** state) {
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_OFF;
    s._suppress_matrix = NULL;
    for (uint8_t idx = 0; idx < 6; ++idx) {
        js110_sp_reset(&s);
        generate(&s, idx, 0, NULL, NULL);
    }
}

static void test_nan_0_1_0(void ** state) {
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_NAN;
    s._suppress_samples_window = 1;
    s._suppress_matrix = NULL;
    generate(&s, 0, 1, expect_nan, NULL);
}

static void test_nan_0_2_0(void ** state) {
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_NAN;
    s._suppress_samples_window = 2;
    s._suppress_matrix = NULL;
    generate(&s, 0, 2, expect_nan, NULL);
}

static void test_nan_0_n_0(void ** state) {
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_NAN;
    s._i_range_last = JS110_I_RANGE_MISSING;
    generate(&s, 0, 3, expect_nan, NULL);
}

struct mean_s {
    float i;
    float v;
    float p;
};

static void expect_mean(void * user_data, size_t i, struct js110_sample_s sample) {
    (void) i;
    struct mean_s * m = (struct mean_s *) user_data;
    //printf("mean %f %f %f\n", sample.i, sample.v, sample.p);
    assert_false(isnan(sample.i));
    assert_false(isnan(sample.v));
    assert_false(isnan(sample.p));
    assert_float_equal(m->i, sample.i, 1e-6);
    assert_float_equal(m->v, sample.v, 1e-6);
    assert_float_equal(m->p, sample.p, 1e-6);
}

static void test_mean_1_3_1(void ** state) {
    struct mean_s m = {
            .i = 1.332500f,
            .v = 0.329600f,
            .p = 0.436606f,
    };
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_MEAN;
    s._suppress_samples_window = 3;
    s._suppress_matrix = NULL;
    s._i_range_last = JS110_I_RANGE_MISSING;
    generate(&s, 0, 3, expect_mean, &m);
}

static void test_mean_2_3_1(void ** state) {
    struct mean_s m = {
            .i = 1.688333f,
            .v = 0.328400f,
            .p = 0.551871f,
    };
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_MEAN;
    s._suppress_samples_pre = 2;
    s._suppress_samples_window = 3;
    s._suppress_matrix = NULL;
    s._i_range_last = JS110_I_RANGE_MISSING;
    generate(&s, 0, 3, expect_mean, &m);
}

static void test_mean_1_3_2(void ** state) {
    struct mean_s m = {
            .i = 0.973000f,
            .v = 0.330000f,
            .p = 0.319078f,
    };
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_MEAN;
    s._suppress_samples_post = 2;
    s._suppress_samples_window = 3;
    s._suppress_matrix = NULL;
    s._i_range_last = JS110_I_RANGE_MISSING;
    generate(&s, 0, 3, expect_mean, &m);
}

struct interp_s {
    float i0;
    float v0;
    float p0;
    float i_step;
    float v_step;
    float p_step;
};

static void expect_interp(void * user_data, size_t i, struct js110_sample_s sample) {
    struct interp_s * m = (struct interp_s *) user_data;
    // printf("interp %f %f %f\n", sample.i, sample.v, sample.p);
    assert_false(isnan(sample.i));
    assert_false(isnan(sample.v));
    assert_false(isnan(sample.p));
    size_t k = i - 31;
    float i_expect = m->i0 + m->i_step * k;
    float v_expect = m->v0 + m->v_step * k;
    float p_expect = m->p0 + m->p_step * k;
    assert_float_equal(i_expect, sample.i, 1e-6);
    assert_float_equal(v_expect, sample.v, 1e-6);
    assert_float_equal(p_expect, sample.p, 1e-6);
}

static void test_interp_1_3_1(void ** state) {
    struct interp_s m = {
            .i0 = 2.41000009f,
            .v0 = 0.327199996f,
            .p0 = 0.788551986,
            .i_step = (float) ((0.254999995 - 2.41000009) / 4),
            .v_step = (float) ((0.331999987 - 0.327199996) / 4),
            .p_step = (float) ((0.084660001 - 0.788551986) / 4),
    };
    SETUP()
    s._suppress_mode = JS110_SUPPRESS_MODE_INTERP;
    s._suppress_samples_window = 3;
    s._suppress_matrix = NULL;
    s._i_range_last = JS110_I_RANGE_MISSING;
    generate(&s, 0, 3, expect_interp, &m);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_off),
            cmocka_unit_test(test_nan_0_1_0),
            cmocka_unit_test(test_nan_0_2_0),
            cmocka_unit_test(test_nan_0_n_0),
            cmocka_unit_test(test_mean_1_3_1),
            cmocka_unit_test(test_mean_2_3_1),
            cmocka_unit_test(test_mean_1_3_2),
            cmocka_unit_test(test_interp_1_3_1),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
