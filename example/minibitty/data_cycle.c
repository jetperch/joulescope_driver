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
 * @brief Cut a streaming device's USB data lines (or power) through a USB
 *      Insight Hub, then check that signal data returns.
 *
 * See js320/doc/plans/usb_data_disconnect.md.
 */

#include "minibitty_exe_prv.h"
#include "device_match.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv/os_thread.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define NULL_REDIRECT " >NUL"
#else
#define NULL_REDIRECT " >/dev/null"
#endif

enum phase_e {
    PHASE_ALL,   // stream, cut, reopen, check: all in this process
    PHASE_PRE,   // stream, cut, wait for the device to return, exit
    PHASE_POST,  // open, stream, check
};

enum sig_e {
    SIG_I,
    SIG_V,
    SIG_STATS,
    SIG_COUNT,
};

static const char * const SIG_TOPICS[SIG_COUNT] = {"s/i/!data", "s/v/!data", "s/stats/value"};
static const char * const SIG_NAMES[SIG_COUNT] = {"i", "v", "stats"};

struct sig_s {
    volatile uint64_t frames;
    volatile uint64_t sample_id_first;  // first sample_id since the last reset
    volatile uint64_t sample_id_next;   // expected next sample_id
    volatile uint64_t skips;            // sample_id discontinuities
};

static struct sig_s sigs_[SIG_COUNT];

// --comm-stats: ctrl-side link statistics (mb_comm_stats_v1_s).
static const char * const COMM_TOPICS[] = {
    "c/comm/sensor/tx/!stat", "c/comm/sensor/rx/!stat",
    "c/comm/usbd/tx/!stat", "c/comm/usbd/rx/!stat",
};
#define COMM_COUNT (sizeof(COMM_TOPICS) / sizeof(COMM_TOPICS[0]))
struct comm_stat_s {
    volatile uint32_t frames;
    volatile uint32_t frame_error;
    volatile uint32_t link_error;
    volatile uint32_t app_error;
};
static struct comm_stat_s comm_[COMM_COUNT];
static volatile bool target_present_ = false;

static struct {
    const char * uih;
    bool hub_stdio;      // the parent process switches the hub
    uint32_t hub_port;
    bool power;          // cut power instead of data
    uint32_t off_ms;
    int32_t count;
    bool idle;
    bool no_close;       // keep the removed device open, like a stuck host
    bool recover;
    bool comm_stats;     // report ctrl link statistics per check
    uint32_t enable_delay_ms;  // wait after open before enabling the streams
    bool power_restore;  // power cycle after a failure
    enum phase_e phase;
    uint32_t window_ms;
    uint32_t add_timeout_ms;
    uint32_t open_timeout_ms;
    const char * target;
} cfg_;

static int usage(void) {
    printf(
        "usage: minibitty data_cycle --hub-port <N> | --hub-stdio [options] <device>\n"
        "\n"
        "Stream i, v, and statistics, cut the device's USB data lines through\n"
        "a USB Insight Hub, and check that signal data returns.\n"
        "\n"
        "Options:\n"
        "  --hub-port <N>      USB Insight Hub port of the device (required).\n"
        "  --uih <path>        uih executable [env UIH, else uih].\n"
        "  --hub-stdio         Ask the parent process to switch the hub: print\n"
        "                      \"HUB <data|power> <on|off>\" and wait for an \"OK\"\n"
        "                      line on stdin.  For a parent that owns the hub,\n"
        "                      such as the joulescope_ci runner.\n"
        "  --power             Cut power instead of data (the control case).\n"
        "  --off-ms <ms>       Time with the data lines off [3000].\n"
        "  --count <N>         Cycles [1].\n"
        "  --idle              Stop the streams before the cut.\n"
        "  --phase <phase>     all: everything in this process [all].\n"
        "                      pre: stream, cut, wait for the device, exit.\n"
        "                      post: open, stream, check.\n"
        "  --window-ms <ms>    Data check window [2000].\n"
        "  --no-close          Do not close the removed device.\n"
        "  --recover           On failure, try ctrl 0 then 1, then close and reopen.\n"
        "  --comm-stats        Also report the ctrl sensor and USB link statistics.\n"
        "  --enable-delay-ms <ms>  Wait after open before enabling streams [0].\n"
        "  --no-power-restore  Do not power cycle after a failure.\n"
    );
    return 1;
}

static void on_comm_stat(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct comm_stat_s * c = (struct comm_stat_s *) user_data;
    // An 8-byte stdmsg header, then mb_comm_stats_v1_s: two u64 counters,
    // then frames, bytes, frame_error, link_error, app_error.
    if ((value->type != JSDRV_UNION_STDMSG) || (value->size < (8 + 16 + 20))) {
        return;
    }
    uint32_t u32[5];
    memcpy(u32, value->value.bin + 8 + 16, sizeof(u32));
    c->frames += u32[0];
    c->frame_error += u32[2];
    c->link_error += u32[3];
    c->app_error += u32[4];
}

