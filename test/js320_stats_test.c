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
#include <math.h>
#include "jsdrv_prv/js220_i128.h"
#include "jsdrv_prv/js320_stats.h"
#include "jsdrv.h"


#define ASSERT_DOUBLE_NEAR(a, b, tol) \
    assert_true(fabs((a) - (b)) < (tol))


// Build a raw stats message with a constant signal value for one channel.
// value is in native units (e.g., amps).  Q52 fixed-point.
static void fill_channel_constant(struct js320_channel_raw_s * ch,
                                  double value, uint32_t n) {
    // x in 12Q52
    int64_t x_q52 = (int64_t)(value * (double)(1ULL << 52));
    // x1 = sum(x) = n * x, stored as i128 Q52
    js220_i128 x1 = js220_i128_init_i64(x_q52);
    // Multiply by n using repeated addition (fine for small test n)
    js220_i128 sum = {.u64 = {0, 0}};
    for (uint32_t i = 0; i < n; i++) {
        sum = js220_i128_add(sum, x1);
    }
    ch->x1 = sum;

    // x2 = sum(x^2)[127:32].  x^2 in Q104, drop lower 32 -> Q72.
    // For constant signal: x2 = n * (x_q52^2 >> 32)
    // Compute x_q52^2 as i128
    js220_i128 xsq = js220_i128_square_i64(x_q52);
    // Shift right by 32
    js220_i128 xsq_trunc = js220_i128_rshift(xsq, 32);
    js220_i128 sum2 = {.u64 = {0, 0}};
    for (uint32_t i = 0; i < n; i++) {
        sum2 = js220_i128_add(sum2, xsq_trunc);
    }
    ch->x2 = sum2;

    // integ = sum(x) same as x1 for a single block
    ch->integ = sum;
    ch->min = x_q52;
    ch->max = x_q52;
}

static void test_invalid_header(void ** state) {
    (void) state;
    struct js320_statistics_raw_s src;
    struct jsdrv_statistics_s dst;
    memset(&src, 0, sizeof(src));
    src.header = 0x00000000;  // wrong version and type
    assert_int_not_equal(0, js320_stats_convert(&src, &dst));
}

static void test_zero_sample_count(void ** state) {
    (void) state;
    struct js320_statistics_raw_s src;
    struct jsdrv_statistics_s dst;
    memset(&src, 0, sizeof(src));
    src.header = (1U << 24) | (1U << 16);  // version=1, type=1
    src.sample_count = 0;
    assert_int_equal(0, js320_stats_convert(&src, &dst));
    assert_int_equal(1, dst.version);
    assert_int_equal(0, dst.block_sample_count);
}

static void test_constant_signal(void ** state) {
    (void) state;
    uint32_t n = 1000;
    double i_val = 0.5;
    double v_val = 3.3;
    double p_val = i_val * v_val;

    struct js320_statistics_raw_s src;
    memset(&src, 0, sizeof(src));
    src.header = (1U << 24) | (1U << 16);  // version=1, type=1
    src.decimate = 16;
    src.sample_freq = 16000000;
    src.sample_count = n;
    src.block_sample_id = 0;
    src.accum_sample_id = 0;

    fill_channel_constant(&src.i, i_val, n);
    fill_channel_constant(&src.v, v_val, n);
    fill_channel_constant(&src.p, p_val, n);

    struct jsdrv_statistics_s dst;
    assert_int_equal(0, js320_stats_convert(&src, &dst));

    assert_int_equal(1, dst.version);
    assert_int_equal(16, dst.decimate_factor);
    assert_int_equal(n, dst.block_sample_count);
    assert_int_equal(16000000, dst.sample_freq);

    // Averages should match the constant values
    ASSERT_DOUBLE_NEAR(i_val, dst.i_avg, 1e-6);
    ASSERT_DOUBLE_NEAR(v_val, dst.v_avg, 1e-6);
    ASSERT_DOUBLE_NEAR(p_val, dst.p_avg, 1e-6);

    // Std should be ~0 for constant signal
    ASSERT_DOUBLE_NEAR(0.0, dst.i_std, 1e-6);
    ASSERT_DOUBLE_NEAR(0.0, dst.v_std, 1e-6);
    ASSERT_DOUBLE_NEAR(0.0, dst.p_std, 1e-6);

    // Min/max should equal the constant value
    ASSERT_DOUBLE_NEAR(i_val, dst.i_min, 1e-6);
    ASSERT_DOUBLE_NEAR(i_val, dst.i_max, 1e-6);
    ASSERT_DOUBLE_NEAR(v_val, dst.v_min, 1e-6);
    ASSERT_DOUBLE_NEAR(v_val, dst.v_max, 1e-6);
    ASSERT_DOUBLE_NEAR(p_val, dst.p_min, 1e-6);
    ASSERT_DOUBLE_NEAR(p_val, dst.p_max, 1e-6);

    // Charge = integral of current / decimated_freq
    double decimated_freq = 16000000.0 / 16.0;
    double expected_charge = i_val * n / decimated_freq;
    ASSERT_DOUBLE_NEAR(expected_charge, dst.charge_f64, 1e-9);

    double expected_energy = p_val * n / decimated_freq;
    ASSERT_DOUBLE_NEAR(expected_energy, dst.energy_f64, 1e-9);
}

static void test_struct_size(void ** state) {
    (void) state;
    // Per doc/statistics.md: 56 u32 = 224 bytes
    assert_int_equal(224, sizeof(struct js320_statistics_raw_s));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_invalid_header),
        cmocka_unit_test(test_zero_sample_count),
        cmocka_unit_test(test_constant_signal),
        cmocka_unit_test(test_struct_size),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
