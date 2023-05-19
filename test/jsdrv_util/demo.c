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
#include "jsdrv/cstr.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <inttypes.h>


static volatile bool removed_ = false;


static void on_device_remove(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) topic;
    if (value->type == JSDRV_UNION_STR) {
        if (0 == strcmp(self->device.topic, value->value.str)) {
            printf("device removed\n");
            removed_ = true;
        }
    }
}

static void on_pub_cmd(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    static int counter = 0;
    char buffer[256];
    if (value->app == JSDRV_PAYLOAD_TYPE_STREAM) {
        ++counter;
        char ch = '?';
        if (!jsdrv_cstr_ends_with(topic, "/!data")) {
            ch = 'X';
            //printf("JSDRV_PAYLOAD_TYPE_STREAM but topic is %s\n", topic);
        } else if (jsdrv_cstr_ends_with(topic, "i/!data")) {
            ch = 'i';
        } else if (jsdrv_cstr_ends_with(topic, "v/!data")) {
            ch = 'v';
        } else if (jsdrv_cstr_ends_with(topic, "p/!data")) {
            ch = 'p';
        } else if (jsdrv_cstr_ends_with(topic, "i/range/!data")) {
            ch = 'r';
        } else if (jsdrv_cstr_ends_with(topic, "gpi/0/!data")) {
            ch = '0';
        } else if (jsdrv_cstr_ends_with(topic, "gpi/1/!data")) {
            ch = '1';
        }
        putc(ch, stdout);
        if (counter > 64) {
            putc('\n', stdout);
            fflush(stdout);
            counter = 0;
        }
        if (self->sleep_ms) {
            jsdrv_thread_sleep_ms(self->sleep_ms);
        }
        // struct jsdrv_stream_signal_s * s = (struct jsdrv_stream_signal_s *) value->value.bin;
        // printf("on_pub_data(%s) sample_id=%" PRIu64 ", count=%u\n", topic, s->sample_id, s->element_count);
    } else if (value->app == JSDRV_PAYLOAD_TYPE_UNION) {
        jsdrv_union_value_to_str(value, buffer, sizeof(buffer), 1);
        JSDRV_LOGI("pub %s => %s", topic, buffer);
    }
}

static int32_t publish(struct app_s * self, const char * device, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    char buf[32];
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, topic);
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("Publish %s => %s\n", t.topic, buf);
    int32_t rc = jsdrv_publish(self->context, t.topic, value, timeout_ms);
    if (rc) {
        printf("publish %s failed with %d\n", topic, (int) rc);
    }
    return rc;
}

static int usage(void) {
    printf("usage: jsdrv_util demo [<option> <value>] ..."
           "\n"
           "Options:\n"
           "    --duration      The duration in milliseconds.\n"
           "                    0 (default) runs until CTRL-C\n"
           "    --pub_sleep     The amount to sleep in publis, in milliseconds.\n"
           "                    Block the pubsub thread for overflow testing.\n"
           "\n");
    return 1;
}

static bool wait_for_duration_ms(uint32_t duration_ms) {
    int64_t t_end = INT64_MAX;;
    if (duration_ms > 0) {
        t_end = jsdrv_time_utc() + JSDRV_TIME_MILLISECOND * (int64_t) duration_ms;
    }
    while (!quit_ && !removed_ && (jsdrv_time_utc() < t_end)) {
        jsdrv_thread_sleep_ms(10);
    }
    return !removed_;
}

int on_demo(struct app_s * self, int argc, char * argv[]) {
    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &self->duration_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--pub_sleep")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &self->sleep_ms));
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, NULL));
    char * device = self->device.topic;
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB, on_device_remove, self, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, device, JSDRV_MSG_OPEN, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_subscribe(self->context, device, JSDRV_SFLAG_PUB, on_pub_cmd, self, JSDRV_TIMEOUT_MS_DEFAULT));

    if (jsdrv_cstr_starts_with(device, "u/js220")) {
        ROE(publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("10 A"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/i/range/mode", &jsdrv_union_cstr_r("manual"), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/adc/0/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/p/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/i/range/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/gpi/0/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/gpi/1/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        if (wait_for_duration_ms(self->duration_ms)) {
            ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            ROE(publish(self, device, "s/p/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            //ROE(publish(self, device, "s/i/range/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            ROE(publish(self, device, "s/gpi/0/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            ROE(publish(self, device, "s/gpi/1/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
        } else {
            publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
            publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
            publish(self, device, "s/p/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
            //publish(self, device, "s/i/range/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
            publish(self, device, "s/gpi/0/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
            publish(self, device, "s/gpi/1/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
        }
    } else if (jsdrv_cstr_starts_with(device, "u/js110")) {
        ROE(publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("auto"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        //ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        if (wait_for_duration_ms(self->duration_ms)) {
            ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
            //ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
        }
    } else {
        printf("Unsupported device: %s\n", device);
    }

    ROE(publish(self, device, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_unsubscribe(self->context, device, on_pub_cmd, self, JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
