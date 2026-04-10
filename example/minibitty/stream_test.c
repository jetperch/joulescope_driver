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

// Hardware-in-the-loop regression test for the JS320 streaming pipeline.
//
// Walks through a series of (h/fp, fs, signal-enable) combinations,
// subscribes to the expected !data topics, and verifies for each test:
//   * every expected signal received messages
//   * the average per-message element count is within tolerance of
//     sample_rate / publish_rate
//   * the aggregate sample throughput is within tolerance of sample_rate
//   * for grouped signals, the sample_id ranges overlap (alignment)
//   * smart-power: when all three of i/v/p are enabled at fs=1MHz, the
//     host should still emit s/p/!data messages (computed locally)

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/topic.h"
#include "jsdrv/os_thread.h"
#include "jsdrv_prv/platform.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_SIGS 8
#define COUNT_TOL_PCT 25      // ±25% on per-message element count
#define RATE_TOL_PCT  15      // ±15% on aggregate sample rate
#define MIN_MESSAGE_COUNT 5   // need at least this many messages to verify

struct sig_stats_s {
    const char * subtopic;       // "s/i/!data"
    const char * ctrl_topic;     // "s/i/ctrl"
    uint32_t expected_rate;      // expected output sample rate, Hz
    bool     enable;             // enable this signal during the test
    bool     expect_data;        // expect to receive messages for it

    // Filled in by callback (runs on pubsub thread).
    volatile uint32_t message_count;
    volatile uint64_t total_elements;
    volatile uint64_t first_sample_id;
    volatile uint64_t last_sample_id_end;  // sample_id + count*decimate
    volatile uint32_t min_element_count;
    volatile uint32_t max_element_count;
    volatile uint32_t decimate_factor;
    volatile uint32_t sample_rate_native;
    volatile uint8_t  field_id;

    // Sample-id continuity tracking.  Each new message's sample_id MUST
    // equal the previous message's (sample_id + element_count *
    // decimate_factor); otherwise the buffer/UI inserts NaN-fill gaps.
    volatile uint64_t expected_next_sample_id;
    volatile uint32_t sample_id_gaps;
    volatile int64_t  worst_gap_delta;  // (received - expected) of worst gap

    // Time map values, captured from the first received message.  The
    // buffer divides counter_rate by decimate_factor to get the output-
    // rate ticks/sec.  For JS320, counter_rate must be 16e6 (the native
    // sample_id rate); otherwise the UI's x-axis will be wrong.
    volatile double tmap_counter_rate;
    volatile int64_t tmap_offset_time;

    // Sample-data sanity: if the host's port_append builds a 50000+
    // sample message but only the first ~123 samples (= one device
    // frame worth) are valid floats and the rest are NaN/zero/garbage,
    // the UI would render exactly the symptom of "burst then gap".
    // Count NaN samples in incoming float32 messages to catch this.
    volatile uint64_t f32_nan_count;
    volatile uint64_t f32_total_count;
};

struct testcase_s {
    const char * name;
    uint32_t publish_rate;
    uint32_t fs;
    uint32_t signal_dwn_n;     // s/dwnN/N: 0=passthrough, 2..1000=decimate
    uint32_t gpi_dwn_mode;     // s/gpi/+/dwnN/mode: 0=off, 1=toggle, 2=first, 3=majority
    uint32_t gpi_dwn_n;        // s/gpi/+/dwnN/N
    bool     expect_host_compute_power;  // p is computed on host (informational)
    struct sig_stats_s sigs[MAX_SIGS];
    uint32_t duration_ms;
};


// --- Test cases ---
//
// Each test enables a different combination of signals at a given
// publish_rate and fs, then verifies the resulting stream rates and
// alignment.

