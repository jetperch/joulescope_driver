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
 * @brief Time map filter.
 */

#ifndef JSDRV_TIME_MAP_FILTER_H__
#define JSDRV_TIME_MAP_FILTER_H__

#include "jsdrv/time.h"

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_tmf Time map filter.
 *
 * @brief Filter count+UTC updates to produce the "best" time map estimate.
 *
 * @{
 */

JSDRV_CPP_GUARD_START


// forward declaration
struct jsdrv_tmf_s;

/**
 * Create a new time map filter instance.
 * @param counter_rate The counter rate in ticks per second (Hz).
 * @param points The number of points to save for the filtering.
 * @param interval The minimum interval between saved points.
 *
 * @return The new time map instance or NULL on error.
 */
struct jsdrv_tmf_s * jsdrv_tmf_new(uint32_t counter_rate, uint32_t points, int64_t interval);

/**
 * @brief Free a time map instance and release all resources.
 *
 * @param self The instance to free.
 */
void jsdrv_tmf_free(struct jsdrv_tmf_s * self);

/**
 * @brief Clear the time map filter.
 *
 * @param self The instance to clear.
 */
void jsdrv_tmf_clear(struct jsdrv_tmf_s * self);

/**
 * @brief Add a new entry.
 *
 * @param self The instance.
 * @param counter The counter value.
 * @param utc The UTC time value (see jsdrv/time.h).
 *
 * Note: the entry will only be added if the interval provided to "new"
 * has elapsed.  Otherwise, this entry will be ignored.
 */
void jsdrv_tmf_add(struct jsdrv_tmf_s * self, uint64_t counter, int64_t utc);

/**
 * @brief Get the best time map estimate.
 *
 * @param self The instance
 * @param time_map The output time_map.
 */
void jsdrv_tmf_get(struct jsdrv_tmf_s * self, struct jsdrv_time_map_s * time_map);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_TIME_MAP_FILTER_H__ */
