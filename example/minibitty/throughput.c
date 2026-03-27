/*
 * Copyright 2025 Jetperch LLC
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
#include "jsdrv/cstr.h"
#include "jsdrv_prv/platform.h"
#include <stdio.h>
#include <string.h>

#include "jsdrv/os_thread.h"


static void on_device_stats(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    uint32_t * p_u32 = (uint32_t *) value->value.bin;
    printf("%s: %d\n", topic, p_u32[7]);
}

static int usage(void) {
    printf(
        "usage: minibitty throughput [options] device_path\n"
        "options:\n"
        "  --outstanding {n}  The number of in-flight messages. [1]\n"
        "  --prefix {c}       The PubSub prefix [c]\n"
        );
    return 1;
}

int on_throughput(struct app_s * self, int argc, char * argv[]) {
    uint64_t outstanding = 1;
    struct jsdrv_topic_s topic_base;
    struct jsdrv_topic_s topic;
    char *device_filter = NULL;
    char prefix[2] = { 'c', 0 };

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--outstanding")) || (0 == strcmp(argv[0], "-o"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u64(argv[0], &outstanding)) {
                printf("ERROR: invalid --outstanding value\n");
                return usage();
            } else if (outstanding > 128) {
                printf("ERROR: --outstanding value to big (<= 128)\n");
                return usage();
            }
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--prefix")) || (0 == strcmp(argv[0], "-p"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (argv[0][1] != 0) {
                printf("Prefix must be a single character.\n");
                return usage();
            }
            prefix[0] = argv[0][0];
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (NULL == device_filter) {
        printf("device_filter required\n");
        return usage();
    }

    ROE(app_match(self, device_filter));

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic_base, self->device.topic);
    jsdrv_topic_append(&topic_base, prefix);

    jsdrv_topic_set(&topic, topic_base.topic);
    jsdrv_topic_append(&topic, "comm/usbd/0/tx/!stat");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_device_stats, NULL, 0);

    jsdrv_topic_set(&topic, topic_base.topic);
    jsdrv_topic_append(&topic, "comm/usbd/0/rx/!stat");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_device_stats, NULL, 0);

    jsdrv_topic_set(&topic, topic_base.topic);
    jsdrv_topic_append(&topic, "comm/tpt/0/tx/cnt");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u8(outstanding), 0);

    jsdrv_topic_set(&topic, topic_base.topic);
    jsdrv_topic_append(&topic, "comm/tpt/0/tx/sz");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u16(512), 0);

    jsdrv_topic_set(&topic, topic_base.topic);
    jsdrv_topic_append(&topic, "comm/tpt/0/tx/task");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u8(1), 0);  // USBD task

    while (!quit_) {
        jsdrv_thread_sleep_ms(10);
    }

    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u8(0), 0);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);

    return 0;
}
