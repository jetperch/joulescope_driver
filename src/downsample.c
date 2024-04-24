/*
 * Copyright 2014-2022 Jetperch LLC
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

/**
 * @file
 *
 * @brief Joulescope driver downsampling
 */

#include "jsdrv_prv/downsample.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include <math.h>
#include <limits.h>

#define BUFFER_SIZE (128U)              // must be power of 2, <= 256
#define BUFFER_MASK (BUFFER_SIZE - 1U)

#define COEF_2_SIZE (39U)
#define COEF_2_CENTER (COEF_2_SIZE >> 1)  // index
#define COEF_5_SIZE (89U)
#define COEF_5_CENTER (COEF_5_SIZE >> 1)  // index


static const int32_t coef_2[COEF_2_SIZE] = {
        754, -593, -5030, -1156, 14685, 11700, -28090, -40657,
        35742, 96944, -17241, -182873, -60232, 289286, 249916, -395287,
        -692918, 474138, 2599603, 3691226, 2599603, 474138, -692918, -395287,
        249916, 289286, -60232, -182873, -17241, 96944, 35742, -40657,
        -28090, 11700, 14685, -1156, -5030, -593, 754
};

static const int32_t coef_5[COEF_5_SIZE] = {
        -259, -587, -862, -823, -226, 1000, 2617, 4030,
        4420, 3040, -389, -5338, -10366, -13391, -12343, -6052,
        4947, 18034, 28870, 32567, 25431, 6752, -20049, -47528,
        -65874, -65882, -42474, 2391, 58598, 109371, 135433, 121005,
        60011, -39474, -154307, -249777, -287196, -233937, -73317, 188510,
        520909, 873334, 1185369, 1399935, 1476364, 1399935, 1185369, 873334,
        520909, 188510, -73317, -233937, -287196, -249777, -154307, -39474,
        60011, 121005, 135433, 109371, 58598, 2391, -42474, -65882,
        -65874, -47528, -20049, 6752, 25431, 32567, 28870, 18034,
        4947, -6052, -12343, -13391, -10366, -5338, -389, 3040,
        4420, 4030, 2617, 1000, -226, -823, -862, -587,
        -259
};

static const float f_scale_in = 0x1p30f;
static const float f_scale_out = 0x1p-30f;

struct filter_s {
    const int32_t * taps;
    uint8_t taps_length;
    uint8_t taps_center;
    uint8_t buffer_idx;
    int64_t buffer[BUFFER_SIZE];
    uint32_t downsample_factor;
    uint32_t downsample_count;
};


struct jsdrv_downsample_s {
    enum jsdrv_downsample_mode_e mode;
    uint32_t sample_rate_in;
    uint32_t sample_rate_out;
    uint32_t decimate_factor;
    uint32_t sample_delay;
    struct filter_s filters[14];  // enough to go from 2 Msps to 1 sps
    uint64_t sample_count;
    int64_t avg;
};


struct jsdrv_downsample_s * jsdrv_downsample_alloc(uint32_t sample_rate_in, uint32_t sample_rate_out, int mode) {
    if (sample_rate_in < sample_rate_out) {
        JSDRV_LOGE("Not downsample: sample_rate_in < sample_rate_out: %lu < %lu",
                   sample_rate_in, sample_rate_out);
        return NULL;
    }
    if (0 == sample_rate_out) {
        JSDRV_LOGE("Cannot downsample: sample_rate_out cannot be 0");
        return NULL;
    }

    uint32_t decimate_factor = sample_rate_in / sample_rate_out;
    if ((sample_rate_out * decimate_factor) != sample_rate_in) {
        JSDRV_LOGE("Cannot downsample: sample_rate_out * M != sample_rate_in");
        return NULL;
    }

    struct jsdrv_downsample_s * self = jsdrv_alloc_clr(sizeof(struct jsdrv_downsample_s));
    if (NULL == self) {
        return NULL;
    }
    self->avg = 0;
    self->sample_rate_in = sample_rate_in;
    self->sample_rate_out = sample_rate_out;
    self->decimate_factor = decimate_factor;

    switch (mode) {
        case JSDRV_DOWNSAMPLE_MODE_AVERAGE:
            self->mode = JSDRV_DOWNSAMPLE_MODE_AVERAGE;
            break;
        case JSDRV_DOWNSAMPLE_MODE_FLAT_PASSBAND:
            self->mode = JSDRV_DOWNSAMPLE_MODE_FLAT_PASSBAND;
            break;
        default:
            jsdrv_free(self);
            JSDRV_LOGE("Unsupported mode: %d", mode);
            return NULL;
    }

    uint32_t idx = 0;
    while (decimate_factor > 1) {
        struct filter_s * f = &self->filters[idx];
        if (0 == (decimate_factor & 1)) {
            f->taps = coef_2;
            f->taps_length = COEF_2_SIZE;
            f->taps_center = COEF_2_CENTER;
            f->downsample_factor = 2;
            decimate_factor >>= 1;
        } else {
            uint32_t d = decimate_factor / 5;
            if (decimate_factor != (d * 5)) {
                JSDRV_LOGE("Cannot downsample: sample_rate_out * M != sample_rate_in");
                jsdrv_downsample_free(self);
                return NULL;
            }
            f->taps = coef_5;
            f->taps_length = COEF_5_SIZE;
            f->taps_center = COEF_5_CENTER;
            f->downsample_factor = 5;
            decimate_factor = d;
        }
        self->sample_delay += f->taps_center;
        ++idx;
        if (idx >= JSDRV_ARRAY_SIZE(self->filters)) {
            JSDRV_LOGE("too much downsampling");
            jsdrv_downsample_free(self);
            return NULL;
        }
    }