static void sigs_reset(void) {
    for (int k = 0; k < SIG_COUNT; ++k) {
        sigs_[k].frames = 0;
        sigs_[k].sample_id_first = 0;
        sigs_[k].sample_id_next = 0;
        sigs_[k].skips = 0;
    }
}

static void on_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct sig_s * s = (struct sig_s *) user_data;
    uint64_t sample_id = 0;
    uint64_t sample_id_next = 0;
    if ((value->type == JSDRV_UNION_BIN) && (s != &sigs_[SIG_STATS])
            && (value->size >= sizeof(struct jsdrv_stream_signal_s) - JSDRV_STREAM_DATA_SIZE)) {
        const struct jsdrv_stream_signal_s * signal = (const struct jsdrv_stream_signal_s *) value->value.bin;
        sample_id = signal->sample_id;
        sample_id_next = sample_id + (uint64_t) signal->element_count * signal->decimate_factor;
    } else if ((value->type == JSDRV_UNION_BIN) && (value->size >= sizeof(struct jsdrv_statistics_s))) {
        const struct jsdrv_statistics_s * stats = (const struct jsdrv_statistics_s *) value->value.bin;
        sample_id = stats->block_sample_id;
    }
    if (0 == s->frames) {
        s->sample_id_first = sample_id;
    } else if (sample_id_next && (sample_id != s->sample_id_next)) {
        ++s->skips;
    }
    s->sample_id_next = sample_id_next;
    ++s->frames;
}

static const char * device_prefix(const struct jsdrv_union_s * value) {
    if (value->type == JSDRV_UNION_STR) {
        return value->value.str;
    } else if ((value->type == JSDRV_UNION_BIN) && value->value.bin) {
        return (const char *) value->value.bin;
    }
    return NULL;
}

static void on_device_add(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const char * prefix = device_prefix(value);
    if (prefix && device_match(prefix, cfg_.target)) {
        target_present_ = true;
    }
}

static void on_device_remove(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const char * prefix = device_prefix(value);
    if (prefix && device_match(prefix, cfg_.target)) {
        target_present_ = false;
    }
}

static void sleep_ms(uint32_t duration_ms) {
    uint32_t t_end = jsdrv_time_ms_u32() + duration_ms;
    while (!quit_ && ((int32_t) (t_end - jsdrv_time_ms_u32()) > 0)) {
        jsdrv_thread_sleep_ms(10);
    }
}

// Return the elapsed time in ms, or -1 on timeout.
static int32_t wait_present(bool desired, uint32_t timeout_ms) {
    uint32_t t_start = jsdrv_time_ms_u32();
    while (!quit_) {
        uint32_t dt = jsdrv_time_ms_u32() - t_start;
        if (target_present_ == desired) {
            return (int32_t) dt;
        }
        if (dt >= timeout_ms) {
            break;
        }
        jsdrv_thread_sleep_ms(5);
    }
    return -1;
}

static int32_t publish_u32(struct app_s * self, const char * subtopic, uint32_t value, uint32_t timeout_ms) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, subtopic);
    int32_t rc = jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(value), timeout_ms);
    if (rc) {
        printf("  publish %s=%u failed: %d\n", topic.topic, value, (int) rc);
    }
    return rc;
}

static void subscribe_all(struct app_s * self, bool subscribe) {
    struct jsdrv_topic_s topic;
    for (int k = 0; k < SIG_COUNT; ++k) {
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, SIG_TOPICS[k]);
        if (subscribe) {
            jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_data, &sigs_[k], 0);
        } else {
            jsdrv_unsubscribe(self->context, topic.topic, on_data, &sigs_[k], 0);
        }
    }
    for (uint32_t k = 0; cfg_.comm_stats && (k < COMM_COUNT); ++k) {
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, COMM_TOPICS[k]);
        if (subscribe) {
            jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_comm_stat, &comm_[k], 0);
        } else {
            jsdrv_unsubscribe(self->context, topic.topic, on_comm_stat, &comm_[k], 0);
        }
    }
}

static void streams_set(struct app_s * self, uint32_t value, uint32_t timeout_ms) {
    publish_u32(self, "s/i/ctrl", value, timeout_ms);
    publish_u32(self, "s/v/ctrl", value, timeout_ms);
    publish_u32(self, "s/stats/ctrl", value, timeout_ms);
}

