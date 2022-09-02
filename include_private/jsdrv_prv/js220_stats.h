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

#include "jsdrv/cmacro_inc.h"
#include "js220_api.h"
#include <stdint.h>

#ifndef JSDRV_JS220_STATS_H__
#define JSDRV_JS220_STATS_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_js220_stats JS220 stats
 *
 * @brief Validate and convert the JS220 statistics message.
 *
 * @{
 */


JSDRV_CPP_GUARD_START
// forward declaration from js220_api.h
struct js220_statistics_raw_s;
// forward declaration from jsdrv.h
struct jsdrv_statistics_s;

double js220_stats_i128_to_f64(js220_i128 x, uint32_t q);

/**
 * @brief Validate and convert the JS220 statistics data structure.
 *
 * @param src[in] The JS220 statistics data structure
 * @param dst[out] The JSDRV statistics data structure.
 * @return 0 or error code.
 */
int32_t js220_stats_convert(struct js220_statistics_raw_s const * src, struct jsdrv_statistics_s * dst);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_JS220_STATS_H__ */
