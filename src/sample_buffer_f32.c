/*
 * Copyright 2022 Jetperch LLC
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

#include "jsdrv_prv/sample_buffer_f32.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/cdef.h"
#include <math.h>


JSDRV_STATIC_ASSERT(offsetof(struct sbuf_f32_s, msg_sample_id) + 4 == offsetof(struct sbuf_f32_s, buffer), sample_id_location);


void sbuf_f32_clear(struct sbuf_f32_s * self) {
    jsdrv_memset(self, 0, offsetof(struct sbuf_f32_s, buffer));
    self->sample_id_decimate = 2;
}

uint32_t sbuf_f32_length(struct sbuf_f32_s * self) {
    return (self->head - self->tail) & SAMPLE_BUFFER_MASK;
}

void sbuf_f32_add(struct sbuf_f32_s * self, uint64_t sample_id, float * data, uint32_t length) {
    if (self->head_sample_id > sample_id) {
        uint64_t dup = (self->head_sample_id - sample_id) / self->sample_id_decimate;
        if (dup > length) {
            return;
        }
        data += dup;
        length -= (uint32_t) dup;
        sample_id += dup * self->sample_id_decimate;
    } else if (self->head_sample_id < sample_id) {
        uint64_t skips = (sample_id - self->head_sample_id) / self->sample_id_decimate;
        if (skips >= SAMPLE_BUFFER_LENGTH) {
            skips = SAMPLE_BUFFER_LENGTH - 1;
            self->head_sample_id = sample_id - skips * self->sample_id_decimate;
        }
        while (self->head_sample_id < sample_id) {
            self->buffer[self->head] = NAN;
            self->head = (self->head + 1) & SAMPLE_BUFFER_MASK;
            if (self->tail == self->head) {
                self->tail = (self->tail + 1) & SAMPLE_BUFFER_MASK;
            }
            self->head_sample_id += self->sample_id_decimate;
        }
    }
    if (length >= SAMPLE_BUFFER_LENGTH) {
        uint32_t skip = length - (SAMPLE_BUFFER_LENGTH - 1);
        data += skip;
        length = SAMPLE_BUFFER_LENGTH - 1;
        self->head_sample_id += skip * self->sample_id_decimate;
    }
    uint32_t head_inc = self->head + length;
    if (head_inc >= SAMPLE_BUFFER_LENGTH) {
        self->head_sample_id += length * self->sample_id_decimate;
        uint32_t sz1 = SAMPLE_BUFFER_LENGTH - self->head;
        jsdrv_memcpy(self->buffer + self->head, data, sizeof(float) * (SAMPLE_BUFFER_LENGTH - self->head));
        data += sz1;
        length -= sz1;
        jsdrv_memcpy(self->buffer, data, sizeof(float) * length);
        self->head = length;
        self->tail = (self->head + 1) & SAMPLE_BUFFER_MASK;
    } else {
        self->head_sample_id += length * self->sample_id_decimate;
        jsdrv_memcpy(self->buffer + self->head, data, sizeof(float) * length);
        if ((self->tail > self->head) && (self->tail <= head_inc)) {
            self->tail = (head_inc + 1) & SAMPLE_BUFFER_MASK;
        }
    }
    self->head = head_inc & SAMPLE_BUFFER_MASK;
}

void sbuf_f32_mult(struct sbuf_f32_s * r, struct sbuf_f32_s * s1, struct sbuf_f32_s * s2) {
    uint64_t s1_sample_id = s1->head_sample_id - sbuf_f32_length(s1) * s1->sample_id_decimate;
    uint64_t s2_sample_id = s2->head_sample_id - sbuf_f32_length(s2) * s2->sample_id_decimate;
    sbuf_f32_clear(r);

    if (s1_sample_id > s2_sample_id) {
        struct sbuf_f32_s * sk = s1;
        uint64_t sk_sample_id = s1_sample_id;
        s1 = s2;
        s2 = sk;
        s1_sample_id = s2_sample_id;
        s2_sample_id = sk_sample_id;
    }

    while ((s1->tail != s1->head) && (s1_sample_id < s2_sample_id)) {
        s1_sample_id += s1->sample_id_decimate;
        s1->tail = (s1->tail + 1) & SAMPLE_BUFFER_MASK;
    }

    r->sample_id_decimate = s1->sample_id_decimate;
    r->head_sample_id = s1_sample_id;
    r->msg_sample_id = (uint32_t) (s1_sample_id & 0xffffffff);
    while ((s1->tail != s1->head) && (s2->tail != s2->head)) {
        r->buffer[r->head++] = s1->buffer[s1->tail] * s2->buffer[s2->tail];
        s1->tail = (s1->tail + 1) & SAMPLE_BUFFER_MASK;
        s2->tail = (s2->tail + 1) & SAMPLE_BUFFER_MASK;
        r->head_sample_id += r->sample_id_decimate;
    }
}
