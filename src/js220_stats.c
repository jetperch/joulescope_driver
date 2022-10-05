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
#include "jsdrv_prv/js220_i128.h"
#include <math.h>

static js220_i128 compute_integral(js220_i128 x, uint32_t n) {
    if (x.i64[1] < 0) {
        x = js220_i128_neg(x);
        x = js220_i128_udiv(x, n, NULL);
        x = js220_i128_neg(x);
    } else {
        x = js220_i128_udiv(x, n, NULL);
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
    dst->i_std = js220_i128_compute_std(src->i_x1, src->i_x2, dst->block_sample_count, 31);
    dst->i_min = src->i_min * mm_scale;
    dst->i_max = src->i_max * mm_scale;

    dst->v_avg = src->v_x1 * avg_scale;
    dst->v_std = js220_i128_compute_std(src->v_x1, src->v_x2, dst->block_sample_count, 31);
    dst->v_min = src->v_min * mm_scale;
    dst->v_max = src->v_max * mm_scale;

    mm_scale = pow(2, -27);
    avg_scale = 1.0 / (((uint64_t) dst->block_sample_count) << 27);
    dst->p_avg = src->p_x1 * avg_scale;
    dst->p_std = js220_i128_compute_std(src->p_x1, src->p_x2, dst->block_sample_count, 27);
    dst->p_min = src->p_min * mm_scale;
    dst->p_max = src->p_max * mm_scale;

    dst->charge_f64 = js220_i128_to_f64(src->i_int, 31) / sample_freq;
    dst->energy_f64 = js220_i128_to_f64(src->p_int, 31) / sample_freq;

    js220_i128 charge = compute_integral(src->i_int, sample_freq);
    js220_i128 energy = compute_integral(src->p_int, sample_freq);

    dst->charge_i128[0] = charge.u64[0];
    dst->charge_i128[1] = charge.u64[1];
    dst->energy_i128[0] = energy.u64[0];
    dst->energy_i128[1] = energy.u64[1];

    //JSDRV_LOGI(i_avg=%.3g, v_avg=%.3g, p_avg=%.3g", dst->i_avg, dst->v_avg, dst->p_avg);
    return 0;
}
