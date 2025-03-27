/*
* Copyright 2023-2025 Jetperch LLC
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

#include "jsdrv/error_code.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv/tmap.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/mutex.h"
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>


#define ENTRIES_ALLOC_INIT  (1 << 7)  // rounded to upper power of 2


struct jsdrv_tmap_s {
    size_t alloc_size;  // always a power of 2
    size_t head;
    size_t tail;
    volatile size_t ref_count;
    volatile size_t reader_count;
    jsdrv_os_mutex_t mutex;

    bool time_map_update_pending;
    struct jsdrv_time_map_s time_map_update;

    bool tail_update_pending;
    size_t tail_update;

    struct jsdrv_time_map_s * entry;
};

static inline size_t ptr_incr(size_t ptr, size_t size) {
    return (ptr + 1) & (size - 1);
}

static inline size_t ptr_decr(size_t ptr, size_t size) {
    return (ptr - 1) & (size - 1);
}

static size_t tmap_size(const struct jsdrv_tmap_s * self) {
    if (self->head >= self->tail) {
        return self->head - self->tail;
    }
    return self->alloc_size - self->tail + self->head;
}

size_t jsdrv_tmap_size(struct jsdrv_tmap_s * self) {
    return tmap_size(self);
}

static size_t round_size_to_power_of_2(size_t sz) {
    if (sz <= 0) {
        return 1;
    }
    sz--;
    sz |= sz >> 1;
    sz |= sz >> 2;
    sz |= sz >> 4;
    sz |= sz >> 8;
    sz |= sz >> 16;
    if (sizeof(size_t) > 4) {
        sz |= sz >> 32;
    }
    sz++;
    return sz;
}

struct jsdrv_tmap_s * jsdrv_tmap_alloc(size_t initial_size) {
    struct jsdrv_tmap_s * self = calloc(1, sizeof(struct jsdrv_tmap_s));
    JSDRV_ASSERT_ALLOC(self);
    if (initial_size == 0) {
        initial_size = ENTRIES_ALLOC_INIT;
    }
    initial_size = round_size_to_power_of_2(initial_size);
    self->entry = calloc(1, initial_size * sizeof(struct jsdrv_time_map_s));
    if (self->entry == NULL) {
        JSDRV_ASSERT_ALLOC(self->entry);
    }
    self->alloc_size = initial_size;
    self->mutex = jsdrv_os_mutex_alloc("tmap");
    self->ref_count = 1;
    return self;
}

void jsdrv_tmap_ref_incr(struct jsdrv_tmap_s * self) {
    jsdrv_os_mutex_lock(self->mutex);
    ++self->ref_count;
    jsdrv_os_mutex_unlock(self->mutex);
}

static void tmap_free(struct jsdrv_tmap_s * self) {
    free(self->entry);
    self->entry = NULL;
    jsdrv_os_mutex_free(self->mutex);
    self->mutex = NULL;
    self->ref_count = 0;
    free(self);
}

void jsdrv_tmap_ref_decr(struct jsdrv_tmap_s * self) {
    if (self == NULL) {
        return;
    }
    if (0 == self->ref_count) {
        JSDRV_LOGW("tmap_ref_decr called with ref_count == 0");
        return;
    }
    jsdrv_os_mutex_lock(self->mutex);
    if (1 == self->ref_count) {
        jsdrv_os_mutex_unlock(self->mutex);
        tmap_free(self);
    } else {
        --self->ref_count;
        jsdrv_os_mutex_unlock(self->mutex);
    }
}

void jsdrv_tmap_clear(struct jsdrv_tmap_s * self) {
    if (NULL != self) {
        jsdrv_os_mutex_lock(self->mutex);
        self->head = 0;
        self->tail = 0;
        jsdrv_os_mutex_unlock(self->mutex);
    }
}

static void defer_add(struct jsdrv_tmap_s * self, const struct jsdrv_time_map_s * time_map) {
    size_t sz = tmap_size(self);
    if ((sz + 1) >= self->alloc_size) {
        size_t alloc_size = self->alloc_size * 2;
        self->entry = realloc(self->entry, alloc_size * sizeof(struct jsdrv_time_map_s));
        JSDRV_ASSERT_ALLOC(self->entry);
        if (self->head < self->tail) {
            // handle wrap
            for (size_t i = 0; i < self->head; ++i) {
                self->entry[i + self->alloc_size] = self->entry[i];
            }
            self->head += self->alloc_size;
        }
        self->alloc_size = alloc_size;
    } else if (sz) {
        size_t head_prev = ptr_decr(self->head, self->alloc_size);
        struct jsdrv_time_map_s * entry_prev = &self->entry[head_prev];
        if (time_map->offset_time < entry_prev->offset_time) {
            JSDRV_LOGE("UTC add is not monotonically increasing");
            return;
        }
    }

    self->entry[self->head] = *time_map;
    self->head = ptr_incr(self->head, self->alloc_size);
}

void jsdrv_tmap_add(struct jsdrv_tmap_s * self, const struct jsdrv_time_map_s * time_map) {
    size_t sz = tmap_size(self);
    if (time_map->counter_rate <= 0) {
        JSDRV_LOGW("Invalid counter rate: %" PRIu64, time_map->counter_rate);
        return;
    }

    if (sz) {
        size_t head_prev = ptr_decr(self->head, self->alloc_size);
        struct jsdrv_time_map_s * e = &self->entry[head_prev];
        if ((time_map->offset_time == e->offset_time)
                && (time_map->offset_counter == e->offset_counter)
                && (time_map->counter_rate == e->counter_rate)) {
            return;  // deduplicate
        }
    }

    jsdrv_os_mutex_lock(self->mutex);
    if (0 == self->reader_count) {
        defer_add(self, time_map);
    } else {
        self->time_map_update = *time_map;
        self->time_map_update_pending = true;
    }
    jsdrv_os_mutex_unlock(self->mutex);
}

void jsdrv_tmap_expire_by_sample_id(struct jsdrv_tmap_s * self, uint64_t sample_id) {
    if (self->head == self->tail) {
        return;  // nothing to expire
    }
    size_t tail = self->tail;
    while (1) {
        size_t tail_next = ptr_incr(tail, self->alloc_size);
        if (tail_next == self->head) {
            break;
        }
        if (sample_id < self->entry[tail].offset_counter) {
            break;
        }
        if ((sample_id > self->entry[tail].offset_counter) && (sample_id < self->entry[tail_next].offset_counter)) {
            break;
        }
        tail = tail_next;
    }

    if (tail != self->tail) {
        jsdrv_os_mutex_lock(self->mutex);
        if (0 == self->reader_count) {
            self->tail = tail;
        } else {
            self->tail_update = tail;
            self->tail_update_pending = true;
        }
        jsdrv_os_mutex_unlock(self->mutex);
    }
}

void jsdrv_tmap_reader_enter(struct jsdrv_tmap_s * self) {
    jsdrv_os_mutex_lock(self->mutex);
    self->reader_count++;
    jsdrv_os_mutex_unlock(self->mutex);
}

void jsdrv_tmap_reader_exit(struct jsdrv_tmap_s * self) {
    jsdrv_os_mutex_lock(self->mutex);
    self->reader_count--;
    if (self->reader_count == 0) {
        if (self->tail_update_pending) {
            self->tail_update_pending = false;
            self->tail = self->tail_update;
        }
        if (self->time_map_update_pending) {
            self->time_map_update_pending = false;
            defer_add(self, &self->time_map_update);
        }
    }
    jsdrv_os_mutex_unlock(self->mutex);
}

static struct jsdrv_time_map_s * find_entry_by_sample_id(struct jsdrv_tmap_s * self, uint64_t sample_id) {
    struct jsdrv_time_map_s * e_start = &self->entry[self->tail];
    struct jsdrv_time_map_s * e_end = &self->entry[ptr_decr(self->head, self->alloc_size)];
    if (sample_id <= e_start->offset_counter) {
        return e_start;
    } else if (sample_id >= e_end->offset_counter) {
        return e_end;
    }
    // initialize with even time map spacing
    double offset = ((double) (sample_id - e_start->offset_counter)) /
        (double) (e_end->offset_counter - e_start->offset_counter);
    size_t idx = (size_t) ((double) tmap_size(self) * offset);
    while (1) {
        struct jsdrv_time_map_s * entry = &self->entry[idx];
        if (sample_id < entry->offset_counter) {
            idx = ptr_decr(idx, self->alloc_size);
            continue;
        }
        size_t idx_next = ptr_incr(idx, self->alloc_size);
        if (sample_id >= self->entry[idx_next].offset_counter) {
            idx = idx_next;
            continue;
        }
        return entry;
    }
}

static struct jsdrv_time_map_s * find_entry_by_timestamp(struct jsdrv_tmap_s * self, int64_t timestamp) {
    struct jsdrv_time_map_s * e_start = &self->entry[self->tail];
    struct jsdrv_time_map_s * e_end = &self->entry[ptr_decr(self->head, self->alloc_size)];
    if (timestamp <= e_start->offset_time) {
        return e_start;
    } else if (timestamp >= e_end->offset_time) {
        return e_end;
    }
    // initialize with even time map spacing
    double offset = ((double) (timestamp - e_start->offset_time)) /
        (double) (e_end->offset_time - e_start->offset_time);
    size_t idx = (size_t) ((double) tmap_size(self) * offset);
    while (1) {
        struct jsdrv_time_map_s * entry = &self->entry[idx];
        if (timestamp < entry->offset_time) {
            idx = ptr_decr(idx, self->alloc_size);
            continue;
        }
        size_t idx_next = ptr_incr(idx, self->alloc_size);
        if (timestamp >= self->entry[idx_next].offset_time) {
            idx = idx_next;
            continue;
        }
        return entry;
    }
}

static int64_t entry_to_timestamp(struct jsdrv_time_map_s * entry, uint64_t sample_id) {
    double dsample = (double) (int64_t) (sample_id - entry->offset_counter);
    double dt = dsample / entry->counter_rate;
    dt *= JSDRV_TIME_SECOND;
    return entry->offset_time + (int64_t) dt;
}

static uint64_t entry_to_sample_id(struct jsdrv_time_map_s * entry, int64_t timestamp) {
    double dt = (double) (timestamp - entry->offset_time);;
    dt *= (1.0 / JSDRV_TIME_SECOND);
    return entry->offset_counter + (uint64_t) (dt * entry->counter_rate);
}

int32_t jsdrv_tmap_sample_id_to_timestamp(struct jsdrv_tmap_s * self, uint64_t sample_id, int64_t * timestamp) {
    if (tmap_size(self) == 0) {
        *timestamp = 0;
        return JSDRV_ERROR_UNAVAILABLE;
    }
    struct jsdrv_time_map_s * entry = find_entry_by_sample_id(self, sample_id);
    *timestamp = entry_to_timestamp(entry, sample_id);
    return 0;
}

int32_t jsdrv_tmap_timestamp_to_sample_id(struct jsdrv_tmap_s * self, int64_t timestamp, uint64_t * sample_id) {
    if (tmap_size(self) == 0) {
        *sample_id = 0;
        return JSDRV_ERROR_UNAVAILABLE;
    }
    struct jsdrv_time_map_s * entry = find_entry_by_timestamp(self, timestamp);
    *sample_id = entry_to_sample_id(entry, timestamp); // compute using tail
    return 0;
}

struct jsdrv_time_map_s * jsdrv_tmap_get(struct jsdrv_tmap_s * self, size_t index) {
    if (index >= tmap_size(self)) {
        return NULL;
    }
    size_t idx = (self->tail + index) & (self->alloc_size - 1);
    return &self->entry[idx];
}