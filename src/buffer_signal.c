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

#include "jsdrv_prv/buffer_signal.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv/time.h"
#include "jsdrv_prv/js220_i128.h"
#include <inttypes.h>
#include <math.h>
#include <float.h>


const uint64_t SUMMARY_LENGTH_MAX = (sizeof(struct jsdrv_stream_signal_s) - sizeof(struct jsdrv_buffer_response_s)) /
        sizeof(struct jsdrv_summary_entry_s);

void jsdrv_bufsig_alloc(struct bufsig_s * self, uint64_t N, uint64_t r0, uint64_t rN) {
    self->N = N;
    self->r0 = r0;
    self->rN = rN;

    self->k = N / r0;
    self->levels = 1;
    while (self->k >= (rN * rN)) {
        self->k /= rN;
        ++self->levels;
    }

    double size_in_utc = ((double) N) / (self->hdr.sample_rate / self->hdr.decimate_factor);
    self->time_map.counter_rate = ((double) self->hdr.sample_rate) / self->hdr.decimate_factor;
    self->size_in_utc = JSDRV_F64_TO_TIME(size_in_utc);

    if (JSDRV_DATA_TYPE_FLOAT == self->hdr.element_type) {
        JSDRV_ASSERT(self->hdr.element_size_bits == 32);
        self->level0_data = calloc(self->N, sizeof(float));
    } else if (JSDRV_DATA_TYPE_UINT == self->hdr.element_type) {
        if (1 == self->hdr.element_size_bits) {
            self->level0_data = calloc(1, (self->N * self->hdr.element_size_bits + 7) / 8);
        } else if (4 == self->hdr.element_size_bits) {
            self->level0_data = calloc(1, (self->N * self->hdr.element_size_bits + 1) / 2);
        } else {
            JSDRV_ASSERT(false);
        }
    } else {
        JSDRV_ASSERT(false);
    }
    self->level0_head = 0;
    self->level0_size = 0;

    // todo allocate circular summary levels
}

void jsdrv_bufsig_free(struct bufsig_s * self) {
    if (self->level0_data) {
        free(self->level0_data);
        self->level0_data = NULL;
    }
    // todo free summary levels
}

static void samples_to_utc(struct bufsig_s * self,
        struct jsdrv_time_range_samples_s const * samples,
        struct jsdrv_time_range_utc_s * utc) {
    uint64_t length = samples->length;
    utc->start = jsdrv_time_from_counter(&self->time_map, samples->start);
    utc->end = jsdrv_time_from_counter(&self->time_map, samples->end);
    utc->length = length;
}

static void utc_to_samples(struct bufsig_s * self,
                           struct jsdrv_time_range_utc_s const * utc,
                           struct jsdrv_time_range_samples_s * samples) {
    uint64_t length = utc->length;
    samples->start = jsdrv_time_to_counter(&self->time_map, utc->start);
    samples->end = jsdrv_time_to_counter(&self->time_map, utc->end);
    samples->length = length;
}

void jsdrv_bufsig_info(struct bufsig_s * self, struct jsdrv_buffer_info_s * info) {
    memset(info, 0, sizeof(*info));
    info->version = 1;
    info->field_id = self->hdr.field_id;
    info->index = self->hdr.index;
    info->element_type = self->hdr.element_type;
    info->element_size_bits = self->hdr.element_size_bits;
    info->size_in_utc = self->size_in_utc;
    info->size_in_samples = self->N;
    jsdrv_cstr_copy(info->topic, self->topic, sizeof(info->topic));
    info->size_in_utc = self->size_in_utc;
    info->size_in_samples = self->N;
    info->time_range_samples.start = self->sample_id_head - self->level0_size;
    info->time_range_samples.end = self->sample_id_head - 1;
    info->time_range_samples.length = self->level0_size;
    samples_to_utc(self, &info->time_range_samples, &info->time_range_utc);
    info->time_map = self->time_map;
}

static void summarize(struct bufsig_s * self, uint64_t start_idx, uint64_t length) {
    (void) self;
    (void) start_idx;
    (void) length;
    // todo support summary level optimization.
}

static void clear(struct bufsig_s * self, uint64_t sample_id) {
    self->level0_head = 0;
    self->level0_size = 0;
    self->sample_id_head = sample_id;
    self->time_map.offset_counter = sample_id;
    self->time_map.offset_time = jsdrv_time_utc();
}

