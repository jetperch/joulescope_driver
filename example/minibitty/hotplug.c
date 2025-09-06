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

#include "minibitty_exe_prv.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <string.h>

static void on_list(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_STR) {
        printf("! [unknown]\n");
    } else {
        printf("Existing devices: %s\n", value->value.str);
    }
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
    (void) argc;
    (void) argv;

    jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_LIST, JSDRV_SFLAG_RETAIN | JSDRV_SFLAG_PUB,
                        on_list, self, 0);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_LIST, on_list, self, 0);

    printf("# Waiting for device add and device remove events.\n");
    printf("# Press CTRL-C to exit.\n");

    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB,
                        on_add, self, 0));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB,
                        on_remove, self, 0));

    while (!quit_) {
        jsdrv_thread_sleep_ms(10);
    }

    return 0;
}
