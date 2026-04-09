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
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/thread.h"
#include "mb/stdmsg.h"
#include "mb/timesync.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define HISTORY_SIZE (16U)
#define DEFAULT_DURATION_MS (90000U)
#define CONVERGE_TIME_MS (60000U)
#define DEFAULT_MAX_RATE_PPM (200.0)
#define DEFAULT_MAX_SKEW_US (5000.0)


struct ts_history_s {
    double rate_hz;
    double host_skew_us;
};

struct ts_state_s {
    char source_filter;          // 'c', 's', or 0 for both
    bool require_converge;
    double max_rate_ppm;
    double max_skew_us;

    uint32_t map_count;          // total maps received
    uint32_t map_count_c;
    uint32_t map_count_s;
    int64_t  start_utc;          // host UTC when subcommand started

    // Steady-state history (most recent HISTORY_SIZE samples).
    struct ts_history_s history[HISTORY_SIZE];
    uint32_t history_head;
    uint32_t history_count;

    // Last seen update_counter per source.
    uint32_t last_update_counter_c;
    uint32_t last_update_counter_s;

    // Last error tracked for pass/fail at end.
    double last_skew_us;
    double last_rate_hz;
    bool   converged;
};

static struct ts_state_s g_state;


static void format_iso_utc(int64_t utc_q30, char * out, size_t out_size) {
    // Sanity-clamp to a representable range so strftime doesn't assert
    // (year 1970-2099 covers any plausible synced time and any garbage
    // from a misformed map publishes as "INVALID" instead of crashing).
    int64_t time_s = (int64_t) (utc_q30 >> JSDRV_TIME_Q);
    int64_t time_us = (int64_t) JSDRV_TIME_TO_MICROSECONDS(utc_q30 & JSDRV_FRACT_MASK);
    int64_t time_unix = time_s + JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS;
    if (time_unix < 0 || time_unix > 4102444800LL /* 2100-01-01 */) {
        snprintf(out, out_size, "INVALID(0x%016" PRIx64 ")", (uint64_t) utc_q30);
        return;
    }
    time_t t = (time_t) time_unix;
    struct tm tm_utc;
#if _WIN32
    if (_gmtime64_s(&tm_utc, &t) != 0) {
        snprintf(out, out_size, "INVALID(0x%016" PRIx64 ")", (uint64_t) utc_q30);
        return;
    }
#else
    if (gmtime_r(&t, &tm_utc) == NULL) {
        snprintf(out, out_size, "INVALID(0x%016" PRIx64 ")", (uint64_t) utc_q30);
        return;
    }
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    snprintf(out, out_size, "%s.%06" PRId64 "Z", buf, time_us);
}

static void history_push(struct ts_state_s * s, double rate_hz, double skew_us) {
    s->history[s->history_head].rate_hz = rate_hz;
    s->history[s->history_head].host_skew_us = skew_us;
    s->history_head = (s->history_head + 1U) % HISTORY_SIZE;
    if (s->history_count < HISTORY_SIZE) {
        ++s->history_count;
    }
}

