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
 * @brief Confirmed JS320 sensor publishes while streaming.
 *
 * Streams a chosen set of signals to load the sensor link, then sends
 * confirmed publishes to a sensor topic and counts the ones whose return
 * code does not arrive.  See
 * js320/doc/plans/completed/open_stats_ctrl_timeout.md (outbound drops).
 */

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv/os_thread.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct stream_s {
    const char * name;
    const char * ctrl_topic;
};

static const struct stream_s STREAMS[] = {
    {"i", "s/i/ctrl"},
    {"v", "s/v/ctrl"},
    {"p", "s/p/ctrl"},
    {"range", "s/i/range/ctrl"},
    {"adc0", "s/adc/0/ctrl"},
    {"adc1", "s/adc/1/ctrl"},
    {"gpi0", "s/gpi/0/ctrl"},
    {"gpi1", "s/gpi/1/ctrl"},
    {"gpi2", "s/gpi/2/ctrl"},
    {"gpi3", "s/gpi/3/ctrl"},
    {"gpi7", "s/gpi/7/ctrl"},
    {"stats", "s/stats/ctrl"},
};
#define STREAM_COUNT (sizeof(STREAMS) / sizeof(STREAMS[0]))

static volatile uint64_t link_bytes_ = 0;
static volatile uint64_t link_frames_ = 0;

static int usage(void) {
    printf(
        "usage: minibitty publish_load [options] <device>\n"
        "\n"
        "Stream signals to load the JS320 sensor link, then send confirmed\n"
        "publishes to a sensor topic and count the ones that time out.\n"
        "\n"
        "Options:\n"
        "  --streams <list>    Comma-separated streams to enable [i,v]:\n"
        "                      i, v, p, range, adc0, adc1, gpi0..gpi3, gpi7, stats, all.\n"
        "  --count <N>         Confirmed publishes [200].\n"
        "  --period-ms <ms>    Time between publishes [20].\n"
        "  --timeout-ms <ms>   Return code timeout per publish [1000].\n"
        "  --settle-ms <ms>    Stream before the first publish [2000].\n"
        "Exits nonzero if any publish fails.\n"
    );
    return 1;
}

// mb_comm_stats_v1_s after the 8-byte stdmsg header: two u64 window
// counters, then frames and bytes (u32).
static void on_link_stat(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if ((value->type != JSDRV_UNION_STDMSG) || (value->size < (8 + 16 + 8))) {
        return;
    }
    uint32_t u32[2];
    memcpy(u32, value->value.bin + 8 + 16, sizeof(u32));
    link_frames_ += u32[0];
    link_bytes_ += u32[1];
}

static int32_t publish_u32(struct app_s * self, const char * subtopic, uint32_t value, uint32_t timeout_ms) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, subtopic);
    return jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(value), timeout_ms);
}

static void streams_set(struct app_s * self, const bool * enable, uint32_t value) {
    for (uint32_t k = 0; k < STREAM_COUNT; ++k) {
        if (enable[k]) {
            int32_t rc = publish_u32(self, STREAMS[k].ctrl_topic, value, JSDRV_TIMEOUT_MS_DEFAULT);
            if (rc) {
                printf("publish %s=%u failed: %d\n", STREAMS[k].ctrl_topic, value, (int) rc);
            }
        }
    }
}

static int32_t streams_parse(const char * text, bool * enable) {
    char name[16];
    while (*text) {
        size_t n = 0;
        while (text[n] && (text[n] != ',')) {
            ++n;
        }
        if ((n == 0) || (n >= sizeof(name))) {
            return 1;
        }
        memcpy(name, text, n);
        name[n] = 0;
        bool found = false;
        for (uint32_t k = 0; k < STREAM_COUNT; ++k) {
            if ((0 == strcmp(name, "all")) || (0 == strcmp(name, STREAMS[k].name))) {
                enable[k] = true;
                found = true;
            }
        }
        if (!found) {
            printf("Unknown stream: %s\n", name);
            return 1;
        }
        text += n;
        if (*text == ',') {
            ++text;
        }
    }
    return 0;
}

static int32_t arg_u32(char * argv[], uint32_t * value) {
    if (jsdrv_cstr_to_u32(argv[0], value)) {
        printf("Invalid value: %s\n", argv[0]);
        return 1;
    }
    return 0;
}

int on_publish_load(struct app_s * self, int argc, char * argv[]) {
    bool enable[STREAM_COUNT];
    memset(enable, 0, sizeof(enable));
    const char * filter = NULL;
    const char * streams = "i,v";
    uint32_t count = 200;
    uint32_t period_ms = 20;
    uint32_t timeout_ms = 1000;
    uint32_t settle_ms = 2000;

    while (argc) {
        if (argv[0][0] != '-') {
            filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--streams")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            streams = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--count")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &count));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--period-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &period_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--timeout-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &timeout_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--settle-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &settle_ms));
            ARG_CONSUME();
        } else {
            printf("Unknown argument: %s\n", argv[0]);
            return usage();
        }
    }
    if (!filter || streams_parse(streams, enable)) {
        return usage();
    }
    ROE(app_match_ex(self, filter, APP_MATCH_EXPLICIT | APP_MATCH_MB));
    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, 10000));

    struct jsdrv_topic_s stat_topic;
    jsdrv_topic_set(&stat_topic, self->device.topic);
    jsdrv_topic_append(&stat_topic, "c/comm/sensor/rx/!stat");
    jsdrv_subscribe(self->context, stat_topic.topic, JSDRV_SFLAG_PUB, on_link_stat, NULL, 0);

    streams_set(self, enable, 1);
    jsdrv_thread_sleep_ms(settle_ms);
    link_bytes_ = 0;
    link_frames_ = 0;
    uint32_t t_start = jsdrv_time_ms_u32();

    uint32_t fail = 0;
    uint32_t latency_max_ms = 0;
    uint64_t latency_sum_ms = 0;
    for (uint32_t k = 0; (k < count) && !quit_; ++k) {
        uint32_t t0 = jsdrv_time_ms_u32();
        int32_t rc = publish_u32(self, "s/stats/scnt", 500000U + (k & 1U), timeout_ms);
        uint32_t dt = jsdrv_time_ms_u32() - t0;
        if (rc) {
            ++fail;
            printf("publish %u failed: %d %s\n", k, (int) rc, jsdrv_error_code_name(rc));
        } else {
            latency_sum_ms += dt;
            latency_max_ms = (dt > latency_max_ms) ? dt : latency_max_ms;
        }
        jsdrv_thread_sleep_ms(period_ms);
    }

    // Wall time spans the stats windows to within one statistics tick.
    double wall_s = (double) (jsdrv_time_ms_u32() - t_start) / 1000.0;
    double mbps = (wall_s > 0.0) ? (8.0 * (double) link_bytes_ / wall_s / 1e6) : 0.0;
    uint32_t ok = count - fail;
    printf("streams=%s link_rx=%.1f Mbit/s (%.1f s) frames=%" PRIu64 " publishes=%u ok=%u fail=%u "
           "latency_avg=%.1f ms latency_max=%u ms\n",
           streams, mbps, wall_s, (uint64_t) link_frames_, count, ok, fail,
           ok ? ((double) latency_sum_ms / ok) : 0.0, latency_max_ms);

    jsdrv_unsubscribe(self->context, stat_topic.topic, on_link_stat, NULL, 0);
    streams_set(self, enable, 0);
    publish_u32(self, "s/stats/scnt", 500000U, timeout_ms);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    return fail ? 1 : 0;
}
