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
#include "device_match.h"
#include <stdio.h>
#include <string.h>
#include "jsdrv_prv/thread.h"


static int usage() {
    printf("usage: jsdrv reset [--device {device_path}]} {app|update1|update2}\n");
    return 1;
}

static void on_add(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    (void) value;
    volatile uint32_t * counter = (volatile uint32_t *) user_data;
    *counter += 1;
}


int on_reset(struct app_s * self, int argc, char * argv[]) {
    char * device = NULL;
    char * target = NULL;
    volatile uint32_t counter = 0;
    int32_t rc;

    while (argc) {
        if (argv[0][0] != '-') {
            if (target) {
                return usage();
            }
            target = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (!target) {
        printf("Missing reset target\n");
        return usage();
    }
    ROE(app_match(self, device));
    if (device_is_model(self->device.topic, "js320")) {
        // The JS320 has no h/!reset.  Use "minibitty firmware launch" instead.
        printf("Device %s does not support reset\n", self->device.topic);
        return 1;
    }

    struct jsdrv_topic_s topic;

    //printf("Open device %s\n", self->device.topic);
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_OPEN);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(JSDRV_DEVICE_OPEN_MODE_RAW), JSDRV_TIMEOUT_MS_DEFAULT));

    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB, on_add, (void *) &counter, JSDRV_TIMEOUT_MS_DEFAULT));

    //printf("Reset to %s\n", target);
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/!reset");
    rc = jsdrv_publish(self->context, topic.topic, &jsdrv_union_cstr(target), JSDRV_TIMEOUT_MS_DEFAULT);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_CLOSE);
    int32_t close_rc = jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT);
    if (!rc) {
        rc = close_rc;
    }

    //printf("Wait for reconnect\n");
    while (!rc && !counter && !quit_) {
        jsdrv_thread_sleep_ms(1);
    }
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD, on_add, (void *) &counter, JSDRV_TIMEOUT_MS_DEFAULT);
    return rc;
}
