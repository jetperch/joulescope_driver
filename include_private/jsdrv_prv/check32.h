/*
 * Copyright 2026 Jetperch LLC
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
 * @brief Host-side mirror of minibitty's mb_check32_xxhash.
 *
 * This is the host-side counterpart to mb/check32.h.  joulescope_driver
 * does not link against minibitty, so the algorithm is reimplemented
 * here.  Output is bit-for-bit identical to mb_check32_xxhash so
 * records hashed by the firmware validate on the host and vice-versa.
 */

#ifndef JSDRV_PRV_CHECK32_H__
#define JSDRV_PRV_CHECK32_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

JSDRV_CPP_GUARD_START

/// Fixed initial value: floor((phi - 1) * 2^32) where phi is the golden ratio.
#define JSDRV_CHECK32_INITIAL_VALUE (0x9e3779b1U)

/**
 * @brief 32-bit xxHash-inspired integrity check.
 *
 * Bit-for-bit equivalent to mb_check32_xxhash() in
 * ../minibitty/include/mb/check32.h.  This is a corruption check,
 * not a cryptographic hash.
 *
 * @param data Data buffer (32-bit-aligned).
 * @param length Length in 32-bit words.
 * @return The check value.
 */
uint32_t jsdrv_check32_xxhash(const uint32_t * data, uint32_t length);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_CHECK32_H__ */
