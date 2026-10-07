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
 * @brief Match device paths against user-provided device specifications.
 *
 * Device paths have the form "{backend}/{model}/{serial_number}",
 * such as "u/js320/31NB".  The matching behavior is the same as
 * pyjoulescope_driver.device_filter.find.  No jsdrv dependency.
 */

#ifndef JSDRV_EXAMPLE_DEVICE_MATCH_H_
#define JSDRV_EXAMPLE_DEVICE_MATCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if a device path matches the device specifications.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @param filter The comma-separated list of case-insensitive device
 *      specifications.  NULL, "" or only empty specifications match
 *      every device.  Whitespace and leading or trailing "/" around each
 *      specification are ignored.  Each specification is one of:
 *      - the full device path, such as "u/js320/31NB".
 *      - the model and serial number, such as "js320/31NB".
 *      - the model and serial number, such as "js320-31NB".
 *      - the model, such as "js320".
 *      - the serial number, such as "31NB".  Serial numbers must match
 *        exactly, so "8" does not match "u/js320/8W2A".
 * @return true if the device path matches any specification,
 *      false otherwise.
 */
bool device_match(const char * device_path, const char * filter);

/**
 * @brief Validate a brand name.
 *
 * @param brand The case-insensitive brand name or alias, such as
 *      "joulescope" or "js".
 * @return The brand name, such as "Joulescope", or NULL if not supported.
 */
const char * device_brand_validate(const char * brand);

/**
 * @brief Get the brand for a device path.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @return The brand name, such as "Joulescope", or NULL if unknown.
 */
const char * device_brand(const char * device_path);

/**
 * @brief Check if a device path has a brand.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @param brand The case-insensitive brand name or alias, such as
 *      "Joulescope" or "js".  NULL matches all brands.
 * @return true if the device path has the brand, false otherwise,
 *      including for an unsupported brand.
 */
bool device_is_brand(const char * device_path, const char * brand);

/**
 * @brief Check if a device path is a Joulescope instrument.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @return true for JS110, JS220 and JS320 device paths.  false otherwise,
 *      such as for a MiniBitty at "u/mb/1".
 */
bool device_is_joulescope(const char * device_path);

/**
 * @brief Check if a device path has a model.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @param model The case-insensitive model, such as "js320".
 * @return true when the device path's model equals model.
 */
bool device_is_model(const char * device_path, const char * model);

/**
 * @brief Find the devices in a list that match device specifications.
 *
 * @param devices The comma-separated device path list, such as
 *      "u/js220/000415,u/js320/31NB".  NULL is an empty list.
 * @param filter The device specifications.  See device_match().
 * @param brand The brand.  See device_is_brand().
 * @param[out] match The buffer for the first matching device path.
 *      The buffer is set to "" when no device matches.  NULL to skip.
 * @param match_size The size of match in bytes, including the terminator.
 * @return The number of matching devices.
 */
uint32_t device_match_list(const char * devices, const char * filter, const char * brand,
                           char * match, size_t match_size);

#ifdef __cplusplus
}
#endif

#endif  /* JSDRV_EXAMPLE_DEVICE_MATCH_H_ */
