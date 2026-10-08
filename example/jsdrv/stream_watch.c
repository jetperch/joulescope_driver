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
 * @brief Watch i/v/p sample streams and log machine-readable evidence.
 *
 * Designed for host sleep/resume and unclean-close reproduction runs:
 * emits one JSON line per second to a file (flushed per line so evidence
 * survives kill -9 and host freezes), then exits with a code that encodes
 * the final streaming state.  See doc/plans/linux_host_sleep_repro.md.
 *
 * With --cycles, repeatedly stops and restarts the streams instead, and
 * reports sample_id skips and stale first frames per cycle.  See
 * doc/plans/js320_stream_discontinuity.md.
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
    bool enabled;                  // selected by --signals
    volatile uint64_t samples;     // cumulative samples received
    volatile uint64_t msgs;        // cumulative !data messages received
    volatile uint64_t sample_id_last;
    uint64_t samples_prev;         // start-of-window snapshot (main thread)

    // Restart cycle checks (--cycles), reset by the main thread while stopped.
    volatile uint64_t skips;       // messages whose sample_id != sample_id_last
    volatile uint64_t first_id;    // sample_id of the first message, 0 if none
    volatile int64_t first_utc;    // arrival time of the first message
    volatile int64_t last_utc;     // arrival time of the latest message
    volatile uint32_t tick_rate;   // sample_id rate (Hz)
    volatile uint64_t skip_expected;  // first skip: expected sample_id
    volatile uint64_t skip_received;  // first skip: received sample_id
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
        "  --cycles <n>         Stop and restart the streams n times instead\n"
        "  --on-ms <ms>         Streaming time per cycle (default 500)\n"
        "  --off-ms <ms>        Stopped time per cycle (default 1000)\n"
        "  --fs-alt <hz>        Alternate h/fs with --fs while stopped\n"
        "  --fs-delay-ms <ms>   Delay from stop to the h/fs change (default 0)\n"
        "  --signals <chars>    Streams to enable from i, v, p (default ivp)\n"
        "exit code: 0=pass, 1=setup error, 2=rate criterion failed,\n"
        "           3=device removed without re-add, 4=restart cycle failed\n");
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
    int64_t now = jsdrv_time_utc();
    if (0 == ch->first_id) {
        ch->first_id = s->sample_id;
        ch->first_utc = now;
    } else if (s->sample_id != ch->sample_id_last) {
        if (0 == ch->skips) {
            ch->skip_expected = ch->sample_id_last;
            ch->skip_received = s->sample_id;
        }
        ++ch->skips;
    }
    ch->last_utc = now;
    ch->tick_rate = s->sample_rate;
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

#define EXIT_CYCLE_FAIL (4)
// First-frame age allowed beyond off_ms / 2: the host aggregates frames
// (h/fp, 50 ms default) and a 1 kHz device frame spans 123 ms.
#define STALE_SLACK_MS (250.0)