static struct testcase_s TESTS[] = {
    {
        .name = "i only @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        .name = "v only @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/v/!data", "s/v/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        .name = "p only @ fp=20 (device computes)",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/p/!data", "s/p/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        .name = "i+v @ fp=20 (no host compute)",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 1000000, true, true},
            {"s/v/!data", "s/v/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        .name = "i+v+p @ fp=20 (host compute power)",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .expect_host_compute_power = true,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 1000000, true, true},
            {"s/v/!data", "s/v/ctrl", 1000000, true, true},
            {"s/p/!data", "s/p/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        .name = "i+v+p @ fp=100 (host compute power)",
        .publish_rate = 100,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .expect_host_compute_power = true,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 1000000, true, true},
            {"s/v/!data", "s/v/ctrl", 1000000, true, true},
            {"s/p/!data", "s/p/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        // Default GPI decimation: gpi/+/dwnN/mode=2 (first), N=16, output 1 MHz.
        .name = "GPI 0..3 + T @ fp=20 (default decimation)",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/gpi/0/!data", "s/gpi/0/ctrl", 1000000, true, true},
            {"s/gpi/1/!data", "s/gpi/1/ctrl", 1000000, true, true},
            {"s/gpi/2/!data", "s/gpi/2/ctrl", 1000000, true, true},
            {"s/gpi/3/!data", "s/gpi/3/ctrl", 1000000, true, true},
            {"s/gpi/7/!data", "s/gpi/7/ctrl", 1000000, true, true},
            {NULL}
        },
    },
    {
        // GPI passthrough: mode=0 -> output @ 16 MHz native rate.
        .name = "GPI mode=0 (passthrough) @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 0,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/gpi/0/!data", "s/gpi/0/ctrl", 16000000, true, true},
            {"s/gpi/7/!data", "s/gpi/7/ctrl", 16000000, true, true},
            {NULL}
        },
    },
    {
        // GPI mode=2 first, N=8 -> output @ 16 MHz / 8 = 2 MHz.
        .name = "GPI mode=first N=8 (2 MHz output) @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 8,
        .duration_ms = 2000,
        .sigs = {
            {"s/gpi/0/!data", "s/gpi/0/ctrl", 2000000, true, true},
            {"s/gpi/7/!data", "s/gpi/7/ctrl", 2000000, true, true},
            {NULL}
        },
    },
    {
        // i+v at half rate via signal dwnN/N=2 -> 1 MHz / 2 = 500 kHz.
        // Uses the gateware decimate shift path (no division).
        .name = "i+v dwnN=2 (500 kHz) @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .signal_dwn_n = 2,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 500000, true, true},
            {"s/v/!data", "s/v/ctrl", 500000, true, true},
            {NULL}
        },
    },
    {
        // i+v at 1/10 rate via signal dwnN/N=10 -> 1 MHz / 10 = 100 kHz.
        // Uses the gateware decimate sequential divider path.
        .name = "i+v dwnN=10 (100 kHz) @ fp=20",
        .publish_rate = 20,
        .fs = 1000000,
        .signal_dwn_n = 10,
        .gpi_dwn_mode = 2,
        .gpi_dwn_n = 16,
        .duration_ms = 2000,
        .sigs = {
            {"s/i/!data", "s/i/ctrl", 100000, true, true},
            {"s/v/!data", "s/v/ctrl", 100000, true, true},
            {NULL}
        },
    },
};

#define TESTS_COUNT (sizeof(TESTS) / sizeof(TESTS[0]))


// --- Subscription callback ---

static void on_data(void * user_data, const char * topic,
                    const struct jsdrv_union_s * value) {
    (void) topic;
    struct sig_stats_s * s = (struct sig_stats_s *) user_data;
    if (value->app != JSDRV_PAYLOAD_TYPE_STREAM) {
        return;
    }
    if (value->size < sizeof(struct jsdrv_stream_signal_s) - JSDRV_STREAM_DATA_SIZE) {
        return;
    }
    const struct jsdrv_stream_signal_s * sig =
        (const struct jsdrv_stream_signal_s *) value->value.bin;

    if (s->message_count == 0) {
        s->first_sample_id = sig->sample_id;
        s->min_element_count = sig->element_count;
        s->max_element_count = sig->element_count;
        s->field_id = sig->field_id;
        s->sample_rate_native = sig->sample_rate;
        s->decimate_factor = sig->decimate_factor;
        s->tmap_counter_rate = sig->time_map.counter_rate;
        s->tmap_offset_time = sig->time_map.offset_time;
    } else if (sig->sample_id != s->expected_next_sample_id) {
        // Sample-id discontinuity between adjacent messages.  This is
        // exactly what causes the Joulescope UI to render NaN-fill gaps.
        int64_t delta = (int64_t) sig->sample_id
                        - (int64_t) s->expected_next_sample_id;
        s->sample_id_gaps++;
        if ((s->sample_id_gaps == 1)
                || (delta < 0 ? -delta : delta)
                   > (s->worst_gap_delta < 0 ? -s->worst_gap_delta : s->worst_gap_delta)) {
            s->worst_gap_delta = delta;
        }
    }
    uint64_t end = sig->sample_id +
        (uint64_t) sig->element_count *
        (uint64_t) (sig->decimate_factor ? sig->decimate_factor : 1U);
    s->last_sample_id_end = end;
    s->expected_next_sample_id = end;
    s->total_elements += sig->element_count;
    s->message_count++;

    // Scan float32 sample data for NaN to catch the case where the host
    // declares element_count = 50000 but only the first ~123 samples
    // are valid (e.g. a memcpy bug, or accumulator pointer aliasing).
    if ((sig->element_type == JSDRV_DATA_TYPE_FLOAT)
            && (sig->element_size_bits == 32)
            && (sig->element_count > 0U)) {
        const float * fdata = (const float *) sig->data;
        for (uint32_t i = 0; i < sig->element_count; ++i) {
            if (isnan(fdata[i])) {
                s->f32_nan_count++;
            }
        }
        s->f32_total_count += sig->element_count;
    }
    if (sig->element_count < s->min_element_count) {
        s->min_element_count = sig->element_count;
    }
    if (sig->element_count > s->max_element_count) {
        s->max_element_count = sig->element_count;
    }
}


