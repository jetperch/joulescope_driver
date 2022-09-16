/*
 * Copyright 2018-2022 Jetperch LLC
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
 * @brief JS110 raw sample processor.
 */

#ifndef JSDRV_JS110_SAMPLE_PROCESSOR_H__
#define JSDRV_JS110_SAMPLE_PROCESSOR_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

JSDRV_CPP_GUARD_START

#define JS110_SUPPRESS_SAMPLES_MAX 64  // must be power of 2
#define JS110_I_RANGE_MISSING 8

enum js110_supress_mode_e {
    JS110_SUPPRESS_MODE_OFF       = 0,    // disabled, force zero delay
    JS110_SUPPRESS_MODE_MEAN      = 1,
    JS110_SUPPRESS_MODE_INTERP    = 2,
    JS110_SUPPRESS_MODE_NAN       = 3,
};


struct js110_sample_s {
    float i;
    float v;
    float p;
    uint8_t current_range;
    uint8_t gpi0;
    uint8_t gpi1;
    uint8_t reserved_u8;
};

struct js110_sp_s {
    double cal[2][2][9];  // current/voltage, offset/gain

    struct js110_sample_s samples[JS110_SUPPRESS_SAMPLES_MAX];
    uint8_t head;

    int32_t is_skipping;
    int32_t _idx_suppress_start;
    int32_t _idx_out;
    uint64_t sample_missing_count;  // based upon sample_id
    uint64_t skip_count;            // number of sample skips
    uint64_t sample_sync_count;     // based upon alternating 0/1 pattern
    uint64_t contiguous_count;      //
    uint64_t sample_count;

    uint8_t _i_range_last;
    int32_t _suppress_samples_pre;      // the number of samples to use before range change
    int32_t _suppress_samples_window;   // the total number of samples to suppress after range change
    int32_t _suppress_samples_post;
    const uint8_t (*_suppress_matrix)[9][9];

    int32_t _suppress_samples_remaining;  // the suppress counter, 0 = idle, 1 = take action, >1 = decrement
    int32_t _suppress_samples_counter;
    uint8_t _suppress_mode;

    uint16_t sample_toggle_last;
    uint16_t sample_toggle_mask;
    uint8_t _voltage_range;
};


void js110_sp_initialize(struct js110_sp_s * self);

void js110_sp_reset(struct js110_sp_s * self);

struct js110_sample_s js110_sp_process(struct js110_sp_s * self, uint32_t sample_u32, uint8_t v_range);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_JS110_SAMPLE_PROCESSOR_H__ */
