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
 * @brief JS110 calibration format.
 */

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

#ifndef JSDRV_JS110_CAL_H__
#define JSDRV_JS110_CAL_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_js110_cal JS110 calibration
 *
 * @brief Parse the JS110 calibration.
 *
 * @{
 */


JSDRV_CPP_GUARD_START

struct js110_cal_header_s {
    uint8_t magic[16];
    uint64_t length;
    uint32_t version;
    uint32_t crc32;
};

/**
 * @brief Parse the JS110 calibration file format.
 *
 * @param data The JS110 calibration data record.
 * @param[out] cal The resulting cal[2][2][9]
 * @return
 */
int32_t js110_cal_parse(const uint8_t * data, double cal[2][2][9]);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_OS_EVENT_H__ */