// --- Helpers ---

#define PUBLISH_U32(topic_, value_)                                                  \
    do {                                                                             \
        struct jsdrv_topic_s _t;                                                     \
        jsdrv_topic_set(&_t, self->device.topic);                                    \
        jsdrv_topic_append(&_t, topic_);                                             \
        int32_t _rc = jsdrv_publish(self->context, _t.topic,                         \
                                    &jsdrv_union_u32(value_),                        \
                                    JSDRV_TIMEOUT_MS_DEFAULT);                       \
        if (_rc) {                                                                   \
            printf("    publish %s=%u failed: %d\n", topic_, (unsigned)(value_), _rc); \
        }                                                                            \
    } while (0)

// All the s/X/ctrl topics we may want to disable between tests, so a
// previous test never bleeds into the next.
static const char * ALL_CTRLS[] = {
    "s/i/ctrl", "s/v/ctrl", "s/p/ctrl",
    "s/gpi/0/ctrl", "s/gpi/1/ctrl", "s/gpi/2/ctrl", "s/gpi/3/ctrl", "s/gpi/7/ctrl",
    "s/adc/0/ctrl", "s/adc/1/ctrl", "s/i/range/ctrl",
    NULL
};

static void disable_all_streams(struct app_s * self) {
    for (const char ** p = ALL_CTRLS; *p; ++p) {
        PUBLISH_U32(*p, 0);
    }
}

