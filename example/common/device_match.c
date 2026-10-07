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


// Keep the brands in sync with pyjoulescope_driver/device_path.py.
struct brand_s {
    const char * name;
    const char * const * models;
};

struct brand_alias_s {
    const char * alias;
    const char * brand;
};

static const char * const JOULESCOPE_MODELS[] = {"js110", "js220", "js320", NULL};

static const struct brand_s BRANDS_TO_MODELS[] = {
    {"Joulescope", JOULESCOPE_MODELS},
    {NULL, NULL},
};

static const struct brand_alias_s BRAND_ALIASES[] = {
    {"js", "Joulescope"},
    {NULL, NULL},
};

// Compare a with length a_sz to b with length b_sz, ignoring case.
static bool eq_nn(const char * a, size_t a_sz, const char * b, size_t b_sz) {
    if (a_sz != b_sz) {
        return false;
    }
    for (size_t i = 0; i < a_sz; ++i) {
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) {
            return false;
        }
    }
    return true;
}

// Compare the first n characters of a with the string b, ignoring case.
static bool eq_n(const char * a, size_t n, const char * b) {
    return eq_nn(a, n, b, strlen(b));
}

// Split "backend/model/serial" into its parts.
static bool split(const char * device_path, const char ** model, size_t * model_sz,
                  const char ** serial) {
    const char * p1 = strchr(device_path, '/');
    if (!p1) {
        return false;
    }
    const char * p2 = strchr(p1 + 1, '/');
    if (!p2 || strchr(p2 + 1, '/')) {
        return false;
    }
    *model = p1 + 1;
    *model_sz = (size_t) (p2 - p1 - 1);
    *serial = p2 + 1;
    return true;
}

// Match one specification of length spec_sz, already trimmed.
static bool spec_match(const char * device_path, const char * spec, size_t spec_sz) {
    const char * model;
    size_t model_sz;
    const char * serial;
    if (!split(device_path, &model, &model_sz, &serial)) {
        return false;
    }
    size_t serial_sz = strlen(serial);
    size_t backend_model_sz = (size_t) (model - device_path) + model_sz;
    if (eq_n(spec, spec_sz, device_path)                               // backend/model/serial
            || eq_nn(spec, spec_sz, device_path, backend_model_sz)     // backend/model
            || eq_nn(spec, spec_sz, model, model_sz + 1 + serial_sz)   // model/serial
            || eq_nn(spec, spec_sz, model, model_sz)                   // model
            || eq_nn(spec, spec_sz, serial, serial_sz)) {              // serial
        return true;
    }
    // model-serial
    return (spec_sz == model_sz + 1 + serial_sz)
        && eq_nn(spec, model_sz, model, model_sz)
        && (spec[model_sz] == '-')
        && eq_nn(spec + model_sz + 1, serial_sz, serial, serial_sz);
}

bool device_match(const char * device_path, const char * filter) {
    if (!device_path) {
        return false;
    }
    bool has_spec = false;
    const char * s = filter ? filter : "";
    while (1) {
        const char * end = strchr(s, ',');
        const char * spec_end = end ? end : (s + strlen(s));
        // Trim whitespace, then "/", like Python spec.strip().strip('/')
        while ((s < spec_end) && isspace((unsigned char) *s)) {
            ++s;
        }
        while ((spec_end > s) && isspace((unsigned char) spec_end[-1])) {
            --spec_end;
        }
        while ((s < spec_end) && (*s == '/')) {
            ++s;
        }
        while ((spec_end > s) && (spec_end[-1] == '/')) {
            --spec_end;
        }
        if (spec_end > s) {
            has_spec = true;
            if (spec_match(device_path, s, (size_t) (spec_end - s))) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        s = end + 1;
    }
    return !has_spec;
}

const char * device_brand_validate(const char * brand) {
    if (!brand) {
        return NULL;
    }
    for (const struct brand_alias_s * a = BRAND_ALIASES; a->alias; ++a) {
        if (eq_n(brand, strlen(brand), a->alias)) {
            brand = a->brand;
            break;
        }
    }
    for (const struct brand_s * b = BRANDS_TO_MODELS; b->name; ++b) {
        if (eq_n(brand, strlen(brand), b->name)) {
            return b->name;
        }
    }
    return NULL;
}

const char * device_brand(const char * device_path) {
    const char * model;
    size_t model_sz;
    const char * serial;
    if (!device_path || !split(device_path, &model, &model_sz, &serial)) {
        return NULL;
    }
    for (const struct brand_s * b = BRANDS_TO_MODELS; b->name; ++b) {
        for (const char * const * m = b->models; *m; ++m) {
            if (eq_n(model, model_sz, *m)) {
                return b->name;
            }
        }
    }
    return NULL;
}

bool device_is_brand(const char * device_path, const char * brand) {
    if (!brand) {
        return true;
    }
    const char * name = device_brand_validate(brand);
    return name && (device_brand(device_path) == name);
}

bool device_is_joulescope(const char * device_path) {
    return device_is_brand(device_path, "Joulescope");
}

bool device_is_model(const char * device_path, const char * model) {
    const char * p_model;
    size_t p_model_sz;
    const char * serial;
    if (!device_path || !model || !split(device_path, &p_model, &p_model_sz, &serial)) {
        return false;
    }
    return eq_n(p_model, p_model_sz, model);
}

uint32_t device_match_list(const char * devices, const char * filter, const char * brand,
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
            if (device_is_brand(path, brand) && device_match(path, filter)) {
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
