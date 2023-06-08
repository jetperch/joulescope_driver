/*
 * Copyright 2022-2023 Jetperch LLC
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
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <inttypes.h>
#include <math.h>


static volatile bool _removed = false;
static const char RESPONSE_TOPIC[] = "m/007/!rsp";
static uint32_t request_length = 100;


static void on_device_remove(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) topic;
    if (value->type == JSDRV_UNION_STR) {
        if (0 == strcmp(self->device.topic, value->value.str)) {
            printf("device removed\n");
            _removed = true;
        }
    }
}

static void on_pub_cmd(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) self;
    char buffer[256];
    if (value->app == JSDRV_PAYLOAD_TYPE_STREAM) {
        // ignore
    } else if (value->app == JSDRV_PAYLOAD_TYPE_UNION) {
        jsdrv_union_value_to_str(value, buffer, sizeof(buffer), 1);
                JSDRV_LOGI("pub %s => %s", topic, buffer);
    }
}

static void on_buf_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if ((value->type != JSDRV_UNION_BIN) || (value->app != JSDRV_PAYLOAD_TYPE_BUFFER_RSP)) {
        printf("response value type invalid\n");
        return;
    }
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) value->value.bin;
    if (rsp->version != 1) {
        printf("response version unsupported: %d\n", (int) rsp->version);
        return;
    }

    if (1) {
        const char * rsp_type;
        switch (rsp->response_type) {
            case JSDRV_BUFFER_RESPONSE_SAMPLES: rsp_type = "samples"; break;
            case JSDRV_BUFFER_RESPONSE_SUMMARY: rsp_type = "summary"; break;
            default: rsp_type = "unknown"; break;
        };
        int64_t duration = rsp->info.time_range_utc.end - rsp->info.time_range_utc.start;
        float * f32 = (float *) rsp->data;
        if (fabs(f32[0]) >= 20.0) {
            printf("response %s rsp_id=%" PRIu64 ", duration=%.3f, length=%" PRIu64 ", %g, %g, %g, %g\n",
                   rsp_type, rsp->rsp_id,
                   (double) duration / JSDRV_TIME_SECOND,
                   rsp->info.time_range_samples.length,
                   f32[0], f32[1], f32[2], f32[3]);
        }
    }
}

static void on_buf_info(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) topic;
    if ((value->type != JSDRV_UNION_BIN) || (value->app != JSDRV_PAYLOAD_TYPE_BUFFER_INFO)) {
        printf("info value type invalid\n");
        return;
    }
    struct jsdrv_buffer_info_s * info = (struct jsdrv_buffer_info_s *) value->value.bin;
    if (info->version != 1) {
        printf("info version unsupported: %d\n", (int) info->version);
        return;
    }

    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    if (0) {
        req.time_type = JSDRV_TIME_SAMPLES;
        req.time.samples.start = info->time_range_samples.start;
        req.time.samples.end = info->time_range_samples.end;
        req.time.samples.length = request_length;  // summary
    } else {
        int64_t duration = info->time_range_utc.end - info->time_range_utc.start;
        duration = duration / 5;
        req.time_type = JSDRV_TIME_UTC;
        req.time.utc.start = info->time_range_utc.start + duration;
        req.time.utc.end = info->time_range_utc.end - duration;
        req.time.utc.length = request_length;  // summary
    }
    jsdrv_cstr_copy(req.rsp_topic, RESPONSE_TOPIC, sizeof(req.rsp_topic));
    req.rsp_id = 1;
    struct jsdrv_union_s req_value = jsdrv_union_cbin((uint8_t *) &req, sizeof(req));
    req_value.app = JSDRV_PAYLOAD_TYPE_BUFFER_REQ;
    jsdrv_publish(self->context, "m/007/s/003/!req", &req_value, 0);
}

static int32_t publish(struct app_s * self, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    char buf[32];
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("Publish %s => %s\n", topic, buf);
    int32_t rc = jsdrv_publish(self->context, topic, value, timeout_ms);
    if (rc) {
        printf("publish %s failed with %d\n", topic, (int) rc);
    }
    return rc;
}

static int32_t device_publish(struct app_s * self, const char * device, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, topic);
    return publish(self, t.topic, value, timeout_ms);
}

static int usage(void) {
    printf("usage: jsdrv_util stream_buffer [--option value]\n"
           "    --duration   duration in milliseconds\n"
           "    --length     number of entries per summary request\n"
           "    --mem_size   Memory size in bytes (< 4294967296)\n"
           );
    return 1;
}

static bool wait_for_duration_ms(uint32_t duration_ms) {
    int64_t t_end = jsdrv_time_utc() + JSDRV_TIME_MILLISECOND * (int64_t) duration_ms;
    while (!_removed && (jsdrv_time_utc() < t_end)) {
        jsdrv_thread_sleep_ms(10);
    }
    return !_removed;
}

int on_stream_buffer(struct app_s * self, int argc, char * argv[]) {
    uint32_t duration_ms = 5000;
    uint32_t mem_size = 100000000U;
    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &duration_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--length")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &request_length));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--mem_size")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &mem_size));
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, NULL));
    char * device = self->device.topic;
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB, on_device_remove, self, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(device_publish(self, device, JSDRV_MSG_OPEN, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_subscribe(self->context, device, JSDRV_SFLAG_PUB, on_pub_cmd, self, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_subscribe(self->context, RESPONSE_TOPIC, JSDRV_SFLAG_PUB, on_buf_rsp, self, JSDRV_TIMEOUT_MS_DEFAULT));

    if (jsdrv_cstr_starts_with(device, "u/js220")) {
        ROE(device_publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("10 A"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(device_publish(self, device, "s/i/range/mode", &jsdrv_union_cstr_r("manual"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/@/!add",  &jsdrv_union_u8_r(7), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/007/a/!add",  &jsdrv_union_u8_r(3), JSDRV_TIMEOUT_MS_DEFAULT));
        struct jsdrv_topic_s t;
        jsdrv_topic_set(&t, device);
        const char * ctrl_str;
        if (1) {
            jsdrv_topic_append(&t, "s/i/!data");
            ctrl_str = "s/i/ctrl";
        } else {
            jsdrv_topic_append(&t, "s/gpi/0/!data");
            ctrl_str = "s/gpi/0/ctrl";
        }
        ROE(publish(self, "m/007/s/003/topic",  &jsdrv_union_cstr_r(t.topic), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(jsdrv_subscribe(self->context, "m/007/s/003/info", JSDRV_SFLAG_PUB, on_buf_info, self, JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/007/g/size",  &jsdrv_union_u64_r(mem_size), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(device_publish(self, device, ctrl_str, &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        if (wait_for_duration_ms(duration_ms)) {
            ROE(device_publish(self, device, ctrl_str, &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
        } else {
            device_publish(self, device, ctrl_str, &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
        }
    } else if (jsdrv_cstr_starts_with(device, "u/js110")) {
        ROE(device_publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("auto"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/@/!add",  &jsdrv_union_u8_r(7), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/007/a/!add",  &jsdrv_union_u8_r(3), JSDRV_TIMEOUT_MS_DEFAULT));
        struct jsdrv_topic_s t;
        jsdrv_topic_set(&t, device);
        jsdrv_topic_append(&t, "s/i/!data");
        ROE(publish(self, "m/007/s/003/topic",  &jsdrv_union_cstr_r(t.topic), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(jsdrv_subscribe(self->context, "m/007/s/003/info", JSDRV_SFLAG_PUB, on_buf_info, self, JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, "m/007/g/size",  &jsdrv_union_u64_r(mem_size), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(device_publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        if (wait_for_duration_ms(duration_ms)) {
            ROE(device_publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
        } else {
            device_publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
        }
    } else {
        printf("Unsupported device: %s\n", device);
    }

    ROE(device_publish(self, device, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_unsubscribe(self->context, device, on_pub_cmd, self, JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
