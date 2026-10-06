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

/**
 * @file
 *
 * @brief sincN (CIC) downsampling.
 */

#include "jsdrv_prv/downsample_sinc.h"
#include "jsdrv_prv/devices/js220/js220_i128.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include <math.h>
#include <limits.h>

#define FACTOR_MIN (2U)
#define FACTOR_MAX (1000U)
#define ORDER_MIN  (1U)
#define ORDER_MAX  (3U)

static const float f_scale_in = 0x1p30f;
static const float f_scale_out = 0x1p-30f;

struct jsdrv_downsample_sinc_s {
    uint32_t factor;         // decimation factor R
    uint32_t order;          // sinc order (CIC order)
    uint32_t kernel_length;  // order * (factor - 1) + 1
    uint32_t ring_idx;       // next write position == oldest sample
    uint64_t norm;           // factor^order == sum(kernel)
    uint64_t sample_count;   // accepted samples since alloc/clear
    uint32_t * kernel;       // [kernel_length] box^order weights, symmetric
    int64_t * ring;          // [kernel_length] Q30 history; INT64_MIN = NaN
};

// Convolve kernel[0..len) with a box of `factor` ones, in place.  The
// output (length len + factor - 1) at index i only reads inputs at
// indices <= i, so computing with a descending output index never
// clobbers an unread input.
static void kernel_convolve_box(uint32_t * kernel, uint32_t len, uint32_t factor) {
    uint32_t out_len = len + factor - 1U;
    for (uint32_t i = out_len; i-- > 0U; ) {
        uint32_t j_lo = (i >= (factor - 1U)) ? (i - (factor - 1U)) : 0U;
        uint32_t j_hi = (i < len) ? i : (len - 1U);
        uint32_t sum = 0U;
        for (uint32_t j = j_lo; j <= j_hi; ++j) {
            sum += kernel[j];
        }
        kernel[i] = sum;
    }
}

struct jsdrv_downsample_sinc_s * jsdrv_downsample_sinc_alloc(uint32_t factor, uint32_t order) {
    if ((factor < FACTOR_MIN) || (factor > FACTOR_MAX)) {
        JSDRV_LOGE("invalid factor: %lu", (unsigned long) factor);
        return NULL;
    }
    if ((order < ORDER_MIN) || (order > ORDER_MAX)) {
        JSDRV_LOGE("invalid order: %lu", (unsigned long) order);
        return NULL;
    }
    uint32_t kernel_length = order * (factor - 1U) + 1U;
    size_t sz = sizeof(struct jsdrv_downsample_sinc_s)
            + ((size_t) kernel_length * sizeof(int64_t))
            + ((size_t) kernel_length * sizeof(uint32_t));
    struct jsdrv_downsample_sinc_s * self = jsdrv_alloc_clr(sz);
    if (NULL == self) {
        return NULL;
    }
    self->factor = factor;
    self->order = order;
    self->kernel_length = kernel_length;
    self->ring = (int64_t *) (self + 1);
    self->kernel = (uint32_t *) (self->ring + kernel_length);

    uint64_t norm = factor;
    for (uint32_t k = 0; k < factor; ++k) {
        self->kernel[k] = 1U;
    }
    uint32_t len = factor;
    for (uint32_t o = 1U; o < order; ++o) {
        kernel_convolve_box(self->kernel, len, factor);
        len += factor - 1U;
        norm *= factor;
    }
    self->norm = norm;
    return self;
}

void jsdrv_downsample_sinc_free(struct jsdrv_downsample_sinc_s * self) {
    if (NULL != self) {
        jsdrv_free(self);
    }
}

void jsdrv_downsample_sinc_clear(struct jsdrv_downsample_sinc_s * self) {
    if (NULL == self) {
        return;
    }
    self->sample_count = 0;
    self->ring_idx = 0;
}

uint32_t jsdrv_downsample_sinc_decimate_factor(struct jsdrv_downsample_sinc_s * self) {
    return (NULL == self) ? 1U : self->factor;
}

uint32_t jsdrv_downsample_sinc_order(struct jsdrv_downsample_sinc_s * self) {
    return (NULL == self) ? 0U : self->order;
}

bool jsdrv_downsample_sinc_add_f32(struct jsdrv_downsample_sinc_s * self,
                                   uint64_t sample_id, float x_in, float * y_out) {
    if (NULL == self) {
        *y_out = x_in;
        return true;
    }
    int64_t x64;
    if (isnan(x_in)) {
        x64 = INT64_MIN;
    } else {
        x64 = (int64_t) (x_in * f_scale_in);
    }
    if (0 == self->sample_count) {
        if (0 != (sample_id % self->factor)) {
            // discard until aligned
            return false;
        }
        // seed the history with this value to suppress the startup transient
        for (uint32_t k = 0; k < self->kernel_length; ++k) {
            self->ring[k] = x64;
        }
        self->ring_idx = 0;
    }
    self->ring[self->ring_idx] = x64;
    ++self->ring_idx;
    if (self->ring_idx >= self->kernel_length) {
        self->ring_idx = 0;
    }
    ++self->sample_count;
    if (0 != (self->sample_count % self->factor)) {
        return false;
    }

    // Emit: weighted sum of the most recent kernel_length samples.
    // ring_idx is the oldest sample; the kernel is symmetric, so the
    // pairing orientation is irrelevant as long as it is consistent.
    js220_i128 acc = js220_i128_init_i64(0);
    uint32_t idx = self->ring_idx;
    for (uint32_t k = 0; k < self->kernel_length; ++k) {
        int64_t v = self->ring[idx];
        if (INT64_MIN == v) {  // NaN
            *y_out = NAN;
            return true;
        }
        acc = js220_i128_add(acc, js220_i128_init_i64(((int64_t) self->kernel[k]) * v));
        ++idx;
        if (idx >= self->kernel_length) {
            idx = 0;
        }
    }

    // Divide by norm, rounding half away from zero.  udiv is unsigned,
    // so negate a negative accumulator before dividing and negate back.
    js220_i128 half = js220_i128_init_i64((int64_t) (self->norm / 2U));
    bool is_neg = js220_i128_is_neg(acc);
    if (is_neg) {
        acc = js220_i128_neg(acc);
    }
    acc = js220_i128_add(acc, half);
    acc = js220_i128_udiv(acc, self->norm, NULL);
    if (is_neg) {
        acc = js220_i128_neg(acc);
    }
    *y_out = ((float) acc.i64[0]) * f_scale_out;
    return true;
}
