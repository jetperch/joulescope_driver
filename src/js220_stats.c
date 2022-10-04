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

#include "jsdrv_prv/js220_stats.h"
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/log.h"
#if _WIN32
#include <intrin.h>
#include <immintrin.h>
#endif
#include <math.h>

// See https://docs.microsoft.com/en-us/cpp/intrinsics/udiv128
// See https://docs.microsoft.com/en-us/cpp/intrinsics/mul128

static const uint64_t U64_SIGN_BIT = 0x8000000000000000LLU;


static js220_i128 neg(js220_i128 x) {
    if (x.i64[1] < 0) {
        x.u64[1] = ~x.u64[1];
        x.u64[0] = ~x.u64[0];
        uint64_t z = x.u64[0] + 1;
        if ((x.u64[0] & U64_SIGN_BIT) && ~(z & U64_SIGN_BIT)) {
            x.u64[1] += 1;
        }
        x.u64[0] = z;
    }
    return x;
}

static js220_i128 udiv(js220_i128 dividend, uint64_t divisor, uint64_t * remainder) {
    uint64_t r = 0;
    if (NULL == remainder) {
        remainder = &r;
    }
    js220_i128 result;
#if defined(__clang__) || defined(__GNUC__)
    result.i128 = dividend.i128 / divisor;
    *remainder = dividend.i128 - (result.i128 * divisor);
#else
    result.u64[1] = dividend.u64[1] / divisor;
    dividend.u64[1] -= result.u64[1] * divisor;
    result.u64[0] = _udiv128(dividend.u64[1], dividend.u64[0], divisor, remainder);
#endif
    return result;
}

#if 0
static js220_i128 lshift(js220_i128 x, int32_t shift) {
    if (shift > 0) {
        x.u64[1] = (x.u64[1] << shift) | (x.u64[0] >> (64 - shift));
        x.u64[0] = x.u64[0] << shift;
    } else if (0 == shift) {
        // no operation
    } else {  // right shift signed
        x.u64[0] = (x.u64[0] >> shift) | (x.u64[1] << (64 - shift));
        x.i64[1] = x.i64[1] >> shift;
    }
    return x;
}

static js220_i128 rshift(js220_i128 x, int32_t shift) {
    return lshift(x, -shift);
}
#endif

double js220_stats_i128_to_f64(js220_i128 x, uint32_t q) {
    double f;
    int32_t exponent = 128 - q - 64;
    bool is_neg = false;
    if (0 != (x.u64[1] & U64_SIGN_BIT)) {
        is_neg = true;
        x = neg(x);
    }

    if ((x.u64[0] == 0) && (x.u64[1] == 0)) {
        return 0.0;
    }

    while (0 == (x.u64[1] & U64_SIGN_BIT)) {
        x.u64[1] <<= 1;
        x.u64[1] |= (x.u64[0] >> 63) & 1;
        x.u64[0] <<= 1;
        --exponent;
    }
    f = (double) x.u64[1];     // more precision than a double's mantissa
    f *= pow(2, exponent);
    if (is_neg) {
        f = -f;
    }
    return f;
}

static double compute_std(int64_t x1, js220_i128 x2, uint32_t n, uint32_t q) {
    double f;
    if (x1 < 0) {
        x1 = -x1;
    }
    js220_i128 m;
    js220_i128 d;
#if defined(__clang__) || defined(__GNUC__)
    __extension__ __int128 x1_i128 = x1;
    m.i128 = x1_i128 * x1_i128;
#else
    m.u64[0] = _umul128(x1, x1, &m.u64[1]);
#endif
    d = udiv(m, n, NULL);

    if ((x2.u64[1] < d.u64[1]) ||
            ((x2.u64[1] == d.u64[1]) && (x2.u64[0] < d.u64[0]))) {
        // numerical precision error, return 0.
        return 0.0;
    }

    if (d.u64[0] > x2.u64[0]) {
        x2.u64[1] -= 1;
    }
    m.u64[0] = x2.u64[0] - d.u64[0];
    m.u64[1] = x2.u64[1] - d.u64[1];

    // n = n - 1;  // sample variance, not population variance
    d = udiv(m, n, NULL);
    // note: integer square root would have higher precision
    f = js220_stats_i128_to_f64(d, q * 2);
    f = sqrt(f);
    return f;
}

static js220_i128 compute_integral(js220_i128 x, uint32_t n) {
    if (x.i64[1] < 0) {
        x = neg(x);
        x = udiv(x, n, NULL);
        x = neg(x);
    } else {
        x = udiv(x, n, NULL);
    };
    return x;
}

int32_t js220_stats_convert(struct js220_statistics_raw_s const * src, struct jsdrv_statistics_s * dst) {
    if (0x92 != (src->header >> 24)) {
        JSDRV_LOGW("statistics invalid header");
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }

    dst->version = 1;
    dst->rsv1_u8 = 0;
    dst->rsv2_u8 = 0;
    dst->decimate_factor = (src->header >> 24) & 0x0f;
    dst->block_sample_count = src->header & 0x00ffffff;
    dst->sample_freq = src->sample_freq;
    dst->rsv3_u8 = 0;
    dst->block_sample_id = src->block_sample_id;
    dst->accum_sample_id = src->accum_sample_id;
    uint32_t sample_freq = dst->sample_freq / dst->decimate_factor;

    double mm_scale = pow(2, -31);
    double avg_scale = 1.0 / (((uint64_t) dst->block_sample_count) << 31);

    dst->i_avg = src->i_x1 * avg_scale;
    dst->i_std = compute_std(src->i_x1, src->i_x2, dst->block_sample_count, 31);
    dst->i_min = src->i_min * mm_scale;
    dst->i_max = src->i_max * mm_scale;

    dst->v_avg = src->v_x1 * avg_scale;
    dst->v_std = compute_std(src->v_x1, src->v_x2, dst->block_sample_count, 31);
    dst->v_min = src->v_min * mm_scale;
    dst->v_max = src->v_max * mm_scale;

    mm_scale = pow(2, -27);
    avg_scale = 1.0 / (((uint64_t) dst->block_sample_count) << 27);
    dst->p_avg = src->p_x1 * avg_scale;
    dst->p_std = compute_std(src->p_x1, src->p_x2, dst->block_sample_count, 27);
    dst->p_min = src->p_min * mm_scale;
    dst->p_max = src->p_max * mm_scale;

    dst->charge_f64 = js220_stats_i128_to_f64(src->i_int, 31) / sample_freq;
    dst->energy_f64 = js220_stats_i128_to_f64(src->p_int, 31) / sample_freq;

    js220_i128 charge = compute_integral(src->i_int, sample_freq);
    js220_i128 energy = compute_integral(src->p_int, sample_freq);

    dst->charge_i128[0] = charge.u64[0];
    dst->charge_i128[1] = charge.u64[1];
    dst->energy_i128[0] = energy.u64[0];
    dst->energy_i128[1] = energy.u64[1];

    //JSDRV_LOGI(i_avg=%.3g, v_avg=%.3g, p_avg=%.3g", dst->i_avg, dst->v_avg, dst->p_avg);
    return 0;
}
