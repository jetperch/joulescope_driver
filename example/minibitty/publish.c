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

#include "minibitty_exe_prv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    printf(
        "usage: minibitty publish [--type <t>] <device> <subtopic> <value>\n"
        "\n"
        "Open the device, publish one value, and close.\n"
        "  --type <t>   Value type: u8, u16, u32 [u32]\n"
        "  device       The device filter, e.g. u/js320/8W2A\n"
        "  subtopic     The device-relative topic, e.g. c/comm/usbd/wd_en\n"
        "  value        The unsigned integer value\n"
        );
    return 1;
}

int on_publish_cmd(struct app_s * self, int argc, char * argv[]) {
    const char * type_str = "u32";
    char * pos_args[3];
    int pos_count = 0;

    while (argc) {
        if (argv[0][0] != '-') {
            if (pos_count >= 3) {
                return usage();
            }
            pos_args[pos_count++] = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--type")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            type_str = argv[0];
            ARG_CONSUME();
        } else {
            return usage();
        }
    }
    if (pos_count != 3) {
        return usage();
    }

    uint32_t value = (uint32_t) strtoul(pos_args[2], NULL, 0);
    struct jsdrv_union_s v;
    if (0 == strcmp(type_str, "u8")) {
        v = jsdrv_union_u8((uint8_t) value);
    } else if (0 == strcmp(type_str, "u16")) {
        v = jsdrv_union_u16((uint16_t) value);
    } else if (0 == strcmp(type_str, "u32")) {
        v = jsdrv_union_u32(value);
    } else {
        return usage();
    }

    ROE(app_match(self, pos_args[0]));
    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME,
                   JSDRV_TIMEOUT_MS_DEFAULT));

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, pos_args[1]);
    int32_t rc = jsdrv_publish(self->context, topic.topic, &v, JSDRV_TIMEOUT_MS_DEFAULT);
    printf("publish %s = %lu : %s\n", topic.topic, (unsigned long) value,
           rc ? "FAILED" : "ok");
    if (rc) {
        printf("error %ld: %s\n", (long) rc, jsdrv_error_code_name(rc));
    }

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    return (int) rc;
}
