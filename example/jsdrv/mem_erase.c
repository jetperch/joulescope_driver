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
#include "jsdrv/cstr.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>


#define ERASE_TIMEOUT_MS (30000)


static int usage() {
    printf("usage: jsdrv_util mem_erase [--device {device_path}] [--timeout {timeout_ms}] {region}\n");
    return 1;
}

int on_mem_erase(struct app_s * self, int argc, char * argv[]) {
    char * device = NULL;
    char * region = NULL;
    uint32_t erase_timeout_ms = ERASE_TIMEOUT_MS;

    while (argc) {
        if (argv[0][0] != '-') {
            if (NULL == region) {
                region = argv[0];
                ARG_CONSUME();
            } else {
                return usage();
            }
        } else if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--timeout")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u32(argv[0], &erase_timeout_ms)) {
                printf("Could not parse timeout\n");
                return usage();
            }
            device = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (!js220_is_mem_region_valid(region)) {
        return usage();
    }

    ROE(app_match(self, device));

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_OPEN);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(JSDRV_DEVICE_OPEN_MODE_RESUME), JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/mem");
    jsdrv_topic_append(&topic, region);
    jsdrv_topic_append(&topic, "!erase");
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), erase_timeout_ms));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_CLOSE);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
