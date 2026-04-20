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

/**
 * @file
 *
 * @brief JS320 statistics conversion.
 */

#include "jsdrv/cmacro_inc.h"
#include "jsdrv_prv/devices/js220/js220_api.h"
#include <stdint.h>

#ifndef JSDRV_JS320_STATS_H__
#define JSDRV_JS320_STATS_H__

JSDRV_CPP_GUARD_START

struct jsdrv_statistics_s;

struct js320_channel_raw_s {
    js220_i128 x1;      ///< sum(x), 76Q52
    js220_i128 x2;      ///< sum(x^2), truncated: (x*x)[127:32]
    js220_i128 integ;    ///< integration accumulator, 76Q52
    int64_t min;         ///< 12Q52
    int64_t max;         ///< 12Q52
};

struct js320_statistics_raw_s {
    uint32_t header;             ///< [31:24]=version, [23:16]=type, [15:0]=rsv
    uint32_t decimate;           ///< sample_id rate / sample rate = 16
    uint32_t sample_freq;        ///< sample_id rate = 16,000,000
    uint32_t sample_count;       ///< number of samples in this block
    uint64_t block_sample_id;    ///< first sample_id in this block
    uint64_t accum_sample_id;    ///< first sample_id in integration
    struct js320_channel_raw_s i;
    struct js320_channel_raw_s v;
    struct js320_channel_raw_s p;
};

/**
 * @brief Validate and convert the JS320 statistics data structure.
 *
 * @param src The JS320 raw statistics data structure.
 * @param dst The JSDRV statistics data structure.
 * @return 0 or error code.
 */
int32_t js320_stats_convert(
    const struct js320_statistics_raw_s * src,
    struct jsdrv_statistics_s * dst);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_JS320_STATS_H__ */