static int32_t device_open(struct app_s * self) {
    int32_t rc = app_match_ex(self, cfg_.target, APP_MATCH_EXPLICIT);
    if (rc) {
        return rc;
    }
    rc = jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, cfg_.open_timeout_ms);
    if (rc) {
        printf("  open %s failed: %d\n", self->device.topic, (int) rc);
        return rc;
    }
    memset((void *) comm_, 0, sizeof(comm_));  // from open: include the open burst
    subscribe_all(self, true);
    return 0;
}

// Close like the UI: stop the streams without waiting, then close.
static void device_close(struct app_s * self) {
    subscribe_all(self, false);
    streams_set(self, 0, 0);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
}

// Enable the streams, settle, and count frames over the window.
// Return true when i, v, and statistics all arrive.
static bool stream_check(struct app_s * self, const char * label, bool enable) {
    if (enable) {
        sleep_ms(cfg_.enable_delay_ms);
        streams_set(self, 1, JSDRV_TIMEOUT_MS_DEFAULT);
    }
    sleep_ms(1000);
    sigs_reset();
    sleep_ms(cfg_.window_ms);
    bool ok = true;
    printf("  %-8s", label);
    for (int k = 0; k < SIG_COUNT; ++k) {
        printf(" %s=%" PRIu64, SIG_NAMES[k], sigs_[k].frames);
        if (k != SIG_STATS) {
            printf(" (id0=%" PRIu64 " skips=%" PRIu64 ")", sigs_[k].sample_id_first, sigs_[k].skips);
        }
        ok &= (sigs_[k].frames > 0);
    }
    printf(" -> %s\n", ok ? "OK" : "NO DATA");
    for (uint32_t k = 0; cfg_.comm_stats && (k < COMM_COUNT); ++k) {
        struct comm_stat_s * c = &comm_[k];
        printf("    %-24s frames=%u frame_error=%u link_error=%u app_error=%u\n", COMM_TOPICS[k],
               c->frames, c->frame_error, c->link_error, c->app_error);
    }
    return ok;
}

static int32_t hub_set(const char * what, bool on) {
    char cmd[1024];
    if (cfg_.hub_stdio) {
        printf("HUB %s %s\n", what, on ? "on" : "off");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin) || strncmp(cmd, "OK", 2)) {
            printf("  hub request failed: %s\n", feof(stdin) ? "stdin closed" : cmd);
            return 1;
        }
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "%s set %u --%s %s" NULL_REDIRECT,
             cfg_.uih, cfg_.hub_port, what, on ? "on" : "off");
    int rc = system(cmd);
    if (rc) {
        printf("  hub command failed (%d): %s\n", rc, cmd);
    }
    return rc;
}

// Cut data or power, then wait for the device to leave and return.
static int32_t hub_cycle(bool power, uint32_t off_ms) {
    const char * what = power ? "power" : "data";
    uint32_t t0 = jsdrv_time_ms_u32();
    ROE(hub_set(what, false));
    int32_t t_remove = wait_present(false, off_ms);
    if (t_remove >= 0) {
        t_remove = (int32_t) (jsdrv_time_ms_u32() - t0);  // from the off command start
    }
    uint32_t t_off = jsdrv_time_ms_u32() - t0;
    if (t_off < off_ms) {
        sleep_ms(off_ms - t_off);
    }
    t_off = jsdrv_time_ms_u32() - t0;
    ROE(hub_set(what, true));
    if (t_remove < 0) {
        t_remove = wait_present(false, 1000);
        if (t_remove >= 0) {
            t_remove += (int32_t) t_off;
        }
    }
    int32_t t_add = wait_present(true, cfg_.add_timeout_ms);
    printf("  %s off %u ms: remove %d ms, add %d ms after on\n", what, t_off, (int) t_remove, (int) t_add);
    if (t_add < 0) {
        printf("  device did not return\n");
        return 1;
    }
    sleep_ms(250);  // let the driver finish its add processing
    return 0;
}

static void power_restore(struct app_s * self) {
    printf("  power cycle to restore the device\n");
    if (hub_cycle(true, 1000)) {
        return;
    }
    if (0 == device_open(self)) {
        if (!stream_check(self, "restored", true)) {
            printf("  WARNING: power cycle did not restore streaming\n");
        }
        device_close(self);
    }
}

// Direct recovery attempts after a failed check, with the device open.
static void recover(struct app_s * self) {
    publish_u32(self, "s/i/ctrl", 0, JSDRV_TIMEOUT_MS_DEFAULT);
    publish_u32(self, "s/v/ctrl", 0, JSDRV_TIMEOUT_MS_DEFAULT);
    sleep_ms(250);
    if (stream_check(self, "ctrl01", true)) {
        printf("  RECOVERED by ctrl 0 then 1\n");
        return;
    }
    device_close(self);
    sleep_ms(250);
    if (0 == device_open(self)) {
        if (stream_check(self, "reopen", true)) {
            printf("  RECOVERED by close and reopen\n");
        }
    }
}

