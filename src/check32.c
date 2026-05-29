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

#include "jsdrv_prv/check32.h"

uint32_t jsdrv_check32_xxhash(const uint32_t * data, uint32_t length) {
    uint32_t value = JSDRV_CHECK32_INITIAL_VALUE;
    for (uint32_t i = 0; i < length; ++i) {
        value += data[i] * 0x85ebca6bU;
        value = (value << 13) | (value >> 19);
        value *= 0xc2b2ae35U;
    }
    return value;
}
