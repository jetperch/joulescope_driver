/*
* Copyright 2023 Jetperch LLC
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
 * @brief Memory buffer for samples from a single signal.
 */

#ifndef JSDRV_PRV_BUFFER_SIGNAL_H_
#define JSDRV_PRV_BUFFER_SIGNAL_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/msg_queue.h"
#include <stdint.h>


struct buffer_s;

struct bufsig_stream_header_s {
    uint64_t sample_id;                     ///< the starting sample id, which increments by decimate_factor.
    uint8_t field_id;                       ///< jsdrv_field_e
    uint8_t index;                          ///< The channel index within the field.
    uint8_t element_type;                   ///< jsdrv_element_type_e
    uint8_t element_size_bits;              ///< The element size in bits
    uint32_t element_count;                 ///< size of data in elements
    uint32_t sample_rate;                   ///< The frequency for sample_id.
    uint32_t decimate_factor;               ///< The decimation factor from sample_id to data samples.
};


struct bufsig_s {
    uint32_t idx;
    bool active;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct buffer_s * parent;
    struct bufsig_stream_header_s hdr;
    double sample_rate;

    uint64_t N;             // size in samples
    int64_t size_in_utc;    // size in UTC time
    uint64_t r0;
    uint64_t rN;
    uint64_t k;
    uint8_t levels;

    // todo summary data.

    // level 0
    // length in N
    uint64_t level0_head;  // next insert point (also tail when full)
    uint64_t level0_size;
    void * level0_data;
    uint64_t sample_id_head;
};

/**
 * @brief Allocate the sample buffer and reductions.
 *
 * @param self The buffer instance.
 * @param N The total number of samples to store.
 * @param r0 The number of samples in the first reduction.
 * @param rN The number of samples in subsequent reductions.
 */
void jsdrv_bufsig_alloc(struct bufsig_s * self, uint64_t N, uint64_t r0, uint64_t rN);

void jsdrv_bufsig_free(struct bufsig_s * self);

void jsdrv_bufsig_recv_data(struct bufsig_s * self, struct jsdrvp_msg_s * msg);

void jsdrv_bufsig_info(struct bufsig_s * self, struct jsdrv_buffer_info_s * info);

/**
 * @brief Process a request.
 *
 * @param self The buffer instance.
 * @param req The request to process.
 * @param rsp The response to populate which is mostly populated by the caller.
 *      This function simply needs to add the data and update
 *      rsp->info fields size_in_utc, time_range_utc,
 *      size_in_samples, time_range_samples.
 */
void jsdrv_bufsig_process_request(
        struct bufsig_s * self,
        struct jsdrv_buffer_request_s * req,
        struct jsdrv_buffer_response_s * rsp);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_BUFFER_SIGNAL_H_ */