static void on_map(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct ts_state_s * s = (struct ts_state_s *) user_data;

    if (value->type != JSDRV_UNION_STDMSG) {
        return;
    }
    if (value->size < (sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_timesync_map_v1_s))) {
        return;
    }
    const struct mb_stdmsg_header_s * hdr =
        (const struct mb_stdmsg_header_s *) value->value.bin;
    if (hdr->type != MB_STDMSG_TIMESYNC_MAP) {
        return;
    }
    const struct mb_timesync_map_v1_s * body =
        (const struct mb_timesync_map_v1_s *) (hdr + 1);

    // Extract source character: skip device prefix to find "c/ts/!map" or "s/ts/!map".
    // The topic is e.g. "u/js320/000/c/ts/!map" -- find "/c/ts" or "/s/ts".
    char source = '?';
    const char * p = strstr(topic, "/ts/!map");
    if (p && p > topic) {
        source = *(p - 1);  // character just before "/ts/!map"
    }

    if (s->source_filter && source != s->source_filter) {
        return;
    }

    int64_t host_utc_now = jsdrv_time_utc();
    double t_since_start_s = JSDRV_TIME_TO_F64(host_utc_now - s->start_utc);

    // Decode map fields.
    double counter_rate_hz = (double) body->counter_rate / (double)(1ULL << 32);
    double counter_ppb = (double) body->counter_ppb;

    // RTT from sync exchange.
    double rtt_us = 0.0;
    if (counter_rate_hz > 0.0) {
        uint64_t rtt_ticks = body->sync_count_end - body->sync_count_start;
        rtt_us = ((double) rtt_ticks / counter_rate_hz) * 1e6;
    }

    // Host skew: difference between host's perceived midpoint UTC and the
    // device's interpolated UTC at count_mid using this map.
    double host_skew_us = 0.0;
    if (counter_rate_hz > 0.0) {
        uint64_t count_mid = (body->sync_count_start + body->sync_count_end) / 2U;
        int64_t  utc_mid_q30 = (body->sync_utc_recv + body->sync_utc_send) / 2;
        // utc_at(count_mid) using this map (allow signed dc).
        int64_t dc = (int64_t) (count_mid - body->counter);
        double dt_s = (double) dc / counter_rate_hz;
        double utc_at_mid_s = JSDRV_TIME_TO_F64(body->utc) + dt_s;
        double utc_host_mid_s = JSDRV_TIME_TO_F64(utc_mid_q30);
        host_skew_us = (utc_host_mid_s - utc_at_mid_s) * 1e6;
    }

    // Update bookkeeping.
    ++s->map_count;
    if (source == 'c') {
        ++s->map_count_c;
        s->last_update_counter_c = body->update_counter;
    } else if (source == 's') {
        ++s->map_count_s;
        s->last_update_counter_s = body->update_counter;
    }
    history_push(s, counter_rate_hz, host_skew_us);
    s->last_rate_hz = counter_rate_hz;
    s->last_skew_us = host_skew_us;

    // Print line.
    char iso[40];
    format_iso_utc(body->utc, iso, sizeof(iso));
    printf("[%7.2fs] %c #%-5u utc=%s rate=%.6f Hz ppb=%.0f rtt=%6.1f us skew=%+8.1f us\n",
           t_since_start_s, source, (unsigned) body->update_counter, iso,
           counter_rate_hz, counter_ppb, rtt_us, host_skew_us);
    fflush(stdout);
}

static void compute_stats(const struct ts_state_s * s,
                          double * mean_rate, double * rate_stddev_hz,
                          double * mean_abs_skew, double * peak_skew) {
    *mean_rate = 0.0;
    *rate_stddev_hz = 0.0;
    *mean_abs_skew = 0.0;
    *peak_skew = 0.0;
    if (s->history_count == 0) {
        return;
    }
    double rate_sum = 0.0;
    double abs_skew_sum = 0.0;
    double max_abs_skew = 0.0;
    for (uint32_t i = 0; i < s->history_count; ++i) {
        rate_sum += s->history[i].rate_hz;
        double abs_skew = s->history[i].host_skew_us;
        if (abs_skew < 0.0) abs_skew = -abs_skew;
        abs_skew_sum += abs_skew;
        if (abs_skew > max_abs_skew) {
            max_abs_skew = abs_skew;
        }
    }
    *mean_rate = rate_sum / s->history_count;
    *mean_abs_skew = abs_skew_sum / s->history_count;
    *peak_skew = max_abs_skew;

    if (s->history_count >= 2) {
        double sq_sum = 0.0;
        for (uint32_t i = 0; i < s->history_count; ++i) {
            double d = s->history[i].rate_hz - *mean_rate;
            sq_sum += d * d;
        }
        *rate_stddev_hz = sqrt(sq_sum / (s->history_count - 1));
    }
}

static int print_summary(struct ts_state_s * s) {
    double mean_rate, rate_stddev, mean_skew, peak_skew;
    compute_stats(s, &mean_rate, &rate_stddev, &mean_skew, &peak_skew);
    double rate_ppm = (mean_rate > 0.0) ? (rate_stddev / mean_rate * 1e6) : 0.0;

    printf("\n=== Summary ===\n");
    printf("  maps received   : %u (c=%u, s=%u)\n",
           (unsigned) s->map_count, (unsigned) s->map_count_c, (unsigned) s->map_count_s);
    printf("  history samples : %u\n", (unsigned) s->history_count);
    printf("  mean rate       : %.6f Hz\n", mean_rate);
    printf("  rate stddev     : %.6f Hz (%.3f ppm of mean)\n", rate_stddev, rate_ppm);
    printf("  mean abs skew   : %.2f us\n", mean_skew);
    printf("  peak abs skew   : %.2f us\n", peak_skew);

    int rc = 0;
    if (s->require_converge) {
        if (s->map_count == 0) {
            printf("  FAIL: no maps received\n");
            rc = 1;
        } else if (s->map_count < 2) {
            printf("  FAIL: only %u map received (need >= 2 to converge)\n",
                   (unsigned) s->map_count);
            rc = 1;
        }
        if (peak_skew > s->max_skew_us) {
            printf("  FAIL: peak skew %.2f us > %.2f us limit\n",
                   peak_skew, s->max_skew_us);
            rc = 1;
        }
        if (rate_ppm > s->max_rate_ppm) {
            printf("  FAIL: rate stddev %.3f ppm > %.3f ppm limit\n",
                   rate_ppm, s->max_rate_ppm);
            rc = 1;
        }
        if (rc == 0) {
            printf("  PASS\n");
            s->converged = true;
        }
    }
    fflush(stdout);
    return rc;
}