void jsdrv_bufsig_recv_data(struct bufsig_s * self, struct jsdrv_stream_signal_s * s) {
    self->hdr.sample_id = s->sample_id;
    self->hdr.field_id = s->field_id;
    self->hdr.index = s->index;
    self->hdr.element_type = s->element_type;
    self->hdr.element_size_bits = s->element_size_bits;
    self->hdr.element_count = s->element_count;
    self->hdr.sample_rate = s->sample_rate;
    self->hdr.decimate_factor = s->decimate_factor;

    if (NULL == self->level0_data) {
        return;
    }

    uint64_t length = s->element_count;
    uint8_t * f_src =  s->data;
    uint8_t * f_dst = (uint8_t *) self->level0_data;
    uint64_t sample_id = s->sample_id / self->hdr.decimate_factor;
    uint64_t sample_id_end = sample_id + length - 1;
    uint64_t sample_id_expect = self->sample_id_head;
    if (self->sample_id_head == 0) {
        // initial sample, ignore skips
        // todo device should provide time_map
        clear(self, sample_id);
    } else if (sample_id_end < sample_id_expect) {
        JSDRV_LOGW("bufsig_recv_data %s: duplicate rcv=[%" PRIu64 ", %" PRIu64 "] expect=%" PRIu64,
                   self->topic, sample_id, sample_id_end, sample_id_expect);
        if ((sample_id_expect - sample_id_end) < self->N) {
            clear(self, sample_id);
        }
        return;
    } else if (sample_id < sample_id_expect) {
        JSDRV_LOGW("bufsig_recv_data %s: overlap rcv=[%" PRIu64 ", %" PRIu64 "] expect=%" PRIu64,
                   self->topic, sample_id, sample_id_end, sample_id_expect);
        return;
    } else if (sample_id > sample_id_expect) {
        JSDRV_LOGW("bufsig_recv_data %s: skip rcv=[%" PRIu64 ", %" PRIu64 "] expect=%" PRIu64,
                   self->topic, sample_id, sample_id_end, sample_id_expect);
        if ((sample_id - sample_id_expect) > self->N) {
            clear(self, sample_id);
        } else {
            // todo fill
        }
    }

    // JSDRV_LOGI("bufsig_recv_data: sample_id=%" PRIu64 " length=%" PRIu64, s->sample_id, length);
    while (length) {
        uint64_t head = self->level0_head;
        uint64_t k = length;
        uint64_t head_next = head + length;
        if (head_next > self->N) {
            k = self->N - head;
            head_next = 0;
        }
        uint64_t copy_size = (k * self->hdr.element_size_bits + 7) / 8;
        memcpy(&f_dst[(head * self->hdr.element_size_bits) / 8], f_src, copy_size);
        self->level0_size += k;
        if (self->level0_size > self->N) {
            self->level0_size = self->N;
        }
        summarize(self, head, k);
        f_src += copy_size;
        length -= k;
        self->level0_head = head_next;
        sample_id += k;
    }
    self->sample_id_head = sample_id;
}

static uint64_t level0_tail(struct bufsig_s * self) {
    if (self->level0_size != self->N) {
        return 0;
    } else {
        return self->level0_head;
    }
}

static void rsp_empty(struct jsdrv_buffer_response_s * rsp) {
    rsp->info.time_range_samples.start = 0;
    rsp->info.time_range_samples.end = 0;
    rsp->info.time_range_samples.length = 0;
    rsp->info.time_range_utc.start = 0;
    rsp->info.time_range_utc.end = 0;
    rsp->info.time_range_utc.length = 0;
}

static void samples_get(struct bufsig_s * self, struct jsdrv_buffer_response_s * rsp) {
    rsp->response_type = JSDRV_BUFFER_RESPONSE_SAMPLES;
    uint64_t sample_id = rsp->info.time_range_samples.start;
    uint64_t length = rsp->info.time_range_samples.length;
    uint64_t sample_id_tail = self->sample_id_head - self->level0_size;

    if (self->level0_size == 0) {
        rsp_empty(rsp);
        return;
    }
    if (sample_id < sample_id_tail) {
        uint64_t k = sample_id_tail - sample_id;
        if (k >= length) {  // no data available
            rsp_empty(rsp);
            return;
        }
        length -= k;
        sample_id = sample_id_tail;
        rsp->info.time_range_samples.start = sample_id_tail;
        rsp->info.time_range_samples.length = length;
    }
    if (sample_id >= self->sample_id_head) {
        rsp_empty(rsp);
        return;
    }
    uint64_t sample_id_end = sample_id + length - 1;
    if (sample_id_end >= self->sample_id_head) {
        length = self->sample_id_head - sample_id;
        rsp->info.time_range_samples.end = self->sample_id_head - 1;
    }

    uint8_t * data_rsp = (uint8_t *) rsp->data;
    uint8_t * data_buf = (uint8_t *) self->level0_data;
    uint64_t idx = sample_id - sample_id_tail + level0_tail(self);
    if (idx >= self->N) {
        idx = 0;
    }

    while (length) {
        uint64_t idx_next = idx + length;
        uint64_t k = length;
        if (idx_next >= self->N) {
            k = self->N - idx;
            idx_next = 0;
        }
        uint64_t copy_size = (k * self->hdr.element_size_bits) / 8;
        memcpy(data_rsp, &data_buf[(idx * self->hdr.element_size_bits) / 8], copy_size);
        data_rsp += copy_size;
        length -= k;
        idx = idx_next;
    }
    samples_to_utc(self, &rsp->info.time_range_samples, &rsp->info.time_range_utc);
}

