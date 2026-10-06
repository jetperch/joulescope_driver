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

#include "device_match.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>


// Compare the first n characters of a with the string b, ignoring case.
static bool eq_n(const char * a, size_t n, const char * b) {
    for (size_t i = 0; i < n; ++i) {
        if (!b[i] || (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i]))) {
            return false;
        }
    }
    return 0 == b[n];
}

static bool prefix_match(const char * s, const char * prefix) {
    while (*prefix) {
        if (tolower((unsigned char) *s++) != tolower((unsigned char) *prefix++)) {
            return false;
        }
    }
    return true;
}

// Split "backend/model/serial" into its separator positions.
static bool split(const char * device_path, const char ** model, const char ** serial) {
    const char * p1 = strchr(device_path, '/');
    if (!p1) {
        return false;
    }
    const char * p2 = strchr(p1 + 1, '/');
    if (!p2 || strchr(p2 + 1, '/')) {
        return false;
    }
    *model = p1 + 1;
    *serial = p2 + 1;
    return true;
}

bool device_match(const char * device_path, const char * filter) {
    const char * model;
    const char * serial;
    if (!device_path) {
        return false;
    }
    if (!filter || !filter[0]) {
        return true;
    }
    size_t filter_sz = strlen(filter);
    if (filter[filter_sz - 1] == '/') {
        return prefix_match(device_path, filter);
    }
    size_t path_sz = strlen(device_path);
    if (eq_n(device_path, path_sz, filter)) {
        return true;
    }
    if (!split(device_path, &model, &serial)) {
        return false;
    }
    size_t model_sz = (size_t) (serial - model - 1);
    return eq_n(device_path, (size_t) (serial - device_path - 1), filter)   // backend/model
        || eq_n(model, strlen(model), filter)                              // model/serial
        || eq_n(model, model_sz, filter)                                   // model
        || eq_n(serial, strlen(serial), filter);                           // serial
}

bool device_is_joulescope(const char * device_path) {
    const char * model;
    const char * serial;
    if (!device_path || !split(device_path, &model, &serial)) {
        return false;
    }
    return (tolower((unsigned char) model[0]) == 'j') && (tolower((unsigned char) model[1]) == 's');
}

bool device_is_model(const char * device_path, const char * model) {
    const char * p_model;
    const char * serial;
    if (!device_path || !model || !split(device_path, &p_model, &serial)) {
        return false;
    }
    return eq_n(p_model, (size_t) (serial - p_model - 1), model);
}

uint32_t device_match_list(const char * devices, const char * filter,
                           char * match, size_t match_size) {
    char path[256];
    uint32_t count = 0;
    if (match && match_size) {
        match[0] = 0;
    }
    if (!devices) {
        return 0;
    }
    const char * d = devices;
    while (*d) {
        const char * end = strchr(d, ',');
        size_t sz = end ? (size_t) (end - d) : strlen(d);
        if (sz && (sz < sizeof(path))) {
            memcpy(path, d, sz);
            path[sz] = 0;
            if (device_match(path, filter)) {
                if (!count && match && match_size) {
                    snprintf(match, match_size, "%s", path);
                }
                ++count;
            }
        }
        if (!end) {
            break;
        }
        d = end + 1;
    }
    return count;
}
