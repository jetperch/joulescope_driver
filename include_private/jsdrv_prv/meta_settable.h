/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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
 * @brief Select the device topics that a "defaults" open restores.
 */

#ifndef JSDRV_PRV_META_SETTABLE_H_
#define JSDRV_PRV_META_SETTABLE_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Check whether a "defaults" open may set a topic.
 *
 * @param topic The device topic, such as "s/i/range/min".
 * @param meta The topic's JSON metadata.
 * @return True when the topic is writable (not "ro"), retained (no
 *      segment starts with '!'), and has a scalar dtype.  Get the
 *      value with jsdrv_meta_default().
 *
 * The topic check is required: device firmware defines defaults for
 * some non-retained topics, such as "s/stats/!clear".
 */
bool jsdrv_meta_is_settable(const char * topic, const char * meta);

/**
 * @brief Check for a scalar value type.
 *
 * @param type The JSDRV_UNION_* type.
 * @return True for integer, float, and bool types.  False for null,
 *      string, JSON, and binary types.
 */
bool jsdrv_union_type_is_scalar(uint8_t type);

#endif  /* JSDRV_PRV_META_SETTABLE_H_ */
