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
#include "jsdrv_prv/frontend.h"


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

    self->size_in_utc = ((double) N) / (self->hdr.sample_rate / self->hdr.decimate_factor);

    // todo allocate circular sample buffer
    // todo allocate circular summary levels
}

void jsdrv_bufsig_free(struct bufsig_s * self) {
    (void) self;
    // todo
}

void jsdrv_bufsig_recv_data(struct bufsig_s * self, struct jsdrvp_msg_s * msg) {
    struct jsdrv_stream_signal_s * s = (struct jsdrv_stream_signal_s *) msg->value.value.bin;
    self->hdr.sample_id = s->sample_id;
    self->hdr.field_id = s->field_id;
    self->hdr.index = s->index;
    self->hdr.element_type = s->element_type;
    self->hdr.element_size_bits = s->element_size_bits;
    self->hdr.element_count = s->element_count;
    self->hdr.sample_rate = s->sample_rate;
    self->hdr.decimate_factor = s->decimate_factor;

    // todo - add to buffer if initialized
}
