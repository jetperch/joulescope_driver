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

#include "jsdrv_prv/time_map_filter.h"
#include "jsdrv_prv/platform.h"
#include <inttypes.h>


struct tmf_point_s {
    uint64_t counter;
    int64_t utc;
};


struct jsdrv_tmf_s {
    struct jsdrv_time_map_s time_map;
    int64_t interval;
    uint32_t points_max;
    uint32_t points_valid;
    uint32_t head;
    int64_t utc_prev;
    struct tmf_point_s points[];
};


struct jsdrv_tmf_s * jsdrv_tmf_new(uint32_t counter_rate, uint32_t points, int64_t interval) {
    if ((counter_rate == 0) || (points == 0) || (interval < JSDRV_TIME_MICROSECOND)) {
        return NULL;
    }

    size_t sz = sizeof(struct jsdrv_tmf_s) + sizeof(struct tmf_point_s) * points;
    struct jsdrv_tmf_s * self = jsdrv_alloc(sz);
    if (NULL == self) {
        return NULL;
    }
    memset(self, 0, sz);
    self->time_map.counter_rate = counter_rate;
    self->interval = interval;
    self->points_max = points;
    return self;
}

void jsdrv_tmf_free(struct jsdrv_tmf_s * self) {
    if (NULL != self) {
        jsdrv_free(self);
    }
}

void jsdrv_tmf_clear(struct jsdrv_tmf_s * self) {
    if (NULL != self) {
        self->head = 0;
        self->points_valid = 0;
        self->time_map.offset_time = 0;
        self->time_map.offset_counter = 0;
        self->utc_prev = 0;
    }
}

void jsdrv_tmf_add(struct jsdrv_tmf_s * self, uint64_t counter, int64_t utc) {
    if (NULL == self) {
        return;
    }
    if ((utc - self->utc_prev) < self->interval) {
        return;
    }

    // add new point
    self->points[self->head].counter = counter;
    self->points[self->head].utc = utc;
    self->utc_prev = utc;
    ++self->head;
    if (self->head >= self->points_max) {
        self->head = 0;
    }
    if (self->points_valid < self->points_max) {
        ++self->points_valid;
    }

    // update time map
    uint32_t tail = self->points_max + self->head - self->points_valid;
    if (tail >= self->points_max) {
        tail -= self->points_max;
    }
    uint64_t counter_offset = self->points[tail].counter;
    int64_t utc_est = self->points[tail].utc;
    while (tail != self->head) {
        uint64_t counter_delta = self->points[tail].counter - counter_offset;
        int64_t utc_delta = JSDRV_COUNTER_TO_TIME(counter_delta, (uint64_t) self->time_map.counter_rate);
        int64_t utc_est1 = self->points[tail].utc - utc_delta;
        if (utc_est1 < utc_est) {
            utc_est = utc_est1;
        }
        ++tail;
        if (tail >= self->points_max) {
            tail = 0;
        }
    }
    self->time_map.offset_counter = counter_offset;
    self->time_map.offset_time = utc_est;
}

void jsdrv_tmf_get(struct jsdrv_tmf_s * self, struct jsdrv_time_map_s * time_map) {
    if (NULL == time_map) {
        return;
    }
    if (NULL == self) {
        time_map->offset_counter = 0;
        time_map->offset_time = 0;
        time_map->counter_rate = 0;
        return;
    }
    *time_map = self->time_map;
}