static void sleep_ms_quit(uint32_t ms) {
    for (uint32_t t = 0; (t < ms) && !quit_; t += 10) {
        jsdrv_thread_sleep_ms(10);
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

static void data_topic(struct stream_watch_s * self, uint32_t idx, struct jsdrv_topic_s * t) {
    jsdrv_topic_set(t, self->app->device.topic);
    jsdrv_topic_append(t, "s");
    jsdrv_topic_append(t, self->channels[idx].name);
    jsdrv_topic_append(t, "!data");
}

static int32_t subscribe_data(struct stream_watch_s * self, uint32_t idx) {
    struct jsdrv_topic_s t;
    data_topic(self, idx, &t);
    return jsdrv_subscribe(self->app->context, t.topic, JSDRV_SFLAG_PUB,
                           on_data, &self->channels[idx], JSDRV_TIMEOUT_MS_DEFAULT);
}

static void unsubscribe_data(struct stream_watch_s * self, uint32_t idx) {
    struct jsdrv_topic_s t;
    data_topic(self, idx, &t);
    jsdrv_unsubscribe(self->app->context, t.topic, on_data, &self->channels[idx],
                      JSDRV_TIMEOUT_MS_DEFAULT);
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
        if (ch->enabled && (rate < (double) min_rate)) {
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

// Publish every enabled channel, even after a failure, so cleanup stops
// every stream.
static int32_t streams_ctrl(struct stream_watch_s * self, uint32_t value) {
    int32_t rc = 0;
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        struct channel_s * ch = &self->channels[idx];
        if (!ch->enabled) {
            continue;
        }
        char subtopic[16];
        snprintf(subtopic, sizeof(subtopic), "s/%s/ctrl", ch->name);
        int32_t rc_ch = publish_u32(self->app, subtopic, value);
        rc = rc ? rc : rc_ch;
    }
    return rc;
}

// One stop/restart cycle.  A first frame is stale when its sample_id is
// older than the previous cycle's last sample_id plus the elapsed time,
// by more than half of off_ms plus STALE_SLACK_MS: it was held across
// the stop.  Returns
// true when no channel skipped or delivered a stale first frame.
// fs_next is published fs_delay_ms after the stop (0 skips it).
static bool cycle_run(struct stream_watch_s * self, uint32_t cycle, uint32_t fs,
                      uint32_t fs_next, uint32_t fs_delay_ms,
                      uint32_t on_ms, uint32_t off_ms) {
    uint64_t id_prev[CHANNEL_COUNT];
    int64_t utc_prev[CHANNEL_COUNT];
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        struct channel_s * ch = &self->channels[idx];
        id_prev[idx] = ch->first_id ? ch->sample_id_last : 0;
        utc_prev[idx] = ch->last_utc;
        ch->skips = 0;
        ch->first_id = 0;
        ch->msgs = 0;
    }
    if (streams_ctrl(self, 1U)) {
        return false;
    }
    sleep_ms_quit(on_ms);
    streams_ctrl(self, 0U);
    sleep_ms_quit(fs_delay_ms);
    if (fs_next) {
        publish_u32(self->app, "h/fs", fs_next);
    }
    sleep_ms_quit(off_ms - fs_delay_ms);  // let in-flight frames drain

    bool ok = true;
    fprintf(self->out, "{\"cycle\":%" PRIu32 ",\"fs\":%" PRIu32, cycle, fs);
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        struct channel_s * ch = &self->channels[idx];
        double age_ms = 0.0;
        if (ch->first_id && id_prev[idx] && ch->tick_rate) {
            double elapsed = (double) (ch->first_utc - utc_prev[idx]) / JSDRV_TIME_SECOND;
            double expect = (double) id_prev[idx] + elapsed * (double) ch->tick_rate;
            age_ms = (expect - (double) ch->first_id) * 1000.0 / (double) ch->tick_rate;
        }
        if (!ch->enabled) {
            continue;
        }
        bool stale = age_ms > ((off_ms / 2.0) + STALE_SLACK_MS);
        if (ch->skips || stale || (0 == ch->msgs)) {
            ok = false;
        }
        fprintf(self->out, ",\"%s\":{\"msgs\":%" PRIu64 ",\"skips\":%" PRIu64
                ",\"first_age_ms\":%.1f", ch->name, ch->msgs, ch->skips, age_ms);
        if (ch->skips) {
            fprintf(self->out, ",\"first_id\":%" PRIu64 ",\"skip_expected\":%" PRIu64
                    ",\"skip_received\":%" PRIu64, ch->first_id, ch->skip_expected,
                    ch->skip_received);
        }
        fprintf(self->out, "}");
    }
    fprintf(self->out, ",\"ok\":%s}\n", ok ? "true" : "false");
    fflush(self->out);
    return ok;
}

static int cycles_run(struct stream_watch_s * self, uint32_t cycles, uint32_t fs, uint32_t fs_alt,
                      uint32_t fs_delay_ms, uint32_t on_ms, uint32_t off_ms) {
    uint32_t failed = 0;
    uint32_t cycle = 0;
    for (; (cycle < cycles) && !quit_; ++cycle) {
        uint32_t cycle_fs = (fs_alt && (cycle & 1U)) ? fs_alt : fs;
        uint32_t next_fs = (fs_alt && !(cycle & 1U)) ? fs_alt : fs;
        if (!cycle_run(self, cycle, cycle_fs, next_fs, fs_delay_ms, on_ms, off_ms)) {
            ++failed;
        }
    }
    printf("stream_watch cycles: %" PRIu32 " failed of %" PRIu32 "\n", failed, cycle);
    return failed ? EXIT_CYCLE_FAIL : 0;
}

int on_stream_watch(struct app_s * self, int argc, char * argv[]) {
    struct stream_watch_s watch;
    const char * device_filter = "u/js320";
    const char * out_path = NULL;
    uint32_t duration_s = 15U;
    uint32_t fs = 0U;
    uint32_t min_rate = 1000U;
    uint32_t eval_s = 3U;
    uint32_t cycles = 0U;
    const char * signals = "ivp";
    uint32_t on_ms = 500U;
    uint32_t off_ms = 1000U;
    uint32_t fs_alt = 0U;
    uint32_t fs_delay_ms = 0U;

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
        } else if (0 == strcmp(argv[0], "--signals")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            signals = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--cycles")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &cycles));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--on-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &on_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--off-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &off_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--fs-delay-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &fs_delay_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--fs-alt")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &fs_alt));
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
    if ((0 == eval_s) || (eval_s > duration_s) || (fs_delay_ms > off_ms)) {
        return usage();
    }
    for (uint32_t idx = 0; idx < CHANNEL_COUNT; ++idx) {
        watch.channels[idx].enabled = (NULL != strchr(signals, watch.channels[idx].name[0]));
    }

    watch.out = stdout;
    if (out_path) {
        watch.out = fopen(out_path, "w");
        if (!watch.out) {
            printf("could not open --out %s\n", out_path);
            return 1;
        }
    }

    // The callbacks point at watch on this stack frame, so every path
    // below must unsubscribe before returning.
    int rc = 1;
    bool opened = false;
    uint32_t data_subscribed = 0;
    if (jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB,
                        on_device_add, &watch, JSDRV_TIMEOUT_MS_DEFAULT)) {
        goto exit_out;
    }
    if (jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB,
                        on_device_remove, &watch, JSDRV_TIMEOUT_MS_DEFAULT)) {
        goto exit_add;
    }
    if (app_match(self, device_filter)) {
        goto exit;
    }
    printf("stream_watch device=%s duration=%" PRIu32 "s min_rate=%" PRIu32 "\n",
           self->device.topic, duration_s, min_rate);
    // The UI uses a generous open timeout; a leaked prior session can burn
    // ~750 ms in CONNECT_REQ retries before the handshake converges.
    if (jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_DEFAULTS, 5000)) {
        goto exit;
    }
    opened = true;
    if (fs && publish_u32(self, "h/fs", fs)) {
        goto exit;
    }
    for (; data_subscribed < CHANNEL_COUNT; ++data_subscribed) {
        if (subscribe_data(&watch, data_subscribed)) {
            goto exit;
        }
    }
    if (cycles) {
        rc = cycles_run(&watch, cycles, fs, fs_alt, fs_delay_ms, on_ms, off_ms);
        goto exit;
    }
    if (streams_ctrl(&watch, 1U)) {
        goto exit;
    }

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

exit:
    // Clean close (the unclean-close scenarios kill this process instead).
    if (opened) {
        streams_ctrl(&watch, 0U);
    }
    while (data_subscribed) {
        unsubscribe_data(&watch, --data_subscribed);
    }
    if (opened) {
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    }
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, on_device_remove, &watch,
                      JSDRV_TIMEOUT_MS_DEFAULT);
exit_add:
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD, on_device_add, &watch,
                      JSDRV_TIMEOUT_MS_DEFAULT);
exit_out:
    if (out_path) {
        fclose(watch.out);
    }
    return rc;
}
