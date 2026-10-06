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

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef JSDRV_DOWNSAMPLE_SINC_H__
#define JSDRV_DOWNSAMPLE_SINC_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_downsample_sinc Downsample sincN
 *
 * @brief Downsample a single channel with a sincN (CIC order N) filter.
 *
 * Matches the JS320 on-instrument decimation filters so the host can
 * continue decimating below the instrument's minimum output rate with
 * the same frequency response family: sinc1 = moving average (box),
 * sinc2 = box⊗box (triangle), sinc3 = box⊗box⊗box.
 *
 * @{
 */


JSDRV_CPP_GUARD_START

/// Opaque object
struct jsdrv_downsample_sinc_s;

/**
 * @brief Allocate a sincN decimation filter.
 *
 * @param factor The decimation factor R, in [2, 1000].
 * @param order The sinc order, in [1, 3] (sinc1, sinc2, sinc3).
 * @return The instance or NULL on invalid arguments or out of memory.
 */
struct jsdrv_downsample_sinc_s * jsdrv_downsample_sinc_alloc(uint32_t factor, uint32_t order);
void jsdrv_downsample_sinc_free(struct jsdrv_downsample_sinc_s * self);

/**
 * @brief Reset the filter state.  The next add realigns and reseeds.
 */
void jsdrv_downsample_sinc_clear(struct jsdrv_downsample_sinc_s * self);

/// The decimation factor R, or 1 if self is NULL.
uint32_t jsdrv_downsample_sinc_decimate_factor(struct jsdrv_downsample_sinc_s * self);

/// The sinc order, or 0 if self is NULL.
uint32_t jsdrv_downsample_sinc_order(struct jsdrv_downsample_sinc_s * self);

/**
 * @brief Add a sample to the filter.
 *
 * @param self The instance, or NULL to pass through unmodified.
 * @param sample_id The sample id in input-sample ticks.  On the first
 *      sample after alloc/clear, samples are discarded until
 *      (sample_id % factor) == 0, then the filter history is seeded
 *      with the first accepted value to suppress the startup transient.
 * @param x_in The input sample.  |x_in| must be < 8192 for the
 *      fixed-point Q30 tap products to fit in int64 (any real JS320
 *      i/v/p value is far smaller).
 * @param[out] y_out The output sample, valid when this returns true.
 * @return true when an output sample was produced (once per factor
 *      input samples), false otherwise.
 */
bool jsdrv_downsample_sinc_add_f32(struct jsdrv_downsample_sinc_s * self,
                                   uint64_t sample_id, float x_in, float * y_out);

JSDRV_CPP_GUARD_END

/** @} */

#endif // JSDRV_DOWNSAMPLE_SINC_H__
