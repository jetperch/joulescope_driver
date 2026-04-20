/*
 * Copyright 2022 Jetperch LLC
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

#include "jsdrv_prv.h"
#include "jsdrv/topic.h"
#include "jsdrv/cstr.h"
#include "jsdrv/meta.h"
#include "jsdrv/error_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char USAGE[] =
    "\n"
    "usage: jsdrv_util set device_path \"topic=value\" ...\n"
    "    topic: The hierarchical topic relative to device_path separated by /.\n"
    "    value: The value to publish to topic.\n"
    "           String values must start and end with \".\n"
    "           Boolean values are true or false.\n"
    "           Otherwise 32-bit integer values.\n"
    "              0x prefix for hex.\n"
    "              - prefix for negative numbers.\n"
    "           binary values are hexstr(...).\n"
    "\n";


static int usage(void) {
    printf("%s", USAGE);
    return 1;
}

#define VALIDATE(topic__, rc__)  if (!(rc__)) {printf("%s invalid\n", (topic__)); return 1; }


static const char * const true_table_[] = {"ON", "TRUE", "YES", "ENABLE", "ENABLED", NULL};
static const char * const false_table[] = {"OFF", "FALSE", "NO", "DISABLE", "DISABLED", "NULL", "NONE", NULL};
int to_bool(char const * s, bool * value) {
    char buffer[16]; // longer than any entry
    int index;
    if ((s == NULL) || (NULL == value)) {
        return 1;
    }
    jsdrv_cstr_array_copy(buffer, s);
    jsdrv_cstr_toupper(buffer);
    if (0 == jsdrv_cstr_to_index(buffer, true_table_, &index)) {
        *value = true;
        return 0;
    }
    if (0 == jsdrv_cstr_to_index(buffer, false_table, &index)) {
        *value = false;
        return 0;
    }
    return 1;
}

static int is_separator(char c) {
    if ((c == ' ') || (c == ',') || (c == ')') || (c == 0) || ((c >= 9) && (c <= 13))) {
        return 1;
    }
    return 0;
}

static int u32list_to_bin(const char * s, uint8_t ** bin, size_t * bin_size) {
    if ((NULL == s) || (NULL == bin) || (NULL == bin_size)) {
        return 1;
    }
    size_t sz = 0;
    const char * c = s;
    bool mode_separator = true;
    while ((*c != ')') && (*c != 0)) {
        if (mode_separator) {
            if (!is_separator(*c)) {
                mode_separator = false;
                ++sz;
            }
        } else {
            if (is_separator(*c)) {
                mode_separator = true;
            }
        }
        ++c;
    }
    if (sz == 0) {
        return 1;
    }

    uint32_t * b = malloc(sz * 4);
    *bin = (uint8_t *) b;
    *bin_size = sz * 4;
    char u32_buffer[32];
    c = s;

    for (size_t i = 0; i < sz; ++i) {
        while (is_separator(*c)) {
            ++c;
        }
        char * z = u32_buffer;
        *z = 0;
        while (!is_separator(*c)) {
            *z++ = *c++;
            if ((z - u32_buffer) >= (intptr_t) (sizeof(u32_buffer) - 2)) {
                printf("u32list item %lu too long\n", i);
                return 1;
            }
        }
        *z++ = 0;
        if (jsdrv_cstr_to_u32(u32_buffer, &b[i])) {
            printf("u32list item %lu conversion failed: %s\n", i, u32_buffer);
            return 1;
        } else {
            printf("0x%08x\n", b[i]);
        }
    }

    return 0;
}

static int hexstr_to_bin(const char * s, uint8_t ** bin, size_t * bin_size) {
    if ((NULL == s) || (NULL == bin) || (NULL == bin_size)) {
        return 1;
    }

    size_t sz = 0;
    const char * c = s;
    while ((*c != ')') && (*c != 0)) {
        ++sz;
        ++c;
    }
    if (sz & 1) {
        printf("hexstr must have even number of hex characters\n");
        return 1;
    } else if (sz == 0) {
        return 1;
    }

    uint8_t * b = malloc(sz / 2);
    *bin = b;
    *bin_size = sz / 2;
    c = s;
    for (size_t i = 0; i < sz / 2; ++i) {
        *b++ = (jsdrv_cstr_hex_to_u4(c[0]) << 4) | jsdrv_cstr_hex_to_u4(c[1]);
        c += 2;
    }

    return 0;
}

static int set_arg(struct app_s * self, char * device_path, char * arg) {
    struct jsdrv_topic_s topic;
    char * value = arg;
    while (1) {
        if (!*value) {
            return 1;
        } else if (*value == '=') {
            *value++ = 0;
            break;
        }
        value++;
    }

    if (jsdrv_cstr_starts_with(arg, device_path)) {
        jsdrv_topic_set(&topic, arg);
    } else {
        jsdrv_topic_set(&topic, device_path);
        jsdrv_topic_append(&topic, arg);
    }
    printf("%s => %s\n", topic.topic, value);

    size_t slen = strlen(value);
    struct jsdrv_union_s v;
    bool v_bool;
    int32_t v_i32;
    uint32_t v_u32;

    if (0 == slen) {
        v = jsdrv_union_null();
    } else if (value[0] == '"') {
        value[slen - 1] = 0;
        v = jsdrv_union_str(value + 1);
    } else if (jsdrv_cstr_starts_with(value, "u32(")) {
        uint8_t * bin = NULL;
        size_t bin_size = 0;
        if (u32list_to_bin(value + 4, &bin, &bin_size)) {
            return 1;
        }
        v = jsdrv_union_bin(bin, bin_size);
    } else if (jsdrv_cstr_starts_with(value, "hexstr(")) {
        uint8_t * bin = NULL;
        size_t bin_size = 0;
        if (hexstr_to_bin(value + 7, &bin, &bin_size)) {
            return 1;
        }
        v = jsdrv_union_bin(bin, bin_size);
    } else if (!to_bool(value, &v_bool)) {
        v = jsdrv_union_u32(v_bool ? 1 : 0);
    } else if ((value[0] == '-') && !jsdrv_cstr_to_i32(value, &v_i32)) {
        v = jsdrv_union_i32(v_i32);
    } else if (!jsdrv_cstr_to_u32(value, &v_u32)) {
        v = jsdrv_union_u32(v_u32);
    } else {
        printf("Could not parse value: %s\n", value);
        return 1;
    }

    int32_t rc = jsdrv_publish(self->context, topic.topic, &v, 0);
    if (rc) {
        printf("publish failed with %d : %s\n", rc, jsdrv_error_code_description(rc));
    }
    return rc;
}

int on_set(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    while (argc) {
        if (argv[0][0] == '-') {
            // no options at this time
            return usage();
        } else if (NULL == device_filter) {
            device_filter = argv[0];
            if (strchr(device_filter, '=')) {
                return usage();
            }
            ROE(app_match(self, device_filter));
            ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, 1000));
            ARG_CONSUME();
        } else {
            if (set_arg(self, self->device.topic, argv[0])) {
                return usage();
            }
            ARG_CONSUME();
        }
    }

    if (self->device.topic[0]) {
        jsdrv_close(self->context, self->device.topic, 1000);
    }

    return 0;
}
