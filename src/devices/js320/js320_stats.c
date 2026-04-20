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

#include "jsdrv_prv/devices/js320/js320_stats.h"
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/devices/js220/js220_i128.h"
#include <math.h>
#include <string.h>

#define JS320_STATS_Q           52
// x2 = (x * x)[127:32], so Q(52+52) - 32 = Q72
#define JS320_STATS_Q_X2        72

// Compute std using float64 to avoid needing i128*i128 multiply.
// Precision loss is negligible for a double result.
static double js320_compute_std(js220_i128 x1, js220_i128 x2,
                                uint64_t n, uint32_t q, uint32_t q_x2) {
    if (n < 2) {
        return 0.0;
    }
    double mean = js220_i128_to_f64(x1, q) / (double) n;
    double x2_f = js220_i128_to_f64(x2, q_x2);
    double var = (x2_f / (double) n) - (mean * mean);
    return (var > 0.0) ? sqrt(var) : 0.0;
}

int32_t js320_stats_convert(
        const struct js320_statistics_raw_s * src,
        struct jsdrv_statistics_s * dst) {
    uint8_t version = (uint8_t) (src->header >> 24);
    uint8_t type = (uint8_t) (src->header >> 16);
    if (version != 1 || type != 1) {
        JSDRV_LOGW("js320 statistics invalid header: "
                   "version=%u type=%u", version, type);
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }

    memset(dst, 0, sizeof(*dst));
    dst->version = 1;
    dst->decimate_factor = (uint8_t) src->decimate;
    dst->block_sample_count = src->sample_count;
    dst->sample_freq = src->sample_freq;
    dst->block_sample_id = src->block_sample_id;
    dst->accum_sample_id = src->accum_sample_id;

    uint32_t n = src->sample_count;
    if (0 == n) {
        return 0;
    }
    uint32_t decimated_freq = src->sample_freq / src->decimate;
    double mm_scale = pow(2.0, -JS320_STATS_Q);

    // Current
    dst->i_avg = js220_i128_to_f64(src->i.x1, JS320_STATS_Q) / n;
    dst->i_std = js320_compute_std(src->i.x1, src->i.x2,
                                   n, JS320_STATS_Q, JS320_STATS_Q_X2);
    dst->i_min = (double) src->i.min * mm_scale;
    dst->i_max = (double) src->i.max * mm_scale;

    // Voltage
    dst->v_avg = js220_i128_to_f64(src->v.x1, JS320_STATS_Q) / n;
    dst->v_std = js320_compute_std(src->v.x1, src->v.x2,
                                   n, JS320_STATS_Q, JS320_STATS_Q_X2);
    dst->v_min = (double) src->v.min * mm_scale;
    dst->v_max = (double) src->v.max * mm_scale;

    // Power
    dst->p_avg = js220_i128_to_f64(src->p.x1, JS320_STATS_Q) / n;
    dst->p_std = js320_compute_std(src->p.x1, src->p.x2,
                                   n, JS320_STATS_Q, JS320_STATS_Q_X2);
    dst->p_min = (double) src->p.min * mm_scale;
    dst->p_max = (double) src->p.max * mm_scale;

    // Integrals: charge = integral of current, energy = integral of power
    dst->charge_f64 = js220_i128_to_f64(src->i.integ, JS320_STATS_Q)
                    / decimated_freq;
    dst->energy_f64 = js220_i128_to_f64(src->p.integ, JS320_STATS_Q)
                    / decimated_freq;

    js220_i128 charge = js220_i128_compute_integral(
        src->i.integ, decimated_freq);
    js220_i128 energy = js220_i128_compute_integral(
        src->p.integ, decimated_freq);
    dst->charge_i128[0] = charge.u64[0];
    dst->charge_i128[1] = charge.u64[1];
    dst->energy_i128[0] = energy.u64[0];
    dst->energy_i128[1] = energy.u64[1];

    return 0;
}
