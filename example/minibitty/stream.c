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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jsdrv/os_thread.h"


#define MB_TOPIC_LENGTH_MAX (32U)
#define LINK_PING_SIZE_U32 ((512U - 12U) >> 2)
#define PUBSUB_PING_SIZE_U32 ((512U - 12U - 8U - MB_TOPIC_LENGTH_MAX) >> 2)  // frame, stdmsg, publish
#define STREAM_TIMEOUT_S (5)


enum loopback_location_e {
    LOCATION_LINK = 0,
    LOCATION_PUBSUB = 1,
};

static volatile time_t last_data_time_ = 0;

void on_u4_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    static uint32_t counter = 0;
    (void) user_data;
    (void) topic;
    (void) value;
    last_data_time_ = time(NULL);
    if (counter == 1000) {
        const uint32_t * p32 = &value->value.u32;
        printf("%d\n", p32[32] & 0x0f);
        counter = 0;
    } else {
        ++counter;
    }
}

void on_i32_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    static uint32_t counter = 0;
    (void) user_data;
    (void) topic;
    (void) value;
    last_data_time_ = time(NULL);
    if (counter == 1000) {
        const uint32_t * p32 = &value->value.u32;
        printf("0x%08x %d\n", p32[32], p32[32]);
        counter = 0;
    } else {
        ++counter;
    }
}

void on_f32_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) value->value.bin;
    static double f_mean = 0.0;
    static uint32_t counter = 0;
    (void) user_data;
    (void) topic;
    (void) value;
    last_data_time_ = time(NULL);

    const float * f32 = (const float *) &signal->data[0];

    for (uint32_t i = 0; i < signal->element_count; ++i) {
        f_mean += (double) f32[i];
    }
    counter += signal->element_count;

    if (counter >= 500000) {
        f_mean /= counter;
        printf("%f", f_mean);
        //for (uint32_t i = 0; i < signal->element_count; ++i) {
        //    if (0 == (i & 0x7)) {
        //        printf("\n    %f", f32[i]);
        //    } else {
        //        printf(", %f", f32[i]);
        //    }
        //}
        printf("\n");
        f_mean = 0.0;
        counter = 0;
    }
}

void on_stats_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    (void) value;
    last_data_time_ = time(NULL);
    printf("stats\n");
}

#define PUBLISH_U32(topic_, value_) \
    jsdrv_topic_set(&topic, self->device.topic); \
    jsdrv_topic_append(&topic, topic_); \
    rc = jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(value_), JSDRV_TIMEOUT_MS_DEFAULT); \
    if (rc) { \
        printf("publish %s failed with %d\n", topic.topic, (int) rc); \
    }


static int run(struct app_s * self, const char * device) {
    struct jsdrv_topic_s topic;
    int32_t rc = 0;

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_thread_sleep_ms(100);     // todo improved way to detect sensor ready

    PUBLISH_U32("c/led/red", 0);
    PUBLISH_U32("c/led/green", 0x0f);
    PUBLISH_U32("c/led/blue", 0xf0);
    PUBLISH_U32("c/led/en", 1);

    PUBLISH_U32("s/led/red", 0);
    PUBLISH_U32("s/led/green", 0x0f);
    PUBLISH_U32("s/led/blue", 0xf0);
    PUBLISH_U32("s/led/en", 1);

    //PUBLISH_U32("v/range/mode", 1);
    //PUBLISH_U32("v/range/select", 1);


    if (0) {
        // manual current ADCs
        PUBLISH_U32("s/i/range/mode", 5);       // manual
        PUBLISH_U32("s/i/range/select", 0x84);
        PUBLISH_U32("s/i/i0_sel", 2);
        PUBLISH_U32("s/i/i1_sel", 2);
        PUBLISH_U32("s/i/i2_sel", 2);
        PUBLISH_U32("s/adc/0/sel", 2);
        PUBLISH_U32("s/adc/0/ctrl", 1);
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/adc/0/!data");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_i32_data, NULL, 0);
    } else if (1) {
        // manual current calibration
        PUBLISH_U32("s/sys/tick/en", 1);        // disable sys/tick/!ev
        PUBLISH_U32("s/i/range/mode", 4);       // auto
        //PUBLISH_U32("s/i/range/mode", 5);       // manual
        //PUBLISH_U32("s/i/range/select", 0x84);
        PUBLISH_U32("s/i/i0_sel", 2);
        PUBLISH_U32("s/i/i1_sel", 2);
        PUBLISH_U32("s/i/i2_sel", 2);
        PUBLISH_U32("s/i/ctrl", 1);
        PUBLISH_U32("s/v/ctrl", 1);
        PUBLISH_U32("s/i/range/ctrl", 1);
        PUBLISH_U32("s/stats/ctrl", 1);         // stream statistics
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/i/!data");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_f32_data, NULL, 0);

        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/i/range/!data");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_u4_data, NULL, 0);

        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/stats/value");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_stats_data, NULL, 0);
    } else if (0) {
        // manual voltage
        PUBLISH_U32("s/v/range/mode", 1);       // manual
        PUBLISH_U32("s/v/range/select:", 0);    // 15 V
        PUBLISH_U32("s/v/ctrl", 1);
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/v/!data");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_f32_data, NULL, 0);
    } else if (1) {
        // current range
        PUBLISH_U32("s/i/range/ctrl", 1);
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "s/i/range/!data");
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_u4_data, NULL, 0);
    }


    last_data_time_ = time(NULL);
    time_t end_time = self->duration_ms
        ? (time(NULL) + (self->duration_ms + 999) / 1000)
        : 0;

    while (!quit_) {
        jsdrv_thread_sleep_ms(100);
        time_t now = time(NULL);
        if ((now - last_data_time_) >= STREAM_TIMEOUT_S) {
            printf("ERROR: no data received for %d seconds\n", STREAM_TIMEOUT_S);
            rc = 1;
            break;
        }
        if (end_time && (now >= end_time)) {
            break;  // duration elapsed, success
        }
    }

    PUBLISH_U32("s/adc/0/ctrl", 0);
    PUBLISH_U32("s/adc/1/ctrl", 0);
    PUBLISH_U32("s/i/range/ctrl", 0);
    PUBLISH_U32("s/i/ctrl", 0);
    PUBLISH_U32("s/v/ctrl", 0);
    PUBLISH_U32("s/p/ctrl", 0);
    PUBLISH_U32("s/stats/ctrl", 0);

    jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);
    return rc;
}

static int usage(void) {
    printf(
        "usage: minibitty stream [--duration <ms>] device_path\n"
        "\n"
        "  --duration <ms>  Run for duration then exit 0. If no data\n"
        "                   received for 5 seconds, exit with error.\n"
        );
    return 1;
}

int on_stream(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;
    self->duration_ms = 0;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            self->duration_ms = (uint32_t) strtoul(argv[0], NULL, 10);
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
