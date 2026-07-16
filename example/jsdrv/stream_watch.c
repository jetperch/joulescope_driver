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

/**
 * @file
 *
 * @brief Watch i/v/p sample streams and log machine-readable evidence.
 *
 * Designed for host sleep/resume and unclean-close reproduction runs:
 * emits one JSON line per second to a file (flushed per line so evidence
 * survives kill -9 and host freezes), then exits with a code that encodes
 * the final streaming state.  See doc/plans/linux_host_sleep_repro.md.
 */

#include "jsdrv_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/time.h"
#include "jsdrv/union.h"
#include "jsdrv_prv/platform.h"  // jsdrv_time_utc
#include "jsdrv_prv/thread.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANNEL_COUNT (3U)
#define EXIT_RATE_FAIL (2)
#define EXIT_REMOVED (3)

// Counters are written only by the single jsdrv frontend pubsub thread
// (one writer) and read by the main loop; volatile suffices for this
// diagnostic tool (see example/minibitty/stream.c for the same pattern).
struct channel_s {
    const char * name;             // "i", "v", "p"
    volatile uint64_t samples;     // cumulative samples received
    volatile uint64_t msgs;        // cumulative !data messages received
    volatile uint64_t sample_id_last;
    uint64_t samples_prev;         // start-of-window snapshot (main thread)
};

struct stream_watch_s {
    struct app_s * app;
    struct channel_s channels[CHANNEL_COUNT];
    volatile uint32_t device_add_count;
    volatile uint32_t device_remove_count;
    volatile bool device_removed;    // our device is currently removed
    FILE * out;
};

static int usage(void) {
    printf(
        "usage: jsdrv stream_watch [OPTION]...\n"
        "options:\n"
        "  --device <filter>    Device filter prefix (default u/js320)\n"
        "  --duration <s>       Run duration in seconds (default 15)\n"
        "  --fs <hz>            Set h/fs before enabling streams\n"
        "  --min-rate <sps>     Per-channel pass threshold (default 1000)\n"
        "  --eval <s>           Trailing seconds that must pass (default 3)\n"
        "  --out <path>         JSON lines output (default stdout)\n"
        "exit code: 0=pass, 1=setup error, 2=rate criterion failed,\n"
        "           3=device removed without re-add\n");
    return 1;
}

// Wall time as UNIX epoch seconds, portable via the jsdrv clock
// (MSVC has no clock_gettime).
static double time_now(void) {
    return ((double) jsdrv_time_utc()) / ((double) JSDRV_TIME_SECOND)
        + (double) JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS;
}

static void on_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct channel_s * ch = (struct channel_s *) user_data;
    if ((value->type != JSDRV_UNION_BIN) || (value->app != JSDRV_PAYLOAD_TYPE_STREAM)) {
        return;
    }
    const struct jsdrv_stream_signal_s * s =
        (const struct jsdrv_stream_signal_s *) value->value.bin;
    ch->samples += s->element_count;
    ++ch->msgs;
    ch->sample_id_last = s->sample_id + (uint64_t) s->element_count * s->decimate_factor;
}

static void on_device_add(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct stream_watch_s * self = (struct stream_watch_s *) user_data;
    ++self->device_add_count;
    if ((value->type == JSDRV_UNION_STR)
            && (0 == strcmp(value->value.str, self->app->device.topic))) {
        self->device_removed = false;
    }
}

static void on_device_remove(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct stream_watch_s * self = (struct stream_watch_s *) user_data;
    ++self->device_remove_count;
    if ((value->type == JSDRV_UNION_STR)
            && (0 == strcmp(value->value.str, self->app->device.topic))) {
        self->device_removed = true;
    }
}

static int32_t publish_u32(struct app_s * self, const char * subtopic, uint32_t value) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->device.topic);
    jsdrv_topic_append(&t, subtopic);
    int32_t rc = jsdrv_publish(self->context, t.topic,
                               &jsdrv_union_u32_r(value), JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("publish %s=%" PRIu32 " failed: %" PRId32 " %s\n",
               t.topic, value, rc, jsdrv_error_code_name(rc));
    }
    return rc;
}

static int32_t subscribe_data(struct stream_watch_s * self, uint32_t idx) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->app->device.topic);
    jsdrv_topic_append(&t, "s");
    jsdrv_topic_append(&t, self->channels[idx].name);
    jsdrv_topic_append(&t, "!data");
    return jsdrv_subscribe(self->app->context, t.topic, JSDRV_SFLAG_PUB,
                           on_data, &self->channels[idx], JSDRV_TIMEOUT_MS_DEFAULT);
}

