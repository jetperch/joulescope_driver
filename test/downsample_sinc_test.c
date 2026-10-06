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
#include <math.h>
#include "jsdrv_prv/downsample_sinc.h"


static void test_invalid_args(void **state) {
    (void) state;
    assert_null(jsdrv_downsample_sinc_alloc(0, 1));
    assert_null(jsdrv_downsample_sinc_alloc(1, 1));
    assert_null(jsdrv_downsample_sinc_alloc(1001, 1));
    assert_null(jsdrv_downsample_sinc_alloc(10, 0));
    assert_null(jsdrv_downsample_sinc_alloc(10, 4));
}

static void test_accessors(void **state) {
    (void) state;
    assert_int_equal(1, jsdrv_downsample_sinc_decimate_factor(NULL));
    assert_int_equal(0, jsdrv_downsample_sinc_order(NULL));
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(100, 3);
    assert_non_null(d);
    assert_int_equal(100, jsdrv_downsample_sinc_decimate_factor(d));
    assert_int_equal(3, jsdrv_downsample_sinc_order(d));
    jsdrv_downsample_sinc_free(d);
}

static void test_null_passthrough(void **state) {
    (void) state;
    float y = 0.0f;
    assert_true(jsdrv_downsample_sinc_add_f32(NULL, 12345, 1.25f, &y));
    assert_true(y == 1.25f);
}

// Constant input must produce bit-exact constant output (seeding hides
// the startup transient, integer kernel sum == norm makes DC exact).
static void test_dc_exact(void **state) {
    (void) state;
    static const uint32_t factors[] = {2, 5, 10, 100, 1000};
    static const float values[] = {1.5f, -0.25f, 3.140625f};
    for (uint32_t order = 1; order <= 3; ++order) {
        for (size_t fi = 0; fi < sizeof(factors) / sizeof(factors[0]); ++fi) {
            uint32_t factor = factors[fi];
            for (size_t vi = 0; vi < sizeof(values) / sizeof(values[0]); ++vi) {
                float c = values[vi];
                struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(factor, order);
                assert_non_null(d);
                uint32_t out_count = 0;
                for (uint64_t i = 0; i < (uint64_t) factor * 4; ++i) {
                    float y = 0.0f;
                    if (jsdrv_downsample_sinc_add_f32(d, i, c, &y)) {
                        assert_true(y == c);
                        ++out_count;
                    }
                }
                assert_int_equal(4, out_count);
                jsdrv_downsample_sinc_free(d);
            }
        }
    }
}

// Feed zeros with a single unit impulse and return the outputs.
// The first sample (id 0) is 0.0, so seeding does not disturb the result.
static void impulse_response(uint32_t factor, uint32_t order, uint64_t impulse_id,
                             float * y_out, uint32_t y_count) {
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(factor, order);
    assert_non_null(d);
    uint32_t out_idx = 0;
    for (uint64_t i = 0; out_idx < y_count; ++i) {
        float x = (i == impulse_id) ? 1.0f : 0.0f;
        float y = 0.0f;
        if (jsdrv_downsample_sinc_add_f32(d, i, x, &y)) {
            y_out[out_idx++] = y;
        }
    }
    jsdrv_downsample_sinc_free(d);
}

// Verify the kernel shapes through the impulse response.  An impulse at
// input tick t contributes kernel[t - e + L - 1] / norm to the output
// whose window ends at tick e (window length L = order*(R-1) + 1).
static void test_impulse_kernels(void **state) {
    (void) state;
    float y[4];

    // sinc1, R=2: kernel [1,1]/2; impulse t=4 -> window end 5 only.
    impulse_response(2, 1, 4, y, 4);
    assert_true(y[0] == 0.0f);
    assert_true(y[1] == 0.0f);
    assert_true(y[2] == 0.5f);   // window {4,5}
    assert_true(y[3] == 0.0f);

    // sinc2, R=2: kernel [1,2,1]/4; impulse t=5 -> ends 5 and 7.
    impulse_response(2, 2, 5, y, 4);
    assert_true(y[0] == 0.0f);
    assert_true(y[1] == 0.0f);
    assert_true(y[2] == 0.25f);  // kernel[2]/4 (window {3,4,5})
    assert_true(y[3] == 0.25f);  // kernel[0]/4 (window {5,6,7})

    // sinc3, R=2: kernel [1,3,3,1]/8; impulse t=4 -> ends 5 and 7.
    impulse_response(2, 3, 4, y, 4);
    assert_true(y[0] == 0.0f);
    assert_true(y[1] == 0.0f);
    assert_true(y[2] == 3.0f / 8.0f);  // kernel[2]/8 (window {2..5})
    assert_true(y[3] == 1.0f / 8.0f);  // kernel[0]/8 (window {4..7})

    // sinc2, R=3: kernel [1,2,3,2,1]/9; impulse t=4 -> ends 5 and 8.
    impulse_response(3, 2, 4, y, 4);
    assert_true(y[0] == 0.0f);
    assert_true(y[1] == 2.0f / 9.0f);  // kernel[3]/9 (window {1..5})
    assert_true(y[2] == 1.0f / 9.0f);  // kernel[0]/9 (window {4..8})
    assert_true(y[3] == 0.0f);
}