static int usage(void) {
    printf(
        "usage: minibitty timesync [options] <device_filter>\n"
        "\n"
        "Subscribe to timesync map topics on the device, print updates and\n"
        "metrics, and optionally enforce convergence/drift thresholds.\n"
        "\n"
        "Options:\n"
        "  --duration <ms>      Run for this duration then exit (default: %u).\n"
        "  --source <c|s>       Filter on source instance (default: both).\n"
        "  --require-converge   Exit non-zero if not converged at end.\n"
        "  --max-skew-us <us>   Max allowed peak host skew (default: %.0f).\n"
        "  --max-rate-ppm <ppm> Max allowed rate stddev as ppm of mean\n"
        "                       (default: %.0f).\n"
        "\nPress CTRL-C to exit early.\n",
        DEFAULT_DURATION_MS, DEFAULT_MAX_SKEW_US, DEFAULT_MAX_RATE_PPM);
    return 1;
}

int on_timesync(struct app_s * self, int argc, char * argv[]) {
    // Unbuffered stdout so progress is visible when output is redirected
    // (e.g., piped to a log file or run as a background job).
    setvbuf(stdout, NULL, _IONBF, 0);

    struct ts_state_s * s = &g_state;
    memset(s, 0, sizeof(*s));
    s->max_rate_ppm = DEFAULT_MAX_RATE_PPM;
    s->max_skew_us = DEFAULT_MAX_SKEW_US;
    self->duration_ms = DEFAULT_DURATION_MS;

    char * device_filter = NULL;
    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            self->duration_ms = (uint32_t) strtoul(argv[0], NULL, 10);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--source")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (argv[0][0] == 'c' || argv[0][0] == 's') {
                s->source_filter = argv[0][0];
            } else {
                printf("--source must be 'c' or 's'\n");
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--require-converge")) {
            s->require_converge = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--max-skew-us")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            s->max_skew_us = strtod(argv[0], NULL);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--max-rate-ppm")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            s->max_rate_ppm = strtod(argv[0], NULL);
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
    ROE(jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));

    s->start_utc = jsdrv_time_utc();

    struct jsdrv_topic_s topic_c;
    struct jsdrv_topic_s topic_s;
    jsdrv_topic_set(&topic_c, self->device.topic);
    jsdrv_topic_append(&topic_c, "c/ts/!map");
    jsdrv_topic_set(&topic_s, self->device.topic);
    jsdrv_topic_append(&topic_s, "s/ts/!map");

    if (s->source_filter != 's') {
        printf("# Subscribing to %s\n", topic_c.topic);
        ROE(jsdrv_subscribe(self->context, topic_c.topic, JSDRV_SFLAG_PUB,
                             on_map, s, JSDRV_TIMEOUT_MS_DEFAULT));
    }
    if (s->source_filter != 'c') {
        printf("# Subscribing to %s\n", topic_s.topic);
        ROE(jsdrv_subscribe(self->context, topic_s.topic, JSDRV_SFLAG_PUB,
                             on_map, s, JSDRV_TIMEOUT_MS_DEFAULT));
    }
    printf("# Duration %u ms; press CTRL-C to exit early.\n", self->duration_ms);
    fflush(stdout);

    int64_t end_utc = s->start_utc
        + ((int64_t) self->duration_ms) * JSDRV_TIME_MILLISECOND;
    while (!quit_) {
        jsdrv_thread_sleep_ms(50);
        if (jsdrv_time_utc() >= end_utc) {
            break;
        }
    }

    if (s->source_filter != 's') {
        jsdrv_unsubscribe(self->context, topic_c.topic, on_map, s,
                          JSDRV_TIMEOUT_MS_DEFAULT);
    }
    if (s->source_filter != 'c') {
        jsdrv_unsubscribe(self->context, topic_s.topic, on_map, s,
                          JSDRV_TIMEOUT_MS_DEFAULT);
    }
    int rc = print_summary(s);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    return rc;
}
