/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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

/**
 * @file
 *
 * @brief Minimal joulescope_driver program using only the public API.
 *
 * Open the first Joulescope, enable auto-ranging, statistics and current
 * streaming, print statistics for two seconds, then close and finalize.
 * See doc/getting_started.md and doc/streaming_topics.md.
 */

#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/topic.h"
#include "jsdrv/union.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RUN_DURATION_MS (2000)

struct app_s {
    struct jsdrv_context_s * context;
    char device[JSDRV_TOPIC_LENGTH_MAX];  // device path, such as u/js320/8W2A
    uint64_t sample_count;
    uint32_t block_count;
};

// Exit on error.  Real applications should clean up instead.
#define CHECK(x) do {                                                       \
    int32_t rc_ = (x);                                                      \
    if (rc_) {                                                              \
        printf("%s failed: %d %s\n", #x, (int) rc_, jsdrv_error_code_name(rc_)); \
        return rc_;                                                         \
    }                                                                       \
} while (0)

static void on_statistics(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if ((value->type != JSDRV_UNION_BIN) || (value->app != JSDRV_PAYLOAD_TYPE_STATISTICS)) {
        return;
    }
    const struct jsdrv_statistics_s * s = (const struct jsdrv_statistics_s *) value->value.bin;
    if (s->version != 1) {
        return;
    }
    printf("%.6f A, %.3f V, %.6f W, charge %.6f C, energy %.6f J\n",
           s->i_avg, s->v_avg, s->p_avg, s->charge_f64, s->energy_f64);
}

static void on_current(void * user_data, const char * topic,
                       const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) topic;
    if ((value->type != JSDRV_UNION_BIN) || (value->app != JSDRV_PAYLOAD_TYPE_STREAM)) {
        return;
    }
    const struct jsdrv_stream_signal_s * s = (const struct jsdrv_stream_signal_s *) value->value.bin;
    if ((s->version != 1) || (s->element_type != JSDRV_DATA_TYPE_FLOAT) || (s->element_size_bits != 32)) {
        return;
    }
    // const float * samples = (const float *) s->data;  // s->element_count values in amperes
    self->sample_count += s->element_count;
    self->block_count += 1;
}

// Select the first Joulescope from the comma-separated device list.
static int32_t device_select(struct app_s * self, const char * devices) {
    const char * start = devices;
    while (start && *start) {
        const char * end = strchr(start, ',');
        size_t len = end ? (size_t) (end - start) : strlen(start);
        const char * model = memchr(start, '/', len);  // {backend}/{model}/{serial_number}
        if (model && (len < sizeof(self->device)) && (0 == strncmp(model + 1, "js", 2))) {
            memcpy(self->device, start, len);
            self->device[len] = 0;
            return 0;
        }
        start = end ? end + 1 : NULL;
    }
    printf("No Joulescope found in device list \"%s\"\n", devices);
    return JSDRV_ERROR_NOT_FOUND;
}

static int32_t publish(struct app_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->device);
    jsdrv_topic_append(&t, subtopic);
    return jsdrv_publish(self->context, t.topic, value, JSDRV_TIMEOUT_MS_DEFAULT);
}

static int32_t subscribe(struct app_s * self, const char * subtopic, jsdrv_subscribe_fn cbk_fn) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->device);
    jsdrv_topic_append(&t, subtopic);
    return jsdrv_subscribe(self->context, t.topic, JSDRV_SFLAG_PUB, cbk_fn, self, JSDRV_TIMEOUT_MS_DEFAULT);
}

static int32_t run(struct app_s * self) {
    char devices[1024];
    struct jsdrv_union_s list = jsdrv_union_str(devices);
    list.size = sizeof(devices);

    CHECK(jsdrv_query(self->context, JSDRV_MSG_DEVICE_LIST, &list, JSDRV_TIMEOUT_MS_DEFAULT));
    CHECK(device_select(self, devices));
    printf("Using %s\n", self->device);

    CHECK(jsdrv_open(self->context, self->device, JSDRV_DEVICE_OPEN_MODE_DEFAULTS, JSDRV_TIMEOUT_MS_DEFAULT));
    if (0 == strncmp(self->device + 2, "js110", 5)) {
        CHECK(publish(self, "s/i/range/select", &jsdrv_union_cstr_r("auto")));
    } else {
        CHECK(publish(self, "s/i/range/mode", &jsdrv_union_cstr_r("auto")));
    }
    CHECK(publish(self, "s/i/ctrl", &jsdrv_union_u8_r(1)));  // enable streaming
    CHECK(publish(self, "s/v/ctrl", &jsdrv_union_u8_r(1)));
    CHECK(publish(self, "s/p/ctrl", &jsdrv_union_u8_r(1)));
    CHECK(publish(self, "s/stats/scnt", &jsdrv_union_u32_r(500000)));  // samples per block
    CHECK(publish(self, "s/stats/ctrl", &jsdrv_union_u8_r(1)));
    CHECK(subscribe(self, "s/stats/value", on_statistics));
    CHECK(subscribe(self, "s/i/!data", on_current));

    jsdrv_thread_sleep_ms(RUN_DURATION_MS);  // callbacks run on the driver thread

    CHECK(jsdrv_close(self->context, self->device, JSDRV_TIMEOUT_MS_DEFAULT));
    printf("Received %" PRIu64 " current samples in %" PRIu32 " blocks\n",
           self->sample_count, self->block_count);
    return 0;
}

int main(void) {
    struct app_s app;
    memset(&app, 0, sizeof(app));
    CHECK(jsdrv_initialize(&app.context, NULL, JSDRV_TIMEOUT_MS_INIT));
    int32_t rc = run(&app);
    jsdrv_finalize(app.context, 0);  // 0 = default timeout; also closes open devices
    return rc ? 1 : 0;
}
