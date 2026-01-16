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

#include <windows.h>  // todo remove


#define MB_TOPIC_LENGTH_MAX (32U)
#define LINK_PING_SIZE_U32 ((512U - 12U) >> 2)
#define PUBSUB_PING_SIZE_U32 ((512U - 12U - 8U - MB_TOPIC_LENGTH_MAX) >> 2)  // frame, stdmsg, publish


enum loopback_location_e {
    LOCATION_LINK = 0,
    LOCATION_PUBSUB = 1,
};


struct loopback_s {
    uint64_t count;                 // total count
    uint64_t outstanding;           // outstanding messages in flight
    uint64_t size_u32;              // size of ping data in u32
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    volatile uint64_t ping_count;   // current TX ping count
    volatile uint64_t pong_count;   // current RX pong count
    HANDLE event;
};

struct loopback_s loopback_ = {
    .count = 0,
    .outstanding = 1,
    .size_u32 = LINK_PING_SIZE_U32,
    .ping_count = 0,
    .pong_count = 0,
    .event = NULL,
};

static void on_device_stats(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    uint32_t * p_u32 = (uint32_t *) value->value.bin;
    printf("%s: %d\n", topic, p_u32[7]);
}


static void on_pong(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const uint32_t * p_u32 = (const uint32_t *) value->value.bin;
    if (value->size != (loopback_.size_u32 * 4)) {
        printf("ERROR pong size = %u\n", value->size);
        quit_ = true;
        return;
    }
    for (uint32_t i = 0; i < loopback_.size_u32; ++i) {
        if (p_u32[i] != (uint32_t) (loopback_.pong_count + i)) {
            if (0 == loopback_.pong_count) {
                printf("ERROR pong unsynced: %llu %lu %u: %lu != %lu\n",
                    loopback_.pong_count, value->size, i,
                    p_u32[i], (uint32_t) (loopback_.pong_count + i));
                return;
            }
            printf("ERROR pong %llu %lu %u: %lu != %lu\n", loopback_.pong_count, value->size, i,
                p_u32[i], (uint32_t) (loopback_.pong_count + i));
            quit_ = true;
            return;
        }
    }
    loopback_.pong_count++;
    SetEvent(loopback_.event);
}

static int link_lookback(struct app_s * self, const char * device) {
    struct jsdrv_topic_s pong_topic;
    struct jsdrv_topic_s ping_topic;
    struct jsdrv_topic_s topic;
    uint64_t pong_prev = 0;
    int64_t time_prev = jsdrv_time_utc();
    int64_t time_now = 0;
    int64_t time_delta = 0;

    jsdrv_topic_set(&ping_topic, self->device.topic);
    jsdrv_topic_set(&pong_topic, self->device.topic);
    jsdrv_topic_append(&ping_topic, loopback_.topic);
    jsdrv_topic_append(&pong_topic, loopback_.topic);
    jsdrv_topic_append(&ping_topic, "!ping");
    jsdrv_topic_append(&pong_topic, "!pong");

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_subscribe(self->context, pong_topic.topic, JSDRV_SFLAG_PUB, on_pong, NULL, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/comm/usbd/0/tx/!stat");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_device_stats, NULL, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/comm/usbd/0/rx/!stat");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_device_stats, NULL, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/in/frecord");
    // jsdrv_publish(self->context, topic.topic, &jsdrv_union_cstr("out.bin"), 0);

    fflush(stdout);


    while (!quit_) {
        ResetEvent(loopback_.event);
        if (loopback_.count && (loopback_.ping_count >= loopback_.count) && (loopback_.pong_count >= loopback_.count)) {
            break;
        }

        while ((loopback_.ping_count - loopback_.pong_count) < loopback_.outstanding) {
            if (loopback_.count && (loopback_.ping_count >= loopback_.count)) {
                break;
            }
            uint32_t ping_data[512 >> 2];
            for (uint32_t i = 0; i < loopback_.size_u32; ++i) {
                ping_data[i] = loopback_.ping_count + i;
            }
            jsdrv_publish(self->context, ping_topic.topic, &jsdrv_union_bin((uint8_t *) ping_data, loopback_.size_u32 * 4), 0);
            ++loopback_.ping_count;
        }
        time_now = jsdrv_time_utc();
        time_delta = time_now - time_prev;
        if (time_delta > JSDRV_TIME_SECOND) {
            uint64_t pong_delta = loopback_.pong_count - pong_prev;
            printf("Throughput: %llu frames = %llu bytes\n", pong_delta, pong_delta * loopback_.size_u32 * 4);
            fflush(stdout);
            time_prev = time_now;
            pong_prev = loopback_.pong_count;
        }
        WaitForSingleObject(loopback_.event, 1);
    }

    jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);
    return 0;
}

static int usage(void) {
    printf(
        "usage: minibitty loopback [options] device_path\n"
        "options:\n"
        "  --count {n}        The total number of messages to send. [0]\n"
        "  --outstanding {n}  The number of in-flight messages. [1]\n"
        "  --topic {t}        The loopback topic prefix [h/link]\n"
        );
    return 1;
}

int on_loopback(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;
    jsdrv_cstr_copy(loopback_.topic, "h/link", sizeof(loopback_.topic));

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--count")) || (0 == strcmp(argv[0], "-c"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u64(argv[0], &loopback_.count)) {
                printf("ERROR: invalid --count value\n");
                return usage();
            }
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--outstanding")) || (0 == strcmp(argv[0], "-o"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u64(argv[0], &loopback_.outstanding)) {
                printf("ERROR: invalid --outstanding value\n");
                return usage();
            }
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--topic")) || (0 == strcmp(argv[0], "-t"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_copy(loopback_.topic, argv[0], sizeof(loopback_.topic))) {
                printf("ERROR: invalid --topic value\n");
            }
            if (0 == jsdrv_cstr_casecmp(argv[0], "h/link")) {
                loopback_.size_u32 = LINK_PING_SIZE_U32;
            } else {
                loopback_.size_u32 = PUBSUB_PING_SIZE_U32;
            }
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

    loopback_.event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );

    // todo loopback @ pubsub layer

    return link_lookback(self, self->device.topic);
}