    return self;
}

void jsdrv_downsample_clear(struct jsdrv_downsample_s * self) {
    if (NULL == self) {
        return;
    }
    self->sample_count = 0;
    self->avg = 0;
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(self->filters); ++i) {
        self->filters[i].buffer_idx = 0;
        jsdrv_memset(self->filters[i].buffer, 0, sizeof(self->filters[i].buffer));
    }
}

void jsdrv_downsample_free(struct jsdrv_downsample_s * self) {
    if (NULL != self) {
        jsdrv_free(self);
    }
}

uint32_t jsdrv_downsample_decimate_factor(struct jsdrv_downsample_s * self) {
    if (NULL == self) {
        return 1;
    } else {
        return self->decimate_factor;
    }
}

static bool jsdrv_downsample_add_i64q30(struct jsdrv_downsample_s * self, uint64_t sample_id, int64_t x_in, int64_t * x_out) {
    struct filter_s * f;
    if (self->mode == JSDRV_DOWNSAMPLE_MODE_AVERAGE) {
        if (self->sample_count == 0) {
            if (0 != sample_id % self->decimate_factor) {
                // discard until aligned
                return false;
            }
            self->avg = 0;
        }
        self->avg += x_in;
        ++self->sample_count;
        if (self->sample_count >= self->decimate_factor) {
            *x_out = (self->avg / self->sample_count);
            self->sample_count = 0;
            return true;
        }
        return false;
    }

    if (self->sample_count == 0) {
        if (0 != sample_id % self->decimate_factor) {
            // discard until aligned
            return false;
        }
        // seed all buffers with this value
        for (size_t i = 0; i < JSDRV_ARRAY_SIZE(self->filters); ++i) {
            f = &self->filters[i];
            if (0 == f->taps_length) {
                break;
            }
            for (size_t k = 0; k < BUFFER_SIZE; ++k) {
                f->buffer[k] = x_in;
            }
            f->downsample_count = f->downsample_factor;
        }
    }
    ++self->sample_count;

    int64_t x_feed = x_in;
    int64_t x_sum;
    uint32_t fwd_idx;
    uint32_t bwd_idx;

    for (size_t filter_idx = 0; filter_idx < JSDRV_ARRAY_SIZE(self->filters); ++filter_idx) {
        f = &self->filters[filter_idx];
        if (0 == f->taps_length) {
            *x_out = x_feed;
            return true;
        }

        uint8_t buffer_idx = f->buffer_idx;
        f->buffer[buffer_idx] = x_feed;
        f->buffer_idx = (f->buffer_idx + 1) & BUFFER_MASK;
        --f->downsample_count;
        if (0 == f->downsample_count) { // compute filter sample
            fwd_idx = (buffer_idx - f->taps_center) & BUFFER_MASK;
            bwd_idx = fwd_idx;
            if (f->buffer[fwd_idx] == INT64_MIN) {  // NaN
                x_feed = INT64_MIN;
            } else {
                x_feed = f->taps[f->taps_center] * f->buffer[fwd_idx];
                for (uint8_t tap_idx = f->taps_center + 1; tap_idx < f->taps_length; ++tap_idx) {
                    fwd_idx = (fwd_idx + 1) & BUFFER_MASK;
                    bwd_idx = (bwd_idx - 1) & BUFFER_MASK;
                    if ((f->buffer[fwd_idx] == INT64_MIN) || (f->buffer[bwd_idx] == INT64_MIN)) {  // NaN
                        x_feed = INT64_MIN;
                        break;
                    }
                    x_sum = f->buffer[fwd_idx] + f->buffer[bwd_idx];
                    x_feed += x_sum * f->taps[tap_idx];
                }
            }
            if (x_feed != INT64_MIN) {
                x_feed >>= 23;
            }
            f->downsample_count = f->downsample_factor;
        } else {
            break;
        }
    }
    return false;
}

bool jsdrv_downsample_add_f32(struct jsdrv_downsample_s * self, uint64_t sample_id, float x_in, float * x_out) {
    int64_t x64;
    if (NULL == self) {
        *x_out = x_in;
        return true;
    }
    if (isnan(x_in)) {
        x64 = INT64_MIN;
    } else {
        x64 = (int64_t) (x_in * f_scale_in);
    }
    bool rv = jsdrv_downsample_add_i64q30(self, sample_id, x64, &x64);
    if (rv) {
        if (x64 == INT64_MIN) {
            *x_out = NAN;
        } else {
            *x_out = ((float) (x64)) * f_scale_out;
        }
    }
    return rv;
}

bool jsdrv_downsample_add_u8(struct jsdrv_downsample_s * self, uint64_t sample_id, uint8_t x_in, uint8_t * x_out) {
    int64_t x64 = ((int64_t) x_in) << 30;
    bool rv = jsdrv_downsample_add_i64q30(self, sample_id, x64, &x64);
    if (rv) {
        x64 += (1 << 29);  // add 0.5 to so truncation rounds to nearest integer
        *x_out = (x64 < 0) ? 0 : ((uint8_t) (x64 >> 30));
    }
    return rv;
}
