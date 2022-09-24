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

#include "jsdrv_util_prv.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <string.h>

static int usage(void) {
    printf("usage: jsdrv_util hotplug [--retain]\n");
    return 1;
}

static void on_add(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_STR) {
        printf("+ [unknown]\n");
    } else {
        printf("+ %s\n", value->value.str);
    }
}

static void on_remove(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_STR) {
        printf("- [unknown]\n");
    } else {
        printf("- %s\n", value->value.str);
    }
}

int on_hotplug(struct app_s * self, int argc, char * argv[]) {
    uint8_t add_flags = JSDRV_SFLAG_PUB;
    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if ((0 == strcmp(argv[0], "--retain")) || (0 == strcmp(argv[0], "-r"))) {
            add_flags |= JSDRV_SFLAG_RETAIN;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }
    printf("# Waiting for device add and device remove events.\n");
    printf("# Press CTRL-C to exit.\n");

    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, add_flags,
                        on_add, self, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB,
                        on_remove, self, JSDRV_TIMEOUT_MS_DEFAULT));

    while (1) {
        jsdrv_thread_sleep_ms(10);
    }

    return 0;
}
