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
#include "jsdrv_prv/js220_stats.h"
#include "js220_api.h"
#include "jsdrv.h"


static void test_to_f64(void ** state) {
    (void) state;
    assert_float_equal(0.0, js220_stats_i128_to_f64((js220_i128){.u64 = {0, 0}}, 0), 0.0);
    assert_float_equal(0.0, js220_stats_i128_to_f64((js220_i128){.u64 = {0, 0}}, 31), 0.0);
    assert_float_equal(1.0, js220_stats_i128_to_f64((js220_i128){.u64 = {0,  0x4000000000000000LLU}}, 126), 0.0);
    assert_float_equal(2.0, js220_stats_i128_to_f64((js220_i128){.u64 = {0,  0x4000000000000000LLU}}, 125), 0.0);
    assert_float_equal(-1.0, js220_stats_i128_to_f64((js220_i128){.u64 = {0, 0xC000000000000000LLU}}, 126), 0.0);
    assert_float_equal(1.0, js220_stats_i128_to_f64((js220_i128){.u64 = {1, 0}}, 0), 0.0);
    assert_float_equal(1.0, js220_stats_i128_to_f64((js220_i128){.u64 = {2, 0}}, 1), 0.0);
    assert_float_equal(2.0, js220_stats_i128_to_f64((js220_i128){.u64 = {8, 0}}, 2), 0.0);
    assert_float_equal(1.0, js220_stats_i128_to_f64((js220_i128){.u64 = {1LLU << 31, 0}}, 31), 0.0);
}

static void test_basic(void ** state) {
    (void) state;
    int64_t n = 64LL;
    struct js220_statistics_raw_s src = {
            .header = 0x92000000 | (uint32_t) n,
            .sample_freq = (uint32_t) n,
            .block_sample_id = 0x100 + (uint64_t) n,
            .accum_sample_id = 0x100,
            .i_x1 = (1LL * n) << 31,
            .i_min = (int64_t) (((uint64_t) -1LL) << 31),
            .i_max = 1LL << 31,
            .v_x1 = (-2 * n) << 31,
            .v_min = (int64_t) (((uint64_t) -5LL) << 31),
            .v_max = 2LL << 31,
            .p_x1 = (-3LL * n) << 27,
            .p_min = (int64_t) (((uint64_t) -3LL) << 27),
            .p_max = 3LL << 27,
            .i_x2 = {.u64 = {0, 16}},
            .i_int = {.u64 = {(2 * 1 * n) << 31, 0}},
            .v_x2 = {.u64 = {0, 128}},
            .v_int = {.u64 = {(2 * -2 * n) << 31, 0xffffffffffffffffLLU}},
            .p_x2 = {.u64 = {(9 * n) << 54, 0}},
            .p_int = {.u64 = {(2 * -3 * n) << 27, 0xffffffffffffffffLLU}},
    };
    struct jsdrv_statistics_s dst;

    assert_int_equal(0, js220_stats_convert(&src, &dst));
    assert_int_equal(1, dst.version);
    assert_int_equal(2, dst.decimate_factor);

    assert_int_equal(64, dst.block_sample_count);
    assert_int_equal(src.sample_freq, dst.sample_freq);
    assert_int_equal(src.block_sample_id, dst.block_sample_id);
    assert_int_equal(src.accum_sample_id, dst.accum_sample_id);

    assert_float_equal(1.0, dst.i_avg, 0.0);
    assert_float_equal(0.0, dst.i_std, 0.0);
    assert_float_equal(-1.0, dst.i_min, 0.0);
    assert_float_equal(1.0, dst.i_max, 0.0);

    assert_float_equal(-2.0, dst.v_avg, 0.0);
    assert_float_equal(2.0, dst.v_std, 0.0);
    assert_float_equal(-5.0, dst.v_min, 0.0);
    assert_float_equal(2.0, dst.v_max, 0.0);

    assert_float_equal(-3.0, dst.p_avg, 0.0);
    assert_float_equal(0.0, dst.p_std, 0.0);
    assert_float_equal(-3.0, dst.p_min, 0.0);
    assert_float_equal(3.0, dst.p_max, 0.0);

    assert_float_equal(2.0, dst.charge_f64, 0.0);
    assert_float_equal(-6.0, dst.energy_f64, 0.0);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_to_f64),
            cmocka_unit_test(test_basic),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