// Emit one JSON evidence line for the window [t_start, t_end).
// Returns true when every channel met min_rate over the window.
static bool window_emit(struct stream_watch_s * self, double t_start, double t_end,
                        uint32_t min_rate) {
    double dt = t_end - t_start;
    bool ok = (dt > 0.0);
    fprintf(self->out, "{\"t\":%.3f,\"dt\":%.3f", t_end, dt);
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        struct channel_s * ch = &self->channels[idx];
        uint64_t samples = ch->samples;
        uint64_t window = samples - ch->samples_prev;
        ch->samples_prev = samples;
        double rate = (dt > 0.0) ? ((double) window / dt) : 0.0;
        if (rate < (double) min_rate) {
            ok = false;
        }
        fprintf(self->out, ",\"%s\":{\"window\":%" PRIu64 ",\"total\":%" PRIu64
                ",\"sample_id\":%" PRIu64 "}",
                ch->name, window, samples,
                ch->sample_id_last);
    }
    bool removed = self->device_removed;
    if (removed) {
        ok = false;
    }
    fprintf(self->out, ",\"adds\":%u,\"removes\":%u,\"removed\":%s,\"ok\":%s}\n",
            (unsigned) self->device_add_count,
            (unsigned) self->device_remove_count,
            removed ? "true" : "false",
            ok ? "true" : "false");
    fflush(self->out);
    return ok;
}

int on_stream_watch(struct app_s * self, int argc, char * argv[]) {
    struct stream_watch_s watch;
    const char * device_filter = "u/js320";
    const char * out_path = NULL;
    uint32_t duration_s = 15U;
    uint32_t fs = 0U;
    uint32_t min_rate = 1000U;
    uint32_t eval_s = 3U;

    memset(&watch, 0, sizeof(watch));
    watch.app = self;
    watch.channels[0].name = "i";
    watch.channels[1].name = "v";
    watch.channels[2].name = "p";

    while (argc) {
        if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device_filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &duration_s));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--fs")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &fs));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--min-rate")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &min_rate));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--eval")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &eval_s));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--out")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            out_path = argv[0];
            ARG_CONSUME();
        } else {
            return usage();
        }
    }
    if ((0 == eval_s) || (eval_s > duration_s)) {
        return usage();
    }

    watch.out = stdout;
    if (out_path) {
        watch.out = fopen(out_path, "w");
        if (!watch.out) {
            printf("could not open --out %s\n", out_path);
            return 1;
        }
    }

    int rc = 1;
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB,
                        on_device_add, &watch, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB,
                        on_device_remove, &watch, JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(app_match(self, device_filter));
    printf("stream_watch device=%s duration=%" PRIu32 "s min_rate=%" PRIu32 "\n",
           self->device.topic, duration_s, min_rate);
    // The UI uses a generous open timeout; a leaked prior session can burn
    // ~750 ms in CONNECT_REQ retries before the handshake converges.
    ROE(jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_DEFAULTS, 5000));
    if (fs) {
        ROE(publish_u32(self, "h/fs", fs));
    }
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        ROE(subscribe_data(&watch, idx));
    }
    ROE(publish_u32(self, "s/i/ctrl", 1U));
    ROE(publish_u32(self, "s/v/ctrl", 1U));
    ROE(publish_u32(self, "s/p/ctrl", 1U));

    // Window loop.  Windows are wall-clock: a host sleep inside the run
    // shows up as one long window with its actual dt, so per-window rates
    // remain honest across the freeze.
    double t_start = time_now();
    double t_deadline = t_start + (double) duration_s;
    uint32_t ok_streak = 0U;
    uint32_t windows_total = 0U;
    while (!quit_) {
        double t_window_end = t_start + 1.0;
        while (!quit_) {
            double now = time_now();
            if ((now >= t_window_end) || (now >= t_deadline)) {
                break;
            }
            jsdrv_thread_sleep_ms(50);
        }
        double t_end = time_now();
        bool ok = window_emit(&watch, t_start, t_end, min_rate);
        ++windows_total;
        ok_streak = ok ? (ok_streak + 1U) : 0U;
        t_start = t_end;
        if (t_end >= t_deadline) {
            break;
        }
    }

    if (watch.device_removed) {
        rc = EXIT_REMOVED;
    } else if (ok_streak >= eval_s) {
        rc = 0;
    } else {
        rc = EXIT_RATE_FAIL;
    }
    printf("stream_watch result: rc=%d ok_streak=%" PRIu32 "/%" PRIu32
           " windows=%" PRIu32 "\n", rc, ok_streak, eval_s, windows_total);

    // Clean close (the unclean-close scenarios kill this process instead).
    publish_u32(self, "s/i/ctrl", 0U);
    publish_u32(self, "s/v/ctrl", 0U);
    publish_u32(self, "s/p/ctrl", 0U);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    if (out_path) {
        fclose(watch.out);
    }
    return rc;
}
