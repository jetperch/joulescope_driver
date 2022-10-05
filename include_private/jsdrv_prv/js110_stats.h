/*
 * Copyright 2021-2022 Jetperch LLC
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
 * @brief JS220 statistics conversion.
 */

#include "jsdrv.h"
#include "jsdrv/cmacro_inc.h"
#include "js220_api.h"
#include <stdint.h>

#ifndef JSDRV_JS110_STATS_H__
#define JSDRV_JS110_STATS_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_js110_stats JS110 stats
 *
 * @brief Compute JS110 statistics on the host and convert on-instrument statistics.
 *
 * @{
 */


JSDRV_CPP_GUARD_START


struct js110_stats_s {
    struct jsdrv_statistics_s statistics;
    uint32_t sample_count;
    uint32_t valid_count;
};


void js110_stats_clear(struct js110_stats_s * self);

/**
 * @brief Set the number of samples per block.
 *
 * @param self The stats instance.
 * @param sample_count The number of samples in each stats block.
 */
void js110_stats_sample_count_set(struct js110_stats_s * self, uint32_t sample_count);

/**
 * @brief Validate and convert the JS220 statistics data structure.
 *
 * @param self[in] The stats instance.
 * @param i The current in A.
 * @param v The voltage in V.
 * @parma p The power in W.
 * @return NULL or the statistics structure which remains valid until the
 *      next call to this function with self.
 */
struct jsdrv_statistics_s * js110_stats_compute(struct js110_stats_s * self, float i, float v, float p);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_JS220_STATS_H__ */
