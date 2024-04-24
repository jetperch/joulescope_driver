/*
 * Copyright 2021-2022 Jetperch LLC
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
 * @brief JS220 downsampling.
 */

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef JSDRV_DOWNSAMPLE_H__
#define JSDRV_DOWNSAMPLE_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_downsample Downsample
 *
 * @brief Perform downsampling on a single channel.
 *
 * @{
 */


JSDRV_CPP_GUARD_START

enum jsdrv_downsample_mode_e {
    JSDRV_DOWNSAMPLE_MODE_AVERAGE = 0,
    JSDRV_DOWNSAMPLE_MODE_FLAT_PASSBAND = 1,
};

/// Opaque object
struct jsdrv_downsample_s;

struct jsdrv_downsample_s * jsdrv_downsample_alloc(uint32_t sample_rate_in, uint32_t sample_rate_out, int mode);
void jsdrv_downsample_free(struct jsdrv_downsample_s * self);
void jsdrv_downsample_clear(struct jsdrv_downsample_s * self);
uint32_t jsdrv_downsample_decimate_factor(struct jsdrv_downsample_s * self);
bool jsdrv_downsample_add_f32(struct jsdrv_downsample_s * self, uint64_t sample_id, float x_in, float * x_out);
bool jsdrv_downsample_add_u8(struct jsdrv_downsample_s * self, uint64_t sample_id, uint8_t x_in, uint8_t * x_out);

JSDRV_CPP_GUARD_END

#endif // JSDRV_DOWNSAMPLE_H__
