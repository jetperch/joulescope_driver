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

#include "jsdrv_prv/js110_stats.h"
#include "jsdrv_prv/js220_i128.h"
#include "jsdrv_prv/platform.h"
#include <float.h>
#include <math.h>


static void clear(struct js110_stats_s * self) {
    for (int i = 0; i < 3; ++i) {
        struct js110_stats_field_s * f = &self->fields[i];
        f->avg = 0.0;
        f->std = 0.0;
        f->min = FLT_MAX;
        f->max = FLT_MIN;
        f->x1 = 0;
        f->x2.u64[0] = 0;
        f->x2.u64[1] = 0;
    }
}

void js110_stats_initialize(struct js110_stats_s * self) {
    jsdrv_memset(self, 0, sizeof(*self));
    struct jsdrv_statistics_s * s = &self->statistics;
    s->version = 1;
    s->decimate_factor = 1;
    s->block_sample_count = 1000000;
    s->sample_freq = 2000000;
    js110_stats_clear(self);
}

void js110_stats_clear(struct js110_stats_s * self) {
    self->sample_count = 0;
    self->valid_count = 0;
    self->charge.u64[0] = 0;
    self->charge.u64[1] = 0;
    self->energy.u64[0] = 0;
    self->energy.u64[1] = 0;
    struct jsdrv_statistics_s * s = &self->statistics;
    s->decimate_factor = 1;
    s->block_sample_id = 0;
    s->accum_sample_id = 0;
    clear(self);
}

void js110_stats_sample_count_set(struct js110_stats_s * self, uint32_t sample_count) {
    JSDRV_LOGI("js110_stats_sample_count_set(%lu)", sample_count);
    js110_stats_clear(self);
    struct jsdrv_statistics_s * s = &self->statistics;
    s->block_sample_count = sample_count;
}

static void update(struct js110_stats_field_s * f, float x) {
    f->avg += x;
    if (x < f->min) {
        f->min = x;
    }
    if (x > f->max) {
        f->max = x;
    }
    int64_t x_i64 = (int64_t) (x * (1 << 31));
    f->x1 += x_i64;
    js220_i128 r = js220_i128_square_i64(x_i64);
    f->x2 = js220_i128_add(f->x2, r);
}

static void finalize(struct js110_stats_field_s * f, uint32_t sample_count) {
    f->avg /= sample_count;
    f->std = js220_i128_compute_std(f->x1, f->x2, sample_count, 31);
}

#define FIELD_COPY(_idx, _field) \
    s->_field##_avg = self->fields[_idx].avg; \
    s->_field##_std = self->fields[_idx].std; \
    s->_field##_min = self->fields[_idx].min; \
    s->_field##_max = self->fields[_idx].max

struct jsdrv_statistics_s * js110_stats_compute(struct js110_stats_s * self, float i, float v, float p) {
    struct jsdrv_statistics_s * s = &self->statistics;
    if (0 == self->sample_count) {
        clear(self);
    }
    ++self->sample_count;
    if (!isnan(i) && !isnan(v) && !isnan(p)) {
        ++self->valid_count;
        update(&self->fields[0], i);
        update(&self->fields[1], v);
        update(&self->fields[2], p);
    }

    if (self->sample_count == s->block_sample_count) {
        js220_i128 a;
        a.u64[0] = self->fields[0].x1;
        a.i64[1] = (self->fields[0].x1 < 0) ? -1 : 0;
        self->charge = js220_i128_add(self->charge, a);

        a.u64[0] = self->fields[2].x1;
        a.i64[1] = (self->fields[2].x1 < 0) ? -1 : 0;
        self->energy = js220_i128_add(self->energy, a);

        finalize(&self->fields[0], self->valid_count);
        finalize(&self->fields[1], self->valid_count);
        finalize(&self->fields[2], self->valid_count);
        self->sample_count = 0;
        self->valid_count = 0;

        uint32_t sampling_freq = s->sample_freq / s->decimate_factor;
        a = js220_i128_compute_integral(self->charge, sampling_freq);
        s->charge_i128[0] = a.u64[0];
        s->charge_i128[1] = a.u64[1];
        s->charge_f64 = js220_i128_to_f64(a, 31);
        a = js220_i128_compute_integral(self->energy, sampling_freq);
        s->energy_i128[0] = a.u64[0];
        s->energy_i128[1] = a.u64[1];
        s->energy_f64 = js220_i128_to_f64(a, 31);

        FIELD_COPY(0, i);
        FIELD_COPY(1, v);
        FIELD_COPY(2, p);

        return s;
    } else {
        return 0;
    }
}