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
#include "jsdrv_prv/statistics.h"
#include <inttypes.h>
#include <math.h>
#include <float.h>


const uint64_t SUMMARY_LENGTH_MAX = (sizeof(struct jsdrv_stream_signal_s) - sizeof(struct jsdrv_buffer_response_s)) /
        sizeof(struct jsdrv_summary_entry_s);

static uint64_t summary_level0_get_by_idx(struct bufsig_s * self, uint64_t index, uint64_t incr, struct jsdrv_summary_entry_s * y);

static void entry_clear(struct jsdrv_summary_entry_s * y) {
    y->avg = NAN;
    y->std = NAN;
    y->min = NAN;
    y->max = NAN;
}

static struct jsdrv_summary_entry_s * level_entry(struct bufsig_s * self, uint8_t level, uint64_t idx) {
    JSDRV_ASSERT(level > 0);
    JSDRV_ASSERT(level < JSDRV_BUFSIG_LEVELS_MAX);
    struct bufsig_level_s * lvl = &self->levels[level - 1];
    if (idx >= lvl->k) {
        return NULL;
    }
    JSDRV_ASSERT(idx < lvl->k);
    return &lvl->data[idx];
}

void jsdrv_bufsig_alloc(struct bufsig_s * self, uint64_t N, uint64_t r0, uint64_t rN) {
    JSDRV_LOGI("jsdrv_bufsig_alloc %d N=%" PRIu64 ", r0=%" PRIu64", rN=%" PRIu64,
               (int) self->idx, N, r0, rN);
    self->N = N;
    self->r0 = r0;
    self->rN = rN;

    self->k = N / r0;
    self->level_count = 1;
    while (self->k >= (rN * rN)) {
        self->k /= rN;
        ++self->level_count;
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

    uint64_t samples_per_entry = 1;
    for (int i = 0; i < JSDRV_BUFSIG_LEVELS_MAX; ++i) {
        uint64_t r = (i == 0) ? r0 : rN;
        samples_per_entry *= r;
        uint64_t k = N / samples_per_entry;
        if (k <= 1) {
            break;
        }
        struct bufsig_level_s *lvl = &self->levels[i];
        lvl->k = k;
        lvl->r = r;
        lvl->samples_per_entry = samples_per_entry;
        JSDRV_LOGD3("alloc lvl=%d %" PRIu64, i + 1, k);
        lvl->data = malloc(k * sizeof(struct jsdrv_summary_entry_s));
    }
}

void jsdrv_bufsig_free(struct bufsig_s * self) {
    JSDRV_LOGI("jsdrv_bufsig_free %d", (int) self->idx);
    for (int i = 0; i < JSDRV_BUFSIG_LEVELS_MAX; ++i) {
        if (NULL != self->levels[i].data) {
            free(self->levels[i].data);
            self->levels[i].data = NULL;
        }
    }
    if (self->level0_data) {
        free(self->level0_data);
        self->level0_data = NULL;
    }
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

static void summarizeN(struct bufsig_s * self, uint8_t level, uint64_t start_idx, uint64_t length) {
    struct bufsig_level_s * lvl_dn = &self->levels[level - 1];
    struct bufsig_level_s * lvl_up = &self->levels[level];
    if (NULL == lvl_up->data) {
        return;
    }

    uint64_t length_orig = length;
    uint64_t lvl_up_idx = start_idx / lvl_up->samples_per_entry;
    uint64_t level0_idx = lvl_up_idx * lvl_up->samples_per_entry;
    uint64_t lvl_dn_idx = level0_idx / lvl_dn->samples_per_entry;
    length += start_idx - level0_idx;
    struct jsdrv_summary_entry_s * src;
    struct jsdrv_summary_entry_s * dst;
    struct jsdrv_statistics_accum_s s_accum;
    jsdrv_statistics_reset(&s_accum);
    struct jsdrv_statistics_accum_s s_tmp;
    while (length >= lvl_up->samples_per_entry) {
        for (uint64_t i = 0; i < lvl_up->r; ++i) {
            src = level_entry(self, level, lvl_dn_idx + i);
            jsdrv_statistics_from_entry(&s_tmp, src, 1);  // unweighted, all entries equal
            jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
        }
        dst = level_entry(self, level + 1, lvl_up_idx);
        jsdrv_statistics_to_entry(&s_accum, dst);
        lvl_up_idx = (lvl_up_idx + 1) % lvl_up->k;
        lvl_dn_idx = (lvl_dn_idx + lvl_up->r) % lvl_dn->k;
        length -= lvl_up->samples_per_entry;
    }
    summarizeN(self, level + 1, start_idx, length_orig);
}

static void summarize(struct bufsig_s * self, uint64_t start_idx, uint64_t length) {
    struct bufsig_level_s * lvl1 = &self->levels[0];
    if (NULL == lvl1->data) {
        return;
    }
    uint64_t length_orig = length;
    uint64_t level1_idx_init = start_idx / self->r0;
    uint64_t level1_idx = level1_idx_init;
    uint64_t level0_idx = level1_idx * self->r0;
    length += (start_idx - level0_idx);
    while (length >= self->r0) {
        struct jsdrv_summary_entry_s * y = level_entry(self, 1, level1_idx);
        summary_level0_get_by_idx(self, level0_idx, self->r0, y);
        length -= self->r0;
        level1_idx = (level1_idx + 1) % lvl1->k;
        level0_idx = (level0_idx + self->r0) % self->N;
    }

    summarizeN(self, 1, start_idx, length_orig);
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
            uint64_t k = sample_id - sample_id_expect;
            if (s->element_type == JSDRV_DATA_TYPE_FLOAT) {
                if (s->element_size_bits == 32) {
                    // fill float32 with NaN
                    uint64_t idx = self->level0_head;
                    float *f32 = (float *) self->level0_data;
                    for (uint64_t i = 0; i < k; ++i) {
                        if (idx >= self->N) {
                            idx = idx % self->N;
                        }
                        f32[idx] = NAN;
                        ++idx;
                    }
                } else if (s->element_size_bits == 64) {
                    // fill float64 with NaN
                    uint64_t idx = self->level0_head;
                    double *f64 = (double *) self->level0_data;
                    for (uint64_t i = 0; i < k; ++i) {
                        if (idx >= self->N) {
                            idx = idx % self->N;
                        }
                        f64[idx] = NAN;
                        ++idx;
                    }
                }
            } else {
                // fill integer types with zeros
                uint64_t size = (k * s->element_size_bits + 7) / 8;
                uint8_t * data = (uint8_t *) self->level0_data;
                if ((self->level0_head + size) > self->N) {
                    uint64_t size1 = ((self->N - self->level0_head) * s->element_size_bits + 7) / 8;
                    uint64_t size2 = size - size1;
                    memset(&data[self->level0_head], 0, size1);
                    memset(&data[0], 0, size2);
                } else {
                    memset(&data[self->level0_head], 0, size);
                }
            }
            uint64_t start_idx = self->level0_head;
            self->level0_size += k;
            if (self->level0_size > self->N) {
                self->level0_size = self->N;
            }
            self->level0_head = (self->level0_head + k) % self->N;
            summarize(self, start_idx, k);
        }
    }

    // JSDRV_LOGI("bufsig_recv_data: sample_id=%" PRIu64 " length=%" PRIu64, s->sample_id, length);
    self->sample_id_head = sample_id;
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
        f_src += copy_size;
        length -= k;
        self->level0_head = head_next;
        self->sample_id_head += k;
        sample_id += k;
        summarize(self, head, k);
    }
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
    uint64_t idx = (sample_id - sample_id_tail + level0_tail(self)) % self->N;

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

    uint8_t shift = 0;
    if (1 == self->hdr.element_size_bits) {
        shift = idx & 7;
    } else if (4 == self->hdr.element_size_bits) {
        shift = (idx & 1) << 2;
    }
    if (shift) {
        uint8_t shift_left = 64 - shift;
        uint64_t * p = (uint64_t *) rsp->data;
        uint64_t fwd = p[0];
        for (uint64_t i = 1; i < rsp->info.time_range_samples.length; ++i) {
            uint64_t z = p[i];
            p[i - 1] = (fwd >> shift) | (z << shift_left);
            fwd = z;
        }
    }

    samples_to_utc(self, &rsp->info.time_range_samples, &rsp->info.time_range_utc);
}

