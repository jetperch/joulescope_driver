/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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
#include "jsdrv_prv/devices/js110/js110_stats.h"
#include "jsdrv_prv/devices/js220/js220_i128.h"
#include "jsdrv.h"


static void test_positive_signal(void ** state) {
    (void) state;
    struct js110_stats_s s;
    struct jsdrv_statistics_s * stats = NULL;
    js110_stats_initialize(&s);
    js110_stats_sample_count_set(&s, 4);
    assert_null(js110_stats_compute(&s, 1.0f, 4.0f, 4.0f));
    assert_null(js110_stats_compute(&s, 2.0f, 4.0f, 8.0f));
    assert_null(js110_stats_compute(&s, 3.0f, 4.0f, 12.0f));
    stats = js110_stats_compute(&s, 2.0f, 4.0f, 8.0f);
    assert_non_null(stats);
    assert_float_equal(2.0, stats->i_avg, 1e-6);
    assert_float_equal(1.0, stats->i_min, 1e-6);
    assert_float_equal(3.0, stats->i_max, 1e-6);
    assert_float_equal(4.0, stats->v_avg, 1e-6);
    assert_float_equal(8.0, stats->p_avg, 1e-6);
    // i128 accumulators match the f64 fields with JSDRV_STATISTICS_I128_Q bits
    js220_i128 charge = {.u64 = {stats->charge_i128[0], stats->charge_i128[1]}};
    js220_i128 energy = {.u64 = {stats->energy_i128[0], stats->energy_i128[1]}};
    assert_true(stats->charge_f64 > 0.0);
    assert_float_equal(stats->charge_f64, js220_i128_to_f64(charge, JSDRV_STATISTICS_I128_Q), 1e-12);
    assert_float_equal(stats->energy_f64, js220_i128_to_f64(energy, JSDRV_STATISTICS_I128_Q), 1e-12);
}

static void test_negative_signal_max(void ** state) {
    // max initialized to FLT_MIN (+1.18e-38) instead of -FLT_MAX, so an
    // always-negative signal reported max ~= 0 instead of the true max
    (void) state;
    struct js110_stats_s s;
    struct jsdrv_statistics_s * stats = NULL;
    js110_stats_initialize(&s);
    js110_stats_sample_count_set(&s, 3);
    assert_null(js110_stats_compute(&s, -3.0f, -1.0f, 3.0f));
    assert_null(js110_stats_compute(&s, -2.0f, -1.0f, 2.0f));
    stats = js110_stats_compute(&s, -1.0f, -1.0f, 1.0f);
    assert_non_null(stats);
    assert_float_equal(-1.0, stats->i_max, 1e-6);
    assert_float_equal(-3.0, stats->i_min, 1e-6);
    assert_float_equal(-2.0, stats->i_avg, 1e-6);
    assert_float_equal(-1.0, stats->v_max, 1e-6);
}

static void test_all_nan_block(void ** state) {
    // a block of all-NaN samples previously divided by valid_count == 0
    (void) state;
    struct js110_stats_s s;
    struct jsdrv_statistics_s * stats = NULL;
    js110_stats_initialize(&s);
    js110_stats_sample_count_set(&s, 2);
    assert_null(js110_stats_compute(&s, NAN, NAN, NAN));
    stats = js110_stats_compute(&s, NAN, NAN, NAN);
    assert_non_null(stats);
    assert_true(isnan(stats->i_avg));
    assert_true(isnan(stats->i_std));
    assert_true(isnan(stats->i_min));
    assert_true(isnan(stats->i_max));
    assert_true(isnan(stats->v_avg));
    assert_true(isnan(stats->p_avg));

    // and the next block recovers
    assert_null(js110_stats_compute(&s, 1.0f, 1.0f, 1.0f));
    stats = js110_stats_compute(&s, 1.0f, 1.0f, 1.0f);
    assert_non_null(stats);
    assert_float_equal(1.0, stats->i_avg, 1e-6);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_positive_signal),
            cmocka_unit_test(test_negative_signal_max),
            cmocka_unit_test(test_all_nan_block),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
