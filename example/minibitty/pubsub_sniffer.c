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
#include "jsdrv_prv/thread.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int sniff_usage(void) {
    printf("usage: minibitty sniff <device_filter> <topic>\n"
           "\n"
           "Subscribe to a device topic and print received values.\n"
           "  device_filter  Device filter (e.g. \"mb\")\n"
           "  topic          Full topic path (e.g. \"c/jtag_emulator/!log\")\n"
           "\nPress CTRL-C to exit.\n");
    return 1;
}

static void on_publish(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    switch (value->type) {
        case JSDRV_UNION_STR:
            printf("[%s] %s\n", topic, value->value.str);
            break;
        case JSDRV_UNION_BIN:
            printf("[%s] BIN(%u):", topic, (unsigned) value->size);
            for (uint32_t i = 0; i < value->size && i < 64; ++i) {
                printf(" %02X", value->value.bin[i]);
            }
            if (value->size > 64) {
                printf(" ...");
            }
            printf("\n");
            break;
        case JSDRV_UNION_U8:
            printf("[%s] u8=%u\n", topic, (unsigned) value->value.u8);
            break;
        case JSDRV_UNION_U16:
            printf("[%s] u16=%u\n", topic, (unsigned) value->value.u16);
            break;
        case JSDRV_UNION_U32:
            printf("[%s] u32=%" PRIu32 "\n", topic, value->value.u32);
            break;
        case JSDRV_UNION_U64:
            printf("[%s] u64=%" PRIu64 "\n", topic, value->value.u64);
            break;
        case JSDRV_UNION_I8:
            printf("[%s] i8=%d\n", topic, (int) value->value.i8);
            break;
        case JSDRV_UNION_I16:
            printf("[%s] i16=%d\n", topic, (int) value->value.i16);
            break;
        case JSDRV_UNION_I32:
            printf("[%s] i32=%" PRId32 "\n", topic, value->value.i32);
            break;
        case JSDRV_UNION_I64:
            printf("[%s] i64=%" PRId64 "\n", topic, value->value.i64);
            break;
        case JSDRV_UNION_F32:
            printf("[%s] f32=%f\n", topic, (double) value->value.f32);
            break;
        case JSDRV_UNION_F64:
            printf("[%s] f64=%f\n", topic, value->value.f64);
            break;
        default:
            printf("[%s] type=%u\n", topic, (unsigned) value->type);
            break;
    }
    fflush(stdout);
}

int on_pubsub_sniffer(struct app_s * self, int argc, char * argv[]) {
    if (argc < 2) {
        return sniff_usage();
    }

    const char * filter = argv[0];
    const char * topic_suffix = argv[1];

    ROE(app_match(self, filter));
    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));

    // Build full topic: "<device>/<topic_suffix>"
    struct jsdrv_topic_s sub_topic;
    jsdrv_topic_set(&sub_topic, self->device.topic);
    jsdrv_topic_append(&sub_topic, topic_suffix);

    printf("# Subscribing to %s\n", sub_topic.topic);
    printf("# Press CTRL-C to exit.\n");
    fflush(stdout);

    ROE(jsdrv_subscribe(self->context, sub_topic.topic, JSDRV_SFLAG_PUB,
                         on_publish, self, 0));

    while (!quit_) {
        jsdrv_thread_sleep_ms(10);
    }

    return 0;
}