static uint64_t summary_level0_get_by_idx(struct bufsig_s * self, uint64_t index, uint64_t incr, struct jsdrv_summary_entry_s * y) {
    uint64_t sample_count = 0;
    const uint32_t Q = 30;
    JSDRV_ASSERT(index < self->N);
    JSDRV_ASSERT(incr <= self->N);

    if (0 == incr) {
        entry_clear(y);
        return 0;
    }

    if (JSDRV_DATA_TYPE_FLOAT == self->hdr.element_type) {
        float y_min = FLT_MAX;
        float y_max = -FLT_MAX;

        js220_i128 x2;
        x2.u64[0] = 0;
        x2.u64[1] = 0;
        float *src_f32 = (float *) self->level0_data;
        int64_t x1 = 0;

        for (uint64_t k = 0; k < incr; ++k) {
            if (index >= self->N) {
                index %= self->N;
            }
            float f = src_f32[index++];
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
            entry_clear(y);
        }
    } else {
        uint8_t * src_u8 = (uint8_t *) self->level0_data;
        uint8_t x_u8;
        uint8_t y_min = (uint8_t) ((1 << self->hdr.element_size_bits) - 1);
        uint8_t y_max = 0x00;
        uint64_t x1 = 0;
        js220_i128 x2;
        x2.u64[0] = 0;
        x2.u64[1] = 0;
        for (uint64_t k = 0; k < incr; ++k) {
            if (index >= self->N) {
                index = index % self->N;
            }
            if (1 == self->hdr.element_size_bits) {
                x_u8 = (src_u8[index >> 3] >> (index & 7)) & 1;
            } else if (4 == self->hdr.element_size_bits) {
                x_u8 = src_u8[index >> 1];
                if (index & 1) {
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
            ++index;
        }
        y->avg = (float) (((double) x1) / (double) incr);
        y->std = (float) js220_i128_compute_std(x1, x2, incr, 0);
        y->min = y_min;
        y->max = y_max;
    }
    return sample_count;
}

static uint64_t summary_level0_get(struct bufsig_s * self, uint64_t sample_id, uint64_t incr, struct jsdrv_summary_entry_s * y) {
    uint64_t src_idx;
    uint64_t tail = level0_tail(self);
    uint64_t sample_id_tail = self->sample_id_head - self->level0_size;
    uint64_t sample_id_end = sample_id + incr;  // exclusive, one past end
    if ((incr == 0) || (sample_id_end <= sample_id_tail) || (sample_id >= self->sample_id_head)) {
        entry_clear(y);
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
    if (incr == 0) {
        entry_clear(y);
        return 0;
    }

    JSDRV_ASSERT(sample_id >= sample_id_tail);
    JSDRV_ASSERT((sample_id + incr) <= self->sample_id_head);
    src_idx = tail + (sample_id - sample_id_tail);
    src_idx = src_idx % self->N;
    return summary_level0_get_by_idx(self, src_idx, incr, y);
}

static void summary_get(struct bufsig_s * self, struct jsdrv_buffer_response_s * rsp) {
    rsp->response_type = JSDRV_BUFFER_RESPONSE_SUMMARY;
    uint64_t sample_id_tail = self->sample_id_head - self->level0_size;
    uint64_t sample_id_start = rsp->info.time_range_samples.start;
    uint64_t sample_id_end = rsp->info.time_range_samples.end;
    uint64_t entries_length = rsp->info.time_range_samples.length;

    uint64_t range_req = sample_id_end + 1 - sample_id_start;
    uint64_t incr = range_req / entries_length;
    if ((range_req / incr) != entries_length) {
        JSDRV_LOGI("summary request: adjusting increment %f -> %" PRIu64,
                   (double) range_req / (double) entries_length, incr);
    }

    if (self->level0_size == 0) {
        JSDRV_LOGI("summary request: buffer empty");
        rsp->info.time_range_samples.length = 0;
        rsp->info.time_range_utc.length = 0;
    } else if (entries_length == 0) {
        JSDRV_LOGI("summary request: length == 0");
        rsp->info.time_range_utc.length = 0;
        return;
    } else if (entries_length > SUMMARY_LENGTH_MAX) {
        JSDRV_LOGI("summary request: length too long: %" PRIu64 " > %" PRIu64,
                   entries_length, SUMMARY_LENGTH_MAX);
        rsp->info.time_range_samples.length = 0;
        rsp->info.time_range_utc.length = 0;
        return;
    }

    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    struct jsdrv_summary_entry_s * src;
    uint64_t sample_id = sample_id_start;

    struct jsdrv_statistics_accum_s s_tmp;
    struct jsdrv_statistics_accum_s s_accum;
    jsdrv_statistics_reset(&s_accum);

    uint8_t level = 0;
    uint8_t tgt_level;
    for (tgt_level = 0; tgt_level < JSDRV_BUFSIG_LEVELS_MAX; ++tgt_level) {
        if (incr < self->levels[tgt_level].samples_per_entry) {
            break;
        }
    }

    uint64_t remaining = incr;
    uint64_t idx;
    uint64_t valid_count = 0;

    for (uint64_t entry_idx = 0; entry_idx < entries_length; ++entry_idx) {
        uint64_t s_start = sample_id + incr * entry_idx;  // inclusive
        uint64_t s_end = s_start + incr;                  // exclusive
        if (s_end > self->sample_id_head) {
            uint64_t dsize = s_end - self->sample_id_head;
            remaining = (dsize > remaining) ? 0 : (remaining - dsize);
            s_end -= dsize;
        }
        struct jsdrv_summary_entry_s * dst = &entries[entry_idx];
        if ((s_end <= sample_id_tail) || (s_start >= self->sample_id_head)) {
            // completely out of range
            entry_clear(dst);
            continue;
        }
        if (0 == tgt_level) {
            summary_level0_get(self, s_start, incr, dst);
            continue;
        }

        idx = ((self->level0_head + self->N) - (self->sample_id_head - s_start)) % self->N;
        if (0 == valid_count) {
            ++valid_count;
            uint64_t lvl_dn_step = 1;
            uint64_t lvl_up_step = self->r0;
            uint64_t lvl1_idx = (idx + self->levels[0].samples_per_entry - 1) / self->levels[0].samples_per_entry;
            uint64_t idx_aligned = lvl1_idx * self->levels[0].samples_per_entry;
            if (idx != idx_aligned) {
                uint64_t sz0 = idx_aligned - idx;
                if (remaining < sz0) {
                    sz0 = remaining;
                }
                struct jsdrv_summary_entry_s entry_tmp;
                summary_level0_get(self, s_start, sz0, &entry_tmp);
                jsdrv_statistics_from_entry(&s_accum, &entry_tmp, sz0);
                remaining -= sz0;
                idx = idx_aligned % self->N;
            }

            while (level <= tgt_level) {
                uint64_t idx_lvl_dn = (idx + lvl_dn_step - 1) / lvl_dn_step;
                uint64_t idx_lvl_up = (idx + lvl_up_step - 1) / lvl_up_step;
                if ((idx_lvl_dn * lvl_dn_step) == (idx_lvl_up * lvl_up_step)) {
                    if (level == tgt_level) {
                        break;
                    }
                    ++level;
                    lvl_dn_step = self->levels[level - 1].samples_per_entry;
                    lvl_up_step = self->levels[level].samples_per_entry;
                } else if (remaining < lvl_dn_step) {
                    break;
                } else if (level) {
                    src = level_entry(self, level, idx_lvl_dn);
                    jsdrv_statistics_from_entry(&s_tmp, src, lvl_dn_step);
                    jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
                    JSDRV_ASSERT(remaining >= lvl_dn_step);
                    remaining -= lvl_dn_step;
                    idx = (idx + lvl_dn_step) % self->N;
                } else {
                    break;
                }
            }
        } else {
            ++valid_count;
        }

        uint64_t lvl_k = self->levels[level - 1].k;
        uint64_t lvl_step = self->levels[level - 1].samples_per_entry;
        uint64_t lvl_idx = idx / lvl_step;
        while (remaining >= lvl_step) {
            src = level_entry(self, level, lvl_idx);
            jsdrv_statistics_from_entry(&s_tmp, src, lvl_step);
            jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
            lvl_idx = (lvl_idx + 1) % lvl_k;
            JSDRV_ASSERT(remaining >= lvl_step);
            remaining -= lvl_step;
        }

        if ((entry_idx + 1) >= entries_length) {
            // final, get exact ending statistics
            while (remaining) {
                if (level == 0) {
                    struct jsdrv_summary_entry_s entry_tmp;
                    summary_level0_get(self, s_end - remaining, remaining, &entry_tmp);
                    jsdrv_statistics_from_entry(&s_tmp, &entry_tmp, remaining);
                    jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
                    remaining = 0;
                    break;
                }
                struct bufsig_level_s * lvl = &self->levels[level - 1];
                lvl_step = lvl->samples_per_entry;
                if (remaining < lvl_step) {
                    lvl_idx *= lvl->r;
                    --level;
                } else {
                    src = level_entry(self, level, lvl_idx);
                    jsdrv_statistics_from_entry(&s_tmp, src, lvl_step);
                    jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
                    JSDRV_ASSERT(remaining >= lvl_step);
                    remaining -= lvl_step;
                    lvl_idx = (lvl_idx + 1) % lvl->k;
                }
            }
            jsdrv_statistics_to_entry(&s_accum, dst);
        } else {
            // get approximate (averaged) statistics
            src = level_entry(self, level, lvl_idx);

            // complete this entry
            jsdrv_statistics_from_entry(&s_tmp, src, lvl_step);
            jsdrv_statistics_adjust_k(&s_tmp, remaining);
            jsdrv_statistics_combine(&s_accum, &s_accum, &s_tmp);
            jsdrv_statistics_to_entry(&s_accum, dst);

            // and start the next entry
            jsdrv_statistics_from_entry(&s_tmp, src, lvl_step);
            jsdrv_statistics_adjust_k(&s_tmp, lvl_step - remaining);
            jsdrv_statistics_copy(&s_accum, &s_tmp);
            JSDRV_ASSERT(incr >= s_tmp.k);
            remaining = incr - s_tmp.k;
        }
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
