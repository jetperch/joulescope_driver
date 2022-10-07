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

#ifndef JSDRV_SAMPLE_BUFFER_F32_H__
#define JSDRV_SAMPLE_BUFFER_F32_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

#define SAMPLE_BUFFER_LENGTH (1024)  // must be power of 2
#define SAMPLE_BUFFER_MASK (SAMPLE_BUFFER_LENGTH - 1)

JSDRV_CPP_GUARD_START


struct sbuf_f32_s {
    uint64_t head_sample_id;
    uint32_t head;
    uint32_t tail;
    uint8_t sample_id_decimate;
    uint32_t msg_sample_id;
    float buffer[SAMPLE_BUFFER_LENGTH];
};

/**
 * @brief Clear the buffer instance.
 *
 * @param self The buffer instance.
 */
void sbuf_f32_clear(struct sbuf_f32_s * self);

/**
 * @brief Get the length of data in the buffer.
 * @param self The buffer instance.
 * @return The number of float32 values in the buffer.
 */
uint32_t sbuf_f32_length(struct sbuf_f32_s * self);

/**
 * @brief The sample ID of the oldest sample in the buffer.
 *
 * @param self The buffer instance.
 * @return The tail sample ID.
 */
uint64_t sbuf_head_sample_id(struct sbuf_f32_s * self);

/**
 * @brief The sample ID of the newest sample in the buffer.
 *
 * @param self The buffer instance.
 * @return The head sample ID.
 */
uint64_t sbuf_tail_sample_id(struct sbuf_f32_s * self);

/**
 * @brief Add new data to the buffer.
 *
 * @param self The buffer instance.
 * @param sample_id The starting sample id for data.
 * @param data The data samples.
 * @param length The number of data samples.
 */
void sbuf_f32_add(struct sbuf_f32_s * self, uint64_t sample_id, float * data, uint32_t length);

/**
 * @brief Advance buffer tail to the given sample id.
 *
 * @param self The buffer instance.
 * @param sample_id The new tail sample id, if greater than
 *  the existing sample id.  Otherwise, ignore.
 */
void sbuf_f32_advance(struct sbuf_f32_s * self, uint64_t sample_id);

/**
 * @brief Multiply the data in overlapping regions.
 *
 * @param r The resulting multiplied values.
 * @param s1 The source 1 values.
 * @param s2 The source 2 values.
 */
void sbuf_f32_mult(struct sbuf_f32_s * r, struct sbuf_f32_s * s1, struct sbuf_f32_s * s2);


JSDRV_CPP_GUARD_END

#endif /* JSDRV_SAMPLE_BUFFER_F32_H__ */