static uint64_t summary_level0_get(struct bufsig_s * self, uint64_t sample_id, uint64_t incr, struct jsdrv_summary_entry_s * y) {
    const uint32_t Q = 30;
    uint64_t src_idx;
    uint64_t tail = level0_tail(self);
    uint64_t sample_id_tail = self->sample_id_head - self->level0_size;
    uint64_t sample_id_end = sample_id + incr;  // one past end
    if ((sample_id_end <= sample_id_tail) || (sample_id >= self->sample_id_head)) {
        y->avg = NAN;
        y->std = NAN;
        y->min = NAN;
        y->max = NAN;
        return 0;
    }
    if (sample_id < sample_id_tail) {
        uint64_t k = sample_id_tail - sample_id;
        sample_id += k;
        incr -= k;
    }
    if (sample_id_end > self->sample_id_head) {
        uint64_t k = sample_id_end - self->sample_id_head;
        incr -= k;
    }

    JSDRV_ASSERT(sample_id >= sample_id_tail);
    JSDRV_ASSERT((sample_id + incr) <= self->sample_id_head);
    uint64_t sample_count = 0;

    if (JSDRV_DATA_TYPE_FLOAT == self->hdr.element_type) {
        float y_min = FLT_MAX;
        float y_max = -FLT_MAX;

        js220_i128 x2;
        x2.u64[0] = 0;
        x2.u64[1] = 0;
        float *src_f32 = (float *) self->level0_data;
        int64_t x1 = 0;

        src_idx = tail + (sample_id - sample_id_tail);
        src_idx %= self->N;
        for (uint64_t k = 0; k < incr; ++k) {
            if (src_idx >= self->N) {
                src_idx %= self->N;
            }
            float f = src_f32[src_idx++];
            if (isnan(f)) {
                continue;
            }
            int64_t x_i64 = (int64_t) (f * (1 << Q));
            x1 += x_i64;
            x2 = js220_i128_add(x2, js220_i128_square_i64(x_i64));
            ++sample_count;
            if (f < y_min) {
                y_min = f;
            }
            if (f > y_max) {
                y_max = f;
            }
        }
        if (sample_count) {
            y->avg = (float) (((double) x1) / ((double) sample_count * (double) (1 << Q)));
            y->std = (float) js220_i128_compute_std(x1, x2, sample_count, Q);
            y->min = y_min;
            y->max = y_max;
        } else {
            y->avg = NAN;
            y->std = NAN;
            y->min = NAN;
            y->max = NAN;
        }
    } else {
        src_idx = (tail << 3) + (sample_id - sample_id_tail);
        uint8_t * src_u8 = (uint8_t *) self->level0_data;
        uint8_t x_u8;
        uint8_t y_min = (uint8_t) ((1 << self->hdr.element_size_bits) - 1);
        uint8_t y_max = 0x00;
        uint64_t x1 = 0;
        js220_i128 x2;
        x2.u64[0] = 0;
        x2.u64[1] = 0;
        for (uint64_t k = 0; k < incr; ++k) {
            if (src_idx >= self->N) {
                src_idx = 0;
            }
            if (1 == self->hdr.element_size_bits) {
                x_u8 = (src_u8[src_idx >> 3] >> (src_idx & 7)) & 1;
            } else if (4 == self->hdr.element_size_bits) {
                x_u8 = src_u8[src_idx >> 1];
                if (src_idx & 1) {
                    x_u8 = (x_u8 >> 4);
                }
                x_u8 &= 0x0f;
            } else {
                // should never get here, only 1 & 4 bits supported
                x_u8 = 0;
            }
            x1 += x_u8;
            x2 = js220_i128_add(x2, js220_i128_square_i64(x_u8));
            if (x_u8 < y_min) {
                y_min = x_u8;
            }
            if (x_u8 > y_max) {
                y_max = x_u8;
            }
            ++src_idx;
        }
        y->avg = (float) (((double) x1) / (double) incr);
        y->std = (float) js220_i128_compute_std(x1, x2, incr, 0);
        y->min = y_min;
        y->max = y_max;
    }
    return sample_count;
}

