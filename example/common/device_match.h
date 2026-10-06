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
 * @brief Match device paths against a user-provided device filter.
 *
 * Device paths have the form "{backend}/{model}/{serial_number}",
 * such as "u/js320/31NB".  No jsdrv dependency.
 */

#ifndef JSDRV_EXAMPLE_DEVICE_MATCH_H_
#define JSDRV_EXAMPLE_DEVICE_MATCH_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if a device path matches a device filter.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @param filter The case-insensitive device filter, which is one of:
 *      - NULL or "" to match every device.
 *      - the full device path, such as "u/js320/31NB".
 *      - the backend and model, such as "u/js320".
 *      - the model and serial number, such as "js320/31NB".
 *      - the model, such as "js320".
 *      - the serial number, such as "31NB".  Serial numbers must match
 *        exactly, so "8" does not match "u/js320/8W2A".
 *      - a prefix that ends in "/", such as "u/js320/".
 * @return true on a match, false otherwise.
 */
bool device_match(const char * device_path, const char * filter);

/**
 * @brief Check if a device path is a Joulescope instrument.
 *
 * @param device_path The device path, such as "u/js320/31NB".
 * @return true for JS110, JS220 and JS320 device paths, which have
 *      a model starting with "js".  false otherwise, such as for
 *      a MiniBitty at "u/mb/1".
 */
bool device_is_joulescope(const char * device_path);

#ifdef __cplusplus
}
#endif

#endif  /* JSDRV_EXAMPLE_DEVICE_MATCH_H_ */