static int verify_testcase(const struct testcase_s * tc, uint32_t actual_duration_ms) {
    int failures = 0;
    printf("  results (%u ms wall clock):\n", (unsigned) actual_duration_ms);

    for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
        const struct sig_stats_s * s = &tc->sigs[k];
        if (!s->expect_data) {
            continue;
        }

        if (s->message_count < MIN_MESSAGE_COUNT) {
            printf("    FAIL %-18s only %u messages (expected >= %u)\n",
                   s->subtopic, s->message_count, MIN_MESSAGE_COUNT);
            failures++;
            continue;
        }

        // Average element count per message vs target = sample_rate / publish_rate.
        double target_elem = (double) s->expected_rate / (double) tc->publish_rate;
        double avg_elem = (double) s->total_elements / (double) s->message_count;
        double elem_dev = (avg_elem - target_elem) / target_elem * 100.0;

        // Aggregate samples per second vs sample_rate.
        double elapsed_s = (double) actual_duration_ms / 1000.0;
        double observed_sps = (double) s->total_elements / elapsed_s;
        double sps_dev = (observed_sps - (double) s->expected_rate)
                         / (double) s->expected_rate * 100.0;

        bool count_ok = (avg_elem >= target_elem * (1.0 - COUNT_TOL_PCT / 100.0))
                     && (avg_elem <= target_elem * (1.0 + COUNT_TOL_PCT / 100.0));
        bool rate_ok  = (observed_sps >= (double) s->expected_rate * (1.0 - RATE_TOL_PCT / 100.0))
                     && (observed_sps <= (double) s->expected_rate * (1.0 + RATE_TOL_PCT / 100.0));

        // Verify the stream metadata fields the Joulescope UI / buffer
        // depends on.  JS320 always uses 16 MHz native sample_id rate;
        // decimate_factor = sample_rate_native / expected_rate.  These
        // must propagate correctly all the way through the host driver
        // so the buffer can normalize sample_ids and the time map.
        uint32_t expected_decimate =
            (s->expected_rate > 0U) ? (16000000U / s->expected_rate) : 1U;
        bool sr_ok       = (s->sample_rate_native == 16000000U);
        bool decimate_ok = (s->decimate_factor == expected_decimate);
        // Sample-id continuity: every gap between adjacent messages is
        // a NaN-fill region in the buffer / UI.  There must be ZERO.
        bool sample_id_ok = (s->sample_id_gaps == 0);
        bool ok = count_ok && rate_ok && sr_ok && decimate_ok && sample_id_ok;

        printf("    %s %-18s n=%u avg_elem=%.0f (target %.0f, %+.1f%%) sps=%.0f (%+.1f%%)"
               " min=%u max=%u\n",
               ok ? "PASS" : "FAIL",
               s->subtopic, s->message_count, avg_elem, target_elem, elem_dev,
               observed_sps, sps_dev, s->min_element_count, s->max_element_count);
        if (!sr_ok) {
            printf("         sample_rate=%u (expected 16000000)\n",
                   s->sample_rate_native);
        }
        if (!decimate_ok) {
            printf("         decimate_factor=%u (expected %u for output rate %u Hz)\n",
                   s->decimate_factor, expected_decimate, s->expected_rate);
        }
        if (!sample_id_ok) {
            printf("         sample_id gaps=%u  worst delta=%" PRId64
                   " native ticks (= %" PRId64 " output samples)\n",
                   s->sample_id_gaps, s->worst_gap_delta,
                   s->worst_gap_delta / (int64_t) (s->decimate_factor ? s->decimate_factor : 1));
        }
        // Always print the time_map counter_rate so we can spot a
        // mismatch between firmware-reported and JS320-expected (16 MHz).
        // This is what the buffer divides by decimate_factor to get the
        // output sample rate the UI uses for the x-axis.
        printf("         time_map.counter_rate=%.1f Hz (expected 16000000.0)\n",
               s->tmap_counter_rate);
        if (s->f32_total_count > 0U) {
            printf("         f32 NaN count=%" PRIu64 " of %" PRIu64
                   " samples (%.2f%%)\n",
                   s->f32_nan_count, s->f32_total_count,
                   100.0 * (double) s->f32_nan_count / (double) s->f32_total_count);
            if (s->f32_nan_count != 0U) {
                ok = false;
            }
        }
        if (!ok) {
            failures++;
        }
    }

    // Group alignment check: for IVP signals, all enabled members should
    // share approximately the same last_sample_id_end.  Tolerance is one
    // publish window of sample_ids.
    uint64_t window = 0;
    if (tc->publish_rate > 0) {
        window = (uint64_t) (1000000U / tc->publish_rate) * 16ULL;
    }
    if (window) {
        // IVP group
        uint64_t min_end = UINT64_MAX, max_end = 0;
        uint32_t group_n = 0;
        for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
            const struct sig_stats_s * s = &tc->sigs[k];
            if (!s->expect_data || s->message_count < MIN_MESSAGE_COUNT) continue;
            if (s->field_id != JSDRV_FIELD_CURRENT && s->field_id != JSDRV_FIELD_VOLTAGE
                && s->field_id != JSDRV_FIELD_POWER) continue;
            if (s->last_sample_id_end < min_end) min_end = s->last_sample_id_end;
            if (s->last_sample_id_end > max_end) max_end = s->last_sample_id_end;
            ++group_n;
        }
        if (group_n >= 2) {
            uint64_t spread = max_end - min_end;
            bool ok = spread <= window;
            printf("    %s IVP group alignment: spread=%" PRIu64
                   " sample_ids (window=%" PRIu64 ")\n",
                   ok ? "PASS" : "FAIL", spread, window);
            if (!ok) failures++;
        }

        // GPIT group
        min_end = UINT64_MAX; max_end = 0; group_n = 0;
        for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
            const struct sig_stats_s * s = &tc->sigs[k];
            if (!s->expect_data || s->message_count < MIN_MESSAGE_COUNT) continue;
            if (s->field_id != JSDRV_FIELD_GPI) continue;
            if (s->last_sample_id_end < min_end) min_end = s->last_sample_id_end;
            if (s->last_sample_id_end > max_end) max_end = s->last_sample_id_end;
            ++group_n;
        }
        if (group_n >= 2) {
            uint64_t spread = max_end - min_end;
            bool ok = spread <= window;
            printf("    %s GPIT group alignment: spread=%" PRIu64
                   " sample_ids (window=%" PRIu64 ")\n",
                   ok ? "PASS" : "FAIL", spread, window);
            if (!ok) failures++;
        }
    }

    return failures;
}