// One cycle.  Return 0 on pass, 1 on failure, negative on a setup error.
static int32_t cycle(struct app_s * self, uint32_t iteration) {
    printf("\n--- cycle %u ---\n", iteration);
    bool is_open = false;
    if (cfg_.phase != PHASE_POST) {
        if (device_open(self)) {
            return -1;
        }
        is_open = true;
        if (!stream_check(self, "before", true)) {
            printf("  device not streaming before the cut\n");
            device_close(self);
            return -1;
        }
        if (cfg_.idle) {
            streams_set(self, 0, JSDRV_TIMEOUT_MS_DEFAULT);
            sleep_ms(250);
        }
        if (hub_cycle(cfg_.power, cfg_.off_ms)) {
            return -1;
        }
        if (!cfg_.no_close) {
            device_close(self);
            is_open = false;
        }
        if (cfg_.phase == PHASE_PRE) {
            if (is_open) {
                device_close(self);
            }
            return 0;
        }
    }
    if (is_open) {
        subscribe_all(self, false);
    }
    if (device_open(self)) {
        return 1;
    }
    bool ok = stream_check(self, "after", true);
    if (!ok && cfg_.recover) {
        recover(self);
    }
    device_close(self);
    if (!ok && cfg_.power_restore) {
        power_restore(self);
    }
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int32_t arg_u32(char * argv[], uint32_t * value) {
    if (jsdrv_cstr_to_u32(argv[0], value)) {
        printf("Invalid value: %s\n", argv[0]);
        return 1;
    }
    return 0;
}

int on_data_cycle(struct app_s * self, int argc, char * argv[]) {
    memset(&cfg_, 0, sizeof(cfg_));
    cfg_.uih = getenv("UIH");
    if (!cfg_.uih || !cfg_.uih[0]) {
        cfg_.uih = "uih";
    }
    cfg_.off_ms = 3000;
    cfg_.count = 1;
    cfg_.power_restore = true;
    cfg_.phase = PHASE_ALL;
    cfg_.window_ms = 2000;
    cfg_.add_timeout_ms = 10000;
    cfg_.open_timeout_ms = 10000;

    while (argc) {
        if (argv[0][0] != '-') {
            cfg_.target = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--hub-port")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &cfg_.hub_port));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--uih")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            cfg_.uih = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--hub-stdio")) {
            cfg_.hub_stdio = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--power")) {
            cfg_.power = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--off-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &cfg_.off_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--count")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_i32(argv[0], &cfg_.count)) {
                printf("Invalid value: %s\n", argv[0]);
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--idle")) {
            cfg_.idle = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--phase")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (0 == strcmp(argv[0], "all")) {
                cfg_.phase = PHASE_ALL;
            } else if (0 == strcmp(argv[0], "pre")) {
                cfg_.phase = PHASE_PRE;
            } else if (0 == strcmp(argv[0], "post")) {
                cfg_.phase = PHASE_POST;
            } else {
                printf("Invalid phase: %s\n", argv[0]);
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--window-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &cfg_.window_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--no-close")) {
            cfg_.no_close = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--enable-delay-ms")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(arg_u32(argv, &cfg_.enable_delay_ms));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--comm-stats")) {
            cfg_.comm_stats = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--recover")) {
            cfg_.recover = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--no-power-restore")) {
            cfg_.power_restore = false;
            ARG_CONSUME();
        } else {
            printf("Unknown argument: %s\n", argv[0]);
            return usage();
        }
    }
    bool has_hub = cfg_.hub_stdio || (cfg_.hub_port != 0);
    if (!cfg_.target || (cfg_.phase != PHASE_POST && !has_hub)) {
        printf("device and --hub-port (or --hub-stdio) are required\n");
        return usage();
    }
    if (cfg_.phase == PHASE_POST) {
        cfg_.power_restore = cfg_.power_restore && has_hub;
    }

    ROE(app_match_ex(self, cfg_.target, APP_MATCH_EXPLICIT));
    target_present_ = true;
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB, on_device_add, self, 0));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB, on_device_remove, self, 0));

    uint32_t pass = 0;
    uint32_t fail = 0;
    uint32_t error = 0;
    for (uint32_t iteration = 1; !quit_ && ((cfg_.count < 0) || (iteration <= (uint32_t) cfg_.count)); ++iteration) {
        int32_t rc = cycle(self, iteration);
        if (rc == 0) {
            ++pass;
        } else if (rc > 0) {
            ++fail;
        } else {
            ++error;
            break;  // the device needs attention
        }
    }

    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD, on_device_add, self, 0);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_REMOVE, on_device_remove, self, 0);
    printf("\n=== RESULT pass=%u fail=%u error=%u ===\n", pass, fail, error);
    return (fail || error) ? 1 : 0;
}