static void summary_get(struct bufsig_s * self, struct jsdrv_buffer_response_s * rsp) {
    rsp->response_type = JSDRV_BUFFER_RESPONSE_SUMMARY;
    uint64_t sample_id_tail = self->sample_id_head - self->level0_size;
    uint64_t sample_id_start = rsp->info.time_range_samples.start;
    uint64_t sample_id_end = rsp->info.time_range_samples.end;
    uint64_t length = rsp->info.time_range_samples.length;

    uint64_t range_req = sample_id_end + 1 - sample_id_start;
    uint64_t incr = range_req / length;
    if ((range_req / incr) != length) {
        JSDRV_LOGI("summary request: adjusting increment %f -> %" PRIu64,
                   (double) range_req / (double) length, incr);
    }

    if (self->level0_size == 0) {
        JSDRV_LOGI("summary request: buffer empty");
        rsp->info.time_range_samples.length = 0;
        rsp->info.time_range_utc.length = 0;
    } else if (length == 0) {
        JSDRV_LOGI("summary request: length == 0");
        rsp->info.time_range_utc.length = 0;
        return;
    } else if (length > SUMMARY_LENGTH_MAX) {
        JSDRV_LOGI("summary request: length too long: %" PRIu64 " > %" PRIu64,
                   length, SUMMARY_LENGTH_MAX);
        rsp->info.time_range_samples.length = 0;
        rsp->info.time_range_utc.length = 0;
        return;
    }

    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    struct jsdrv_summary_entry_s * y;
    uint32_t dst_idx = 0;
    uint64_t sample_id = sample_id_start;
    uint32_t out_of_range_count = 0;

    while (dst_idx < length) {
        uint64_t sample_id_next = sample_id + incr;
        y = &entries[dst_idx++];
        if (0 == summary_level0_get(self, sample_id, incr, y)) {
            ++out_of_range_count;
        }
        sample_id = sample_id_next;
    }

    if (out_of_range_count) {
        JSDRV_LOGI("summary request: out_of_range_count == %" PRIu32
                   " fs=%.0f,"
                   " req=[%" PRIu64 ", %" PRIu64 "], buf=[%" PRIu64 ", %" PRIu64 "]",
                   out_of_range_count, self->time_map.counter_rate,
                   sample_id_start, sample_id_start + length * incr,
                   sample_id_tail, self->sample_id_head);
    }

    samples_to_utc(self, &rsp->info.time_range_samples, &rsp->info.time_range_utc);
}

void jsdrv_bufsig_process_request(
        struct bufsig_s * self,
        struct jsdrv_buffer_request_s * req,
        struct jsdrv_buffer_response_s * rsp) {
    rsp->version = 1;
    rsp->response_type = 0;
    rsp->rsv1_u8 = 0;
    rsp->rsv2_u8 = 0;
    rsp->rsv3_u32 = 0;
    rsp->rsp_id = req->rsp_id;
    jsdrv_bufsig_info(self, &rsp->info);

    if (JSDRV_TIME_SAMPLES == req->time_type) {
        // no action needed
    } else if (JSDRV_TIME_UTC == req->time_type) {
        utc_to_samples(self, &req->time.utc, &req->time.samples);
    } else {
        JSDRV_LOGW("invalid time_type: %d", (int) req->time_type);
        return;
    }
    rsp->info.time_range_samples = req->time.samples;
    struct jsdrv_time_range_samples_s * r = &rsp->info.time_range_samples;
    uint64_t interval = r->end - r->start + 1;
    if (r->length && r->end) {
        if (r->length >= interval) {
            r->length = interval;
            samples_get(self, rsp);
        } else {
            summary_get(self, rsp);
        }
    } else if (req->time.samples.length) {
        r->end = r->start + r->length - 1;
        samples_get(self, rsp);
    } else {
        r->length = interval;
        samples_get(self, rsp);
    }
}
