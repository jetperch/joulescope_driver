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
#include "jsdrv_prv/platform.h"
#include <float.h>
#include <math.h>


struct stats_block_s {
    double avg;                ///< The average current over the block.
    double std;                ///< The standard deviation of current over the block.
    double min;                ///< The minimum current value in the block.
    double max;                ///< The maximum current value in the block.
};

static void clear(struct js110_stats_s * self) {
    struct jsdrv_statistics_s * s = &self->statistics;
    s->i_avg = 0.0;
    s->i_std = 0.0;
    s->i_min = FLT_MAX;
    s->i_max = FLT_MIN;

    s->v_avg = 0.0;
    s->v_std = 0.0;
    s->v_min = FLT_MAX;
    s->v_max = FLT_MIN;

    s->p_avg = 0.0;
    s->p_std = 0.0;
    s->p_min = FLT_MAX;
    s->p_max = FLT_MIN;
}


void js110_stats_clear(struct js110_stats_s * self) {
    jsdrv_memset(self, 0, sizeof(*self));
    struct jsdrv_statistics_s * s = &self->statistics;
    s->version = 1;
    s->decimate_factor = 1;
    s->block_sample_count = 1000000;
    s->sample_freq = 2000000;
    s->block_sample_id = 0;
    s->accum_sample_id = 0;
    clear(self);
}

void js110_stats_sample_count_set(struct js110_stats_s * self, uint32_t sample_count) {
    js110_stats_clear(self);
    struct jsdrv_statistics_s * s = &self->statistics;
    s->block_sample_count = sample_count;
}

static void update(struct stats_block_s * b, float x) {
    b->avg += x;
    if (x < b->min) {
        b->min = x;
    }
    if (x > b->max) {
        b->max = x;
    }
    // todo compute standard deviation
}

static void finalize(struct stats_block_s * b, uint32_t sample_count) {
    b->avg /= sample_count;
}

struct jsdrv_statistics_s * js110_stats_compute(struct js110_stats_s * self, float i, float v, float p) {
    struct jsdrv_statistics_s * s = &self->statistics;
    if (0 == self->sample_count) {
        clear(self);
    }
    ++self->sample_count;
    if (!isnan(i) && !isnan(v) && !isnan(p)) {
        ++self->valid_count;
        update((struct stats_block_s *) &s->i_avg, i);
        update((struct stats_block_s *) &s->v_avg, v);
        update((struct stats_block_s *) &s->p_avg, p);
    }

    if (self->sample_count == s->block_sample_count) {
        finalize((struct stats_block_s *) &s->i_avg, self->valid_count);
        finalize((struct stats_block_s *) &s->v_avg, self->valid_count);
        finalize((struct stats_block_s *) &s->p_avg, self->valid_count);
        self->sample_count = 0;
        self->valid_count = 0;

        // todo update charge & energy

        return s;
    } else {
        return 0;
    }
}