static int run_testcase(struct app_s * self, struct testcase_s * tc) {
    printf("\n[TEST] %s\n", tc->name);

    // Reset per-test stats.
    for (uint32_t k = 0; k < MAX_SIGS; ++k) {
        struct sig_stats_s * s = &tc->sigs[k];
        if (!s->subtopic) break;
        s->message_count = 0;
        s->total_elements = 0;
        s->first_sample_id = 0;
        s->last_sample_id_end = 0;
        s->min_element_count = 0;
        s->max_element_count = 0;
        s->decimate_factor = 0;
        s->sample_rate_native = 0;
        s->field_id = 0;
    }

    // Make sure no leftover streams from a previous test.
    disable_all_streams(self);
    jsdrv_thread_sleep_ms(50);

    // Configure rate first so the smart-power decision sees correct fs
    // before any stream ctrl flips.
    PUBLISH_U32("h/fs", tc->fs);
    PUBLISH_U32("h/fp", tc->publish_rate);

    // Always publish the dwn knobs from the test case.  Each test case
    // sets the values explicitly (firmware defaults are signal=0,
    // gpi_mode=2, gpi_n=16) so behavior is deterministic regardless of
    // any previous test's leftover state.
    PUBLISH_U32("s/dwnN/N",          tc->signal_dwn_n);
    PUBLISH_U32("s/gpi/+/dwnN/mode", tc->gpi_dwn_mode);
    PUBLISH_U32("s/gpi/+/dwnN/N",    tc->gpi_dwn_n);

    // Subscribe to expected topics BEFORE enabling, so the first batches
    // are captured.
    for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
        struct sig_stats_s * s = &tc->sigs[k];
        if (!s->expect_data) continue;
        struct jsdrv_topic_s topic;
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, s->subtopic);
        int32_t rc = jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB,
                                     on_data, s, JSDRV_TIMEOUT_MS_DEFAULT);
        if (rc) {
            printf("  subscribe %s failed: %d\n", topic.topic, rc);
        }
    }

    // Enable each requested ctrl.
    for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
        struct sig_stats_s * s = &tc->sigs[k];
        if (!s->enable) continue;
        PUBLISH_U32(s->ctrl_topic, 1);
    }

    jsdrv_thread_sleep_ms(tc->duration_ms);

    // Disable cleanly and let in-flight messages drain.
    for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
        struct sig_stats_s * s = &tc->sigs[k];
        if (!s->enable) continue;
        PUBLISH_U32(s->ctrl_topic, 0);
    }
    jsdrv_thread_sleep_ms(100);

    // Unsubscribe so the callbacks don't see further data.
    for (uint32_t k = 0; k < MAX_SIGS && tc->sigs[k].subtopic; ++k) {
        struct sig_stats_s * s = &tc->sigs[k];
        if (!s->expect_data) continue;
        struct jsdrv_topic_s topic;
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, s->subtopic);
        jsdrv_unsubscribe(self->context, topic.topic, on_data, s, JSDRV_TIMEOUT_MS_DEFAULT);
    }

    return verify_testcase(tc, tc->duration_ms);
}

static int run(struct app_s * self, const char * device) {
    int total_failures = 0;
    int32_t rc = 0;

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME,
                   JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_thread_sleep_ms(200);  // let timesync settle

    for (uint32_t i = 0; i < TESTS_COUNT && !quit_; ++i) {
        int failures = run_testcase(self, &TESTS[i]);
        if (failures) {
            printf("  -> %d failure(s)\n", failures);
            total_failures += failures;
        } else {
            printf("  -> OK\n");
        }
    }

    disable_all_streams(self);
    jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);

    printf("\n=== stream_test summary: %d failure(s) across %u test(s) ===\n",
           total_failures, (unsigned) TESTS_COUNT);
    if (total_failures) {
        rc = 1;
    }
    return rc;
}

static int usage(void) {
    printf(
        "usage: minibitty stream_test device_path\n"
        "\n"
        "  Hardware-in-the-loop regression test for the JS320 streaming\n"
        "  pipeline.  Walks through several combinations of h/fp, h/fs,\n"
        "  and signal enables, and verifies per-signal message counts,\n"
        "  per-message element counts, aggregate sample rates, and\n"
        "  group alignment.  Exits 0 on success, non-zero if any test\n"
        "  fails.\n"
        );
    return 1;
}

int on_stream_test(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
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