// sinc1 output is the arithmetic mean of each factor-sized block.
static void test_sinc1_block_mean(void **state) {
    (void) state;
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(10, 1);
    assert_non_null(d);
    uint32_t out_count = 0;
    for (uint64_t i = 0; i < 50; ++i) {
        float y = 0.0f;
        if (jsdrv_downsample_sinc_add_f32(d, i, (float) i, &y)) {
            assert_float_equal(y, (double) out_count * 10.0 + 4.5, 1e-6);
            ++out_count;
        }
    }
    assert_int_equal(5, out_count);
    jsdrv_downsample_sinc_free(d);
}

// Unaligned initial sample_id: discard until (sample_id % factor) == 0.
static void test_alignment(void **state) {
    (void) state;
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(5, 1);
    assert_non_null(d);
    float y = 0.0f;
    // ids 7, 8, 9 discarded (values chosen to poison the mean if kept)
    for (uint64_t i = 7; i < 10; ++i) {
        assert_false(jsdrv_downsample_sinc_add_f32(d, i, 100.0f, &y));
    }
    // ids 10..14 form the first block
    for (uint64_t i = 10; i < 14; ++i) {
        assert_false(jsdrv_downsample_sinc_add_f32(d, i, 2.0f, &y));
    }
    assert_true(jsdrv_downsample_sinc_add_f32(d, 14, 2.0f, &y));
    assert_true(y == 2.0f);
    jsdrv_downsample_sinc_free(d);
}

// A NaN input poisons exactly the outputs whose window contains it.
// R=5, order=2, L=9: NaN at tick 6 lands in windows ending at 9 and 14
// (outputs 1 and 2); all other outputs stay clean.
static void test_nan(void **state) {
    (void) state;
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(5, 2);
    assert_non_null(d);
    float y[8];
    uint32_t out_idx = 0;
    for (uint64_t i = 0; out_idx < 8; ++i) {
        float x = (i == 6) ? NAN : 1.0f;
        if (jsdrv_downsample_sinc_add_f32(d, i, x, &y[out_idx])) {
            ++out_idx;
        }
    }
    assert_false(isnan(y[0]));
    assert_true(isnan(y[1]));
    assert_true(isnan(y[2]));
    for (uint32_t k = 3; k < 8; ++k) {
        assert_false(isnan(y[k]));
        assert_true(y[k] == 1.0f);
    }
    jsdrv_downsample_sinc_free(d);
}

// clear() resets alignment and seeding; the filter recovers cleanly on
// a new unaligned stream.
static void test_clear_realign(void **state) {
    (void) state;
    struct jsdrv_downsample_sinc_s * d = jsdrv_downsample_sinc_alloc(4, 3);
    assert_non_null(d);
    float y = 0.0f;
    for (uint64_t i = 0; i < 6; ++i) {
        jsdrv_downsample_sinc_add_f32(d, i, 5.0f, &y);
    }
    jsdrv_downsample_sinc_clear(d);
    // realign: ids 10, 11 discarded
    assert_false(jsdrv_downsample_sinc_add_f32(d, 10, 9.0f, &y));
    assert_false(jsdrv_downsample_sinc_add_f32(d, 11, 9.0f, &y));
    // ids 12..15: first block after reseed; constant -> exact
    for (uint64_t i = 12; i < 15; ++i) {
        assert_false(jsdrv_downsample_sinc_add_f32(d, i, 3.0f, &y));
    }
    assert_true(jsdrv_downsample_sinc_add_f32(d, 15, 3.0f, &y));
    assert_true(y == 3.0f);
    jsdrv_downsample_sinc_free(d);
}

// filter(-x) == -filter(x) exactly: exercises the negate / udiv / negate
// path and the symmetric rounding.
static void test_negation_symmetry(void **state) {
    (void) state;
    struct jsdrv_downsample_sinc_s * dp = jsdrv_downsample_sinc_alloc(7, 3);
    struct jsdrv_downsample_sinc_s * dn = jsdrv_downsample_sinc_alloc(7, 3);
    assert_non_null(dp);
    assert_non_null(dn);
    uint32_t out_count = 0;
    for (uint64_t i = 0; i < 700; ++i) {
        float x = ((float) ((i * 37U) % 23U) - 11.0f) * 0.815f;
        float yp = 0.0f;
        float yn = 0.0f;
        bool rp = jsdrv_downsample_sinc_add_f32(dp, i, x, &yp);
        bool rn = jsdrv_downsample_sinc_add_f32(dn, i, -x, &yn);
        assert_int_equal(rp, rn);
        if (rp) {
            assert_true(yn == -yp);
            ++out_count;
        }
    }
    assert_int_equal(100, out_count);
    jsdrv_downsample_sinc_free(dp);
    jsdrv_downsample_sinc_free(dn);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_invalid_args),
            cmocka_unit_test(test_accessors),
            cmocka_unit_test(test_null_passthrough),
            cmocka_unit_test(test_dc_exact),
            cmocka_unit_test(test_impulse_kernels),
            cmocka_unit_test(test_sinc1_block_mean),
            cmocka_unit_test(test_alignment),
            cmocka_unit_test(test_nan),
            cmocka_unit_test(test_clear_realign),
            cmocka_unit_test(test_negation_symmetry),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
