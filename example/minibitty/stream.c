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
#include "jsdrv/cstr.h"
#include "jsdrv_prv/platform.h"
#include <stdio.h>
#include <string.h>

#include <windows.h>  // todo remove


#define MB_TOPIC_LENGTH_MAX (32U)
#define LINK_PING_SIZE_U32 ((512U - 12U) >> 2)
#define PUBSUB_PING_SIZE_U32 ((512U - 12U - 8U - MB_TOPIC_LENGTH_MAX) >> 2)  // frame, stdmsg, publish


enum loopback_location_e {
    LOCATION_LINK = 0,
    LOCATION_PUBSUB = 1,
};

void on_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    static uint32_t counter = 0;
    (void) user_data;
    (void) topic;
    (void) value;
    // do something
    if (counter == 1000) {
        const uint32_t * p32 = &value->value.u32;
        printf("%d\n", p32[32]);
        counter = 0;
    } else {
        ++counter;
    }
}

#define PUBLISH_U32(topic_, value_) \
    jsdrv_topic_set(&topic, self->device.topic); \
    jsdrv_topic_append(&topic, topic_); \
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(value_), 0)


static int run(struct app_s * self, const char * device) {
    struct jsdrv_topic_s topic;

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(100);     // todo improved way to detect sensor ready

    PUBLISH_U32("c/led/red", 0);
    PUBLISH_U32("c/led/green", 0x0f);
    PUBLISH_U32("c/led/blue", 0xf0);
    PUBLISH_U32("c/led/en", 1);

    PUBLISH_U32("s/led/red", 0);
    PUBLISH_U32("s/led/green", 0x0f);
    PUBLISH_U32("s/led/blue", 0xf0);
    PUBLISH_U32("s/led/en", 1);

    //PUBLISH_U32("s/adc/0/sel", 0);
    //PUBLISH_U32("s/adc/0/ctrl", 1);

    PUBLISH_U32("s/adc/0/sel", 3);
    PUBLISH_U32("i/range/mode", 5);
    PUBLISH_U32("i/range/select", 0);
    PUBLISH_U32("v/range/mode", 1);
    PUBLISH_U32("v/range/select", 1);
    PUBLISH_U32("s/adc/0/ctrl", 1);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "s/adc/0/!data");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_data, NULL, 0);

    while (!quit_) {
    }

    PUBLISH_U32("s/adc/0/ctrl", 0);

    jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);
    return 0;
}

static int usage(void) {
    printf(
        "usage: minibitty stream device_path\n"
        );
    return 1;
}

int on_stream(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
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

    return run(self, self->device.topic);
}
