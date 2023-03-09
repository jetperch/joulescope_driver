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

#include "jsdrv/time.h"
#include "tinyprintf.h"
#include <inttypes.h>
#include <math.h>

int32_t jsdrv_time_to_str(int64_t t, char * str, size_t size) {
    if (!size) {
        return 0;
    }
    int64_t microseconds = JSDRV_TIME_TO_MICROSECONDS(t);
    int64_t seconds = microseconds / 1000000;
    uint32_t days = (uint32_t) (seconds / (60 * 60 * 24));
    days += 719468 + 17532;
    uint32_t era = days / 146097;
    uint32_t doe = days - era * 146097;
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;  // [0, 399]
    uint32_t y = yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);                // [0, 365]
    uint32_t mp = (5*doy + 2)/153;                                   // [0, 11]
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;                       // [1, 31]
    uint32_t m = mp + (mp < 10 ? 3 : -9);                            // [1, 12]
    y += (m <= 2) ? 1 : 0;

    uint32_t us = (uint32_t) (microseconds - (seconds * 1000000));
    uint32_t ss = (uint32_t) (seconds % (60 * 60 * 24));
    uint32_t hh = ss / (60 * 60);
    ss -= hh * (60 * 60);
    uint32_t mm = ss / 60;
    ss -= mm * 60;
    tfp_snprintf(str, size,
                 "%04" PRIu32 "-%02" PRIu32"-%02" PRIu32 "T"
                     "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 ".%06" PRIu32,
            y, m, d, hh, mm, ss, us);
    if (size >= JSDRV_TIME_STRING_LENGTH) {
        return (JSDRV_TIME_STRING_LENGTH - 1);
    } else {
        return ((int32_t) size - 1);
    }
}

int64_t jsdrv_time_from_counter(struct jsdrv_time_map_s * self, uint64_t counter) {
    int64_t delta = (int64_t) (counter - self->offset_counter);
    double scale =  (double) JSDRV_TIME_SECOND / self->counter_rate;
    int64_t time64 = (int64_t) round(scale * (double) delta);
    time64 += self->offset_time;
    return time64;
}

uint64_t jsdrv_time_to_counter(struct jsdrv_time_map_s * self, int64_t time64) {
    time64 -= self->offset_time;
    double scale = self->counter_rate / (double) JSDRV_TIME_SECOND;
    int64_t counter = (int64_t) round(scale * (double) time64);
    counter += self->offset_counter;
    return counter;
}
