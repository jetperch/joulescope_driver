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

// JS320 driver unit tests.  These exercise the static helpers in
// js320_drv.c (port table, frame combining, group alignment, smart
// power) by including the source file directly and stubbing every
// dependency on mb_device, jtag, fwup, and the message allocator.
// The test links jsdrv_support_objlib for jsdrv_topic_*, jsdrv_cstr_*,
// jsdrv_union_*, sbuf_f32_*, jsdrv_alloc_*, and jsdrv_time_utc.

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "jsdrv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/topic.h"
#include "jsdrv/union.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/js320_fwup.h"
#include "jsdrv_prv/js320_jtag.h"
#include "jsdrv_prv/js320_stats.h"
#include "jsdrv_prv/mb_drv.h"
#include "jsdrv_prv/platform.h"


// --- Capture state for stubs ---

#define CAPTURE_MAX 256

struct device_publish_s {
    char topic[64];
    uint32_t value_u32;
};

struct return_code_s {
    char topic[64];
    int32_t rc;
};

struct test_capture_s {
    struct jsdrvp_msg_s * backend_sends[CAPTURE_MAX];
    uint32_t backend_send_count;

    struct device_publish_s device_publishes[CAPTURE_MAX];
    uint32_t device_publish_count;

    struct return_code_s return_codes[CAPTURE_MAX];
    uint32_t return_code_count;

    struct jsdrv_time_map_s time_map;
};

static struct test_capture_s g_cap;

static void capture_reset(void) {
    for (uint32_t i = 0; i < g_cap.backend_send_count; ++i) {
        if (g_cap.backend_sends[i]) {
            jsdrv_free(g_cap.backend_sends[i]);
            g_cap.backend_sends[i] = NULL;
        }
    }
    memset(&g_cap, 0, sizeof(g_cap));
}


// --- Stubs for jsdrv message alloc/free ---

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    (void) context;
    size_t sz = sizeof(struct jsdrvp_msg_s) - sizeof(union jsdrvp_payload_u)
                + sizeof(struct jsdrv_stream_signal_s);
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sz);
    m->value = jsdrv_union_bin(&m->payload.bin[0], 0);
    if (topic) {
        jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    }
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context,
                                              const char * topic,
                                              const struct jsdrv_union_s * value) {
    (void) context; (void) topic; (void) value;
    return NULL;
}

struct jsdrvp_msg_s * jsdrvp_msg_clone(struct jsdrv_context_s * context,
                                        const struct jsdrvp_msg_s * msg_src) {
    (void) context; (void) msg_src;
    return NULL;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * m) {
    (void) context;
    jsdrv_free(m);
}


// --- Stubs for mb_device upper-driver services ---

void jsdrvp_mb_dev_send_to_frontend(struct jsdrvp_mb_dev_s * dev,
                                     const char * subtopic,
                                     const struct jsdrv_union_s * value) {
    (void) dev; (void) subtopic; (void) value;
}

void jsdrvp_mb_dev_publish_to_device(struct jsdrvp_mb_dev_s * dev,
                                      const char * topic,
                                      const struct jsdrv_union_s * value) {
    (void) dev;
    if (g_cap.device_publish_count < CAPTURE_MAX) {
        struct device_publish_s * p = &g_cap.device_publishes[g_cap.device_publish_count++];
        jsdrv_cstr_copy(p->topic, topic, sizeof(p->topic));
        p->value_u32 = value->value.u32;
    }
}

void jsdrvp_mb_dev_send_to_device(struct jsdrvp_mb_dev_s * dev,
                                   enum mb_frame_service_type_e service_type,
                                   uint16_t metadata,
                                   const uint32_t * data,
                                   uint32_t length_u32) {
    (void) dev; (void) service_type; (void) metadata; (void) data; (void) length_u32;
}

struct jsdrv_context_s * jsdrvp_mb_dev_context(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return NULL;
}

const char * jsdrvp_mb_dev_prefix(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return "u/js320/test";
}

void jsdrvp_mb_dev_backend_send(struct jsdrvp_mb_dev_s * dev,
                                 struct jsdrvp_msg_s * msg) {
    (void) dev;
    if (g_cap.backend_send_count < CAPTURE_MAX) {
        g_cap.backend_sends[g_cap.backend_send_count++] = msg;
    } else {
        jsdrv_free(msg);
    }
}

void jsdrvp_mb_dev_set_timeout(struct jsdrvp_mb_dev_s * dev, int64_t timeout_utc) {
    (void) dev; (void) timeout_utc;
}

void jsdrvp_mb_dev_send_return_code(struct jsdrvp_mb_dev_s * dev,
                                     const char * subtopic,
                                     int32_t rc) {
    (void) dev;
    if (g_cap.return_code_count < CAPTURE_MAX) {
        struct return_code_s * r = &g_cap.return_codes[g_cap.return_code_count++];
        jsdrv_cstr_copy(r->topic, subtopic, sizeof(r->topic));
        r->rc = rc;
    }
}

int32_t jsdrvp_mb_dev_open_mode(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return 0;
}

const struct jsdrv_time_map_s * jsdrvp_mb_dev_time_map(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return &g_cap.time_map;
}

struct jsdrv_time_map_s * jsdrvp_mb_dev_time_map_mut(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return &g_cap.time_map;
}


// --- Stubs for js320 sub-modules (jtag, fwup, stats) ---

struct js320_jtag_s { int dummy; };
static struct js320_jtag_s g_jtag_stub;
struct js320_jtag_s * js320_jtag_alloc(void) { return &g_jtag_stub; }
void js320_jtag_free(struct js320_jtag_s * self) { (void) self; }
void js320_jtag_on_open(struct js320_jtag_s * self, struct jsdrvp_mb_dev_s * dev) { (void) self; (void) dev; }
void js320_jtag_on_close(struct js320_jtag_s * self) { (void) self; }
void js320_jtag_on_timeout(struct js320_jtag_s * self) { (void) self; }
bool js320_jtag_handle_cmd(struct js320_jtag_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    (void) self; (void) subtopic; (void) value;
    return false;
}
bool js320_jtag_handle_publish(struct js320_jtag_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    (void) self; (void) subtopic; (void) value;
    return false;
}

struct js320_fwup_s { int dummy; };
static struct js320_fwup_s g_fwup_stub;
struct js320_fwup_s * js320_fwup_alloc(void) { return &g_fwup_stub; }
void js320_fwup_free(struct js320_fwup_s * self) { (void) self; }
void js320_fwup_on_open(struct js320_fwup_s * self, struct jsdrvp_mb_dev_s * dev) { (void) self; (void) dev; }
void js320_fwup_on_close(struct js320_fwup_s * self) { (void) self; }
void js320_fwup_on_timeout(struct js320_fwup_s * self) { (void) self; }
bool js320_fwup_handle_cmd(struct js320_fwup_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    (void) self; (void) subtopic; (void) value;
    return false;
}
bool js320_fwup_handle_publish(struct js320_fwup_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    (void) self; (void) subtopic; (void) value;
    return false;
}

int32_t js320_stats_convert(const struct js320_statistics_raw_s * src, struct jsdrv_statistics_s * dst) {
    (void) src; (void) dst;
    return -1;
}

// js320_drv_factory in the source under test calls into the mb_device USB
// factory.  Stub it to skip the lower-half setup; tests construct the
// upper driver directly via js320_drv_factory().
int32_t jsdrvp_ul_mb_device_usb_factory(struct jsdrvp_ul_device_s ** device,
                                         struct jsdrv_context_s * context,
                                         struct jsdrvp_ll_device_s * ll,
                                         struct jsdrvp_mb_drv_s * drv) {
    (void) device; (void) context; (void) ll; (void) drv;
    return 0;
}


// --- Now pull in the driver source under test ---

// js320_drv.c sets JSDRV_LOG_LEVEL itself; suppress the redefinition warning
// since the test file may have pulled in log.h transitively already.
#undef JSDRV_LOG_LEVEL
#include "../src/js320_drv.c"


// --- Test fixtures ---

static struct js320_drv_s * make_drv(void) {
    struct jsdrvp_mb_drv_s * drv = NULL;
    int32_t rc = js320_drv_factory(&drv);
    assert_int_equal(0, rc);
    return (struct js320_drv_s *) drv;
}

static int test_setup(void ** state) {
    capture_reset();
    *state = make_drv();
    return 0;
}

static int test_teardown(void ** state) {
    struct js320_drv_s * self = *state;
    if (self && self->drv.finalize) {
        self->drv.finalize(&self->drv);
    }
    capture_reset();
    return 0;
}


// --- Helpers ---

// Find the most-recently-captured device publish for a given topic.
static const struct device_publish_s * find_publish(const char * topic) {
    for (int32_t i = (int32_t) g_cap.device_publish_count - 1; i >= 0; --i) {
        if (0 == strcmp(g_cap.device_publishes[i].topic, topic)) {
            return &g_cap.device_publishes[i];
        }
    }
    return NULL;
}

// Count the number of captured publishes for a given topic.
static uint32_t count_publishes(const char * topic) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cap.device_publish_count; ++i) {
        if (0 == strcmp(g_cap.device_publishes[i].topic, topic)) {
            ++n;
        }
    }
    return n;
}

// Build a fake APP frame: [u64 sample_id][n u32 of payload data].
// Returns the resulting length (in u32 words) suitable for handle_app.
// `out` must hold at least n+2 u32 words.
static uint8_t build_frame(uint32_t * out, uint64_t sample_id, const uint32_t * data, uint32_t n) {
    memcpy(out, &sample_id, sizeof(sample_id));
    if (n) {
        memcpy(out + 2, data, n * sizeof(uint32_t));
    }
    return (uint8_t) (n + 2U);
}


// --- Tests: h/fp handler ---

static void test_h_fp_default(void ** state) {
    struct js320_drv_s * self = *state;
    assert_int_equal(20, self->publish_rate);
    assert_int_equal(1000000, self->fs);
}

static void test_h_fp_set(void ** state) {
    struct js320_drv_s * self = *state;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(100));
    assert_true(handled);
    assert_int_equal(100, self->publish_rate);
    assert_int_equal(1, g_cap.return_code_count);
    assert_int_equal(0, g_cap.return_codes[0].rc);
    assert_string_equal("h/fp", g_cap.return_codes[0].topic);
}

static void test_h_fp_clamp_zero(void ** state) {
    struct js320_drv_s * self = *state;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(0));
    assert_true(handled);
    // Zero is clamped to 1 to avoid div-by-zero in element_count_max.
    assert_int_equal(1, self->publish_rate);
}


// --- Tests: smart power state machine ---

static void test_smart_power_enable_i_only(void ** state) {
    struct js320_drv_s * self = *state;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    assert_true(handled);
    assert_false(self->power_compute_on_host);
    // First reconcile flushes the unknown cache for all three i/v/p ctrls.
    assert_int_equal(1, find_publish("s/i/ctrl")->value_u32);
    assert_int_equal(0, find_publish("s/v/ctrl")->value_u32);
    assert_int_equal(0, find_publish("s/p/ctrl")->value_u32);
}

static void test_smart_power_iv_no_compute(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    assert_false(self->power_compute_on_host);
    assert_int_equal(1, find_publish("s/i/ctrl")->value_u32);
    assert_int_equal(1, find_publish("s/v/ctrl")->value_u32);
    assert_int_equal(0, find_publish("s/p/ctrl")->value_u32);
}

static void test_smart_power_ivp_high_rate_host_compute(void ** state) {
    struct js320_drv_s * self = *state;
    // fs default is 1 MHz.
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/p/ctrl", &jsdrv_union_u32_r(1));
    assert_true(self->power_compute_on_host);
    // The most recent forwarded s/p/ctrl must remain 0 (host computes p).
    assert_int_equal(0, find_publish("s/p/ctrl")->value_u32);
    // i and v are still on at the device.
    assert_int_equal(1, find_publish("s/i/ctrl")->value_u32);
    assert_int_equal(1, find_publish("s/v/ctrl")->value_u32);
}

static void test_smart_power_ivp_low_rate_device_compute(void ** state) {
    struct js320_drv_s * self = *state;
    // Drop fs below 1 MHz; smart-power must NOT activate even with all 3.
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(500000));
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/p/ctrl", &jsdrv_union_u32_r(1));
    assert_false(self->power_compute_on_host);
    // Device gets all three enabled.
    assert_int_equal(1, find_publish("s/i/ctrl")->value_u32);
    assert_int_equal(1, find_publish("s/v/ctrl")->value_u32);
    assert_int_equal(1, find_publish("s/p/ctrl")->value_u32);
}

static void test_smart_power_fs_transition_clears_compute(void ** state) {
    struct js320_drv_s * self = *state;
    // Start at 1 MHz with all 3 enabled -> host_compute=true.
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/p/ctrl", &jsdrv_union_u32_r(1));
    assert_true(self->power_compute_on_host);

    // Drop to 500 kHz: device must take over power streaming.
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(500000));
    assert_false(self->power_compute_on_host);
    assert_int_equal(1, find_publish("s/p/ctrl")->value_u32);
}

static void test_smart_power_no_double_forward(void ** state) {
    struct js320_drv_s * self = *state;
    // Get into i+v+p host_compute state.
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    uint32_t before = count_publishes("s/p/ctrl");
    self->drv.handle_cmd(&self->drv, NULL, "s/p/ctrl", &jsdrv_union_u32_r(1));
    uint32_t after = count_publishes("s/p/ctrl");
    // The user said s/p/ctrl=1, but the device cache already had 0 from the
    // previous reconcile so reconcile_power must NOT re-publish: count
    // unchanged.
    assert_int_equal(before, after);
}


// --- Tests: frame combining + group flush ---

static void push_current_frame(struct js320_drv_s * self,
                                uint64_t sample_id,
                                const float * samples,
                                uint32_t n) {
    uint32_t buf[64];
    assert_true(n + 2 <= 64);
    uint8_t length = build_frame(buf, sample_id, (const uint32_t *) samples, n);
    self->drv.handle_app(&self->drv, NULL, /*ch=*/5, buf, length);
}

static void push_voltage_frame(struct js320_drv_s * self,
                                uint64_t sample_id,
                                const float * samples,
                                uint32_t n) {
    uint32_t buf[64];
    assert_true(n + 2 <= 64);
    uint8_t length = build_frame(buf, sample_id, (const uint32_t *) samples, n);
    self->drv.handle_app(&self->drv, NULL, /*ch=*/6, buf, length);
}

static void test_frame_combining_rate_budget(void ** state) {
    struct js320_drv_s * self = *state;
    // publish_rate=20 -> element_count_max=50000 (1 MHz / 20).  Pick a
    // smaller publish_rate so the test fits in a few calls.
    self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(20000));
    g_cap.return_code_count = 0;
    // 1 MHz / 20000 = 50 samples per flush.

    float samples[10];
    for (uint32_t k = 0; k < 10; ++k) {
        samples[k] = (float) k;
    }

    // 4 frames of 10 samples each = 40 samples: not enough to flush.
    for (uint32_t i = 0; i < 4; ++i) {
        push_current_frame(self, /*sample_id=*/i * 10ULL * JS320_DECIMATE, samples, 10);
    }
    assert_int_equal(0, g_cap.backend_send_count);

    // 5th frame brings element_count to 50, which triggers the flush.
    push_current_frame(self, /*sample_id=*/4 * 10ULL * JS320_DECIMATE, samples, 10);
    assert_int_equal(1, g_cap.backend_send_count);

    struct jsdrv_stream_signal_s * sig =
        (struct jsdrv_stream_signal_s *) g_cap.backend_sends[0]->payload.bin;
    assert_int_equal(50, sig->element_count);
    assert_int_equal(0, sig->sample_id);
    assert_int_equal(JSDRV_FIELD_CURRENT, sig->field_id);
    // JS320 sample_id increments at 16 MHz native; full-rate i/v/p
    // is decimated 16:1 to 1 MHz, so decimate_factor must be 16.
    assert_int_equal(16000000, sig->sample_rate);
    assert_int_equal(16, sig->decimate_factor);
}

static void test_group_alignment_ivp(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(20000));
    g_cap.return_code_count = 0;

    float samples[25];
    for (uint32_t k = 0; k < 25; ++k) { samples[k] = (float) k; }

    // Push 25 samples on i (no flush yet).
    push_current_frame(self, 0, samples, 25);
    // Push 25 samples on v (no flush yet).
    push_voltage_frame(self, 0, samples, 25);
    assert_int_equal(0, g_cap.backend_send_count);

    // Push 25 more on i -> i hits element_count_max=50, triggers GROUP flush.
    push_current_frame(self, 25ULL * JS320_DECIMATE, samples, 25);

    // Both i and v should have been flushed even though v has fewer samples.
    assert_int_equal(2, g_cap.backend_send_count);

    struct jsdrv_stream_signal_s * s0 =
        (struct jsdrv_stream_signal_s *) g_cap.backend_sends[0]->payload.bin;
    struct jsdrv_stream_signal_s * s1 =
        (struct jsdrv_stream_signal_s *) g_cap.backend_sends[1]->payload.bin;

    // Expect one CURRENT and one VOLTAGE flush, both starting at sample_id 0.
    bool saw_i = false, saw_v = false;
    if (s0->field_id == JSDRV_FIELD_CURRENT && s1->field_id == JSDRV_FIELD_VOLTAGE) {
        saw_i = saw_v = true;
        assert_int_equal(50, s0->element_count);
        assert_int_equal(25, s1->element_count);
    } else if (s0->field_id == JSDRV_FIELD_VOLTAGE && s1->field_id == JSDRV_FIELD_CURRENT) {
        saw_i = saw_v = true;
        assert_int_equal(25, s0->element_count);
        assert_int_equal(50, s1->element_count);
    }
    assert_true(saw_i && saw_v);
}

static void test_compute_power_correctness(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(20000));
    // Enable host-side compute by enabling all three at full rate.
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/v/ctrl", &jsdrv_union_u32_r(1));
    self->drv.handle_cmd(&self->drv, NULL, "s/p/ctrl", &jsdrv_union_u32_r(1));
    assert_true(self->power_compute_on_host);
    g_cap.backend_send_count = 0;
    g_cap.return_code_count = 0;

    // Push matching i and v frames (1 MHz, 25 samples each, sample_id steps
    // by 16 per sample due to JS320_DECIMATE).
    float i_samples[25];
    float v_samples[25];
    for (uint32_t k = 0; k < 25; ++k) {
        i_samples[k] = (float) k * 0.1f;
        v_samples[k] = 5.0f;
    }
    push_current_frame(self, 0, i_samples, 25);
    push_voltage_frame(self, 0, v_samples, 25);

    // Buffers should accumulate but no flush yet (need 50 samples for fp=20000).
    assert_int_equal(0, g_cap.backend_send_count);

    // Push 25 more on each.  i hits the rate budget, group flush sends i, v, p.
    push_current_frame(self, 25ULL * JS320_DECIMATE, i_samples, 25);
    push_voltage_frame(self, 25ULL * JS320_DECIMATE, v_samples, 25);

    // Expect 3 flushes: i, v, p (in some order).
    assert_int_equal(3, g_cap.backend_send_count);

    bool found_p = false;
    for (uint32_t i = 0; i < g_cap.backend_send_count; ++i) {
        struct jsdrv_stream_signal_s * sig =
            (struct jsdrv_stream_signal_s *) g_cap.backend_sends[i]->payload.bin;
        if (sig->field_id == JSDRV_FIELD_POWER) {
            found_p = true;
            // p_k = i_k * v_k = (k * 0.1) * 5.0 = 0.5 * k
            float * pdata = (float *) (sig->data);
            for (uint32_t k = 0; k < sig->element_count && k < 25; ++k) {
                float expected = (float) k * 0.5f;
                assert_true(fabsf(pdata[k] - expected) < 1e-4f);
            }
        }
    }
    assert_true(found_p);
}

static void test_dwnN_signal_tracked_and_forwarded(void ** state) {
    struct js320_drv_s * self = *state;
    // Default signal_dwn_n is 0 (passthrough); set to 4 -> i/v/p decimate by 64.
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    // Handler now returns true because it explicitly forwards via
    // jsdrvp_mb_dev_publish_to_device() and emits a return code.
    assert_true(handled);
    assert_int_equal(4, self->signal_dwn_n);

    // The value must have been forwarded to the device.
    const struct device_publish_s * p = find_publish("s/dwnN/N");
    assert_non_null(p);
    assert_int_equal(4, p->value_u32);

    // The ack-bookkeeping state must have advanced.
    assert_int_equal(1, self->signal_ack.acks_outstanding);
    assert_true(self->signal_ack.dropping);

    // The runtime decimate for ch 5 (current) should now be 16 * 4 = 64.
    assert_int_equal(64, js320_runtime_decimate(self, 5));
    assert_int_equal(64, js320_runtime_decimate(self, 6));
    assert_int_equal(64, js320_runtime_decimate(self, 7));
    // GPI is unaffected.
    assert_int_equal(JS320_DECIMATE, js320_runtime_decimate(self, 8));
}

static void test_dwnN_signal_passthrough_codes(void ** state) {
    struct js320_drv_s * self = *state;
    // Both 0 and 1 are passthrough -> factor 1 -> runtime = 16.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(0));
    assert_int_equal(JS320_DECIMATE, js320_runtime_decimate(self, 5));
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(1));
    assert_int_equal(JS320_DECIMATE, js320_runtime_decimate(self, 5));
    // 3 maps to 2 (gateware clamp) -> runtime = 32.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(3));
    assert_int_equal(JS320_DECIMATE * 2, js320_runtime_decimate(self, 5));
    // 2 -> factor 2 -> runtime = 32.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(2));
    assert_int_equal(JS320_DECIMATE * 2, js320_runtime_decimate(self, 5));
}

static void test_dwnN_gpi_mode_off(void ** state) {
    struct js320_drv_s * self = *state;
    // Default GPI: mode=2, N=16 -> decimate=16.  mode=0 -> passthrough -> 1.
    bool handled = self->drv.handle_cmd(&self->drv, NULL,
        "s/gpi/+/dwnN/mode", &jsdrv_union_u32_r(0));
    // Handler now returns true (forwards explicitly via apply helper
    // and emits a return code).
    assert_true(handled);
    assert_int_equal(0, self->gpi_dwn_mode);
    // Value forwarded to device.
    const struct device_publish_s * p = find_publish("s/gpi/+/dwnN/mode");
    assert_non_null(p);
    assert_int_equal(0, p->value_u32);
    // GPI ack bookkeeping advanced; signal_ack untouched.
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);
    assert_int_equal(0, self->signal_ack.acks_outstanding);
    assert_false(self->signal_ack.dropping);
    assert_int_equal(1, js320_runtime_decimate(self, 8));
    assert_int_equal(1, js320_runtime_decimate(self, 12));
    // Signal channels are unaffected.
    assert_int_equal(JS320_DECIMATE, js320_runtime_decimate(self, 5));
}

static void test_dwnN_gpi_n_change(void ** state) {
    struct js320_drv_s * self = *state;
    // mode stays at default 2 (first); change N to 8.
    bool handled = self->drv.handle_cmd(&self->drv, NULL,
        "s/gpi/+/dwnN/N", &jsdrv_union_u32_r(8));
    assert_true(handled);
    assert_int_equal(8, self->gpi_dwn_n);
    const struct device_publish_s * p = find_publish("s/gpi/+/dwnN/N");
    assert_non_null(p);
    assert_int_equal(8, p->value_u32);
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);
    assert_int_equal(8, js320_runtime_decimate(self, 8));
    assert_int_equal(8, js320_runtime_decimate(self, 12));
}

// --- Tests: closed-loop s/dwnN/!ack + drop-until-ack ---

// Simulate the firmware publishing s/dwnN/!ack with the given sample_id.
static void push_dwnN_ack(struct js320_drv_s * self, uint64_t sample_id) {
    struct jsdrv_union_s v = jsdrv_union_u64(sample_id);
    bool handled = self->drv.handle_publish(&self->drv, NULL, "s/dwnN/!ack", &v);
    assert_true(handled);
}

static void test_fs_to_dwn_n_mapping(void ** state) {
    (void) state;
    uint32_t n = 99U;
    assert_int_equal(0, js320_fs_to_dwn_n(1000000U, &n));
    assert_int_equal(0U, n);
    assert_int_equal(0, js320_fs_to_dwn_n(500000U, &n));
    assert_int_equal(2U, n);
    assert_int_equal(0, js320_fs_to_dwn_n(250000U, &n));
    assert_int_equal(4U, n);
    assert_int_equal(0, js320_fs_to_dwn_n(1000U, &n));
    assert_int_equal(1000U, n);
    // fs=333333 doesn't divide 1 MHz evenly.
    assert_int_not_equal(0, js320_fs_to_dwn_n(333333U, &n));
    // Factor-3 rejected (gateware clamps to 2).
    assert_int_not_equal(0, js320_fs_to_dwn_n(1000000U / 3U, &n));
    // fs > 1 MHz rejected.
    assert_int_not_equal(0, js320_fs_to_dwn_n(2000000U, &n));
    // fs=0 rejected.
    assert_int_not_equal(0, js320_fs_to_dwn_n(0U, &n));
}

static void test_hfs_unified_forwards_dwnN(void ** state) {
    struct js320_drv_s * self = *state;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(250000));
    assert_true(handled);
    assert_int_equal(250000, self->fs);
    // 1e6 / 250e3 = 4 -> N=4.
    const struct device_publish_s * p = find_publish("s/dwnN/N");
    assert_non_null(p);
    assert_int_equal(4, p->value_u32);
    assert_int_equal(4, self->signal_dwn_n);
    assert_true(self->signal_ack.dropping);
    assert_int_equal(1, self->signal_ack.acks_outstanding);
    // return code OK.
    assert_int_equal(0, g_cap.return_codes[g_cap.return_code_count - 1].rc);
}

static void test_hfs_unified_rejects_invalid_rate(void ** state) {
    struct js320_drv_s * self = *state;
    uint32_t before = count_publishes("s/dwnN/N");
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(333333));
    assert_true(handled);
    // No device publish and fs unchanged.
    assert_int_equal(before, count_publishes("s/dwnN/N"));
    assert_int_equal(1000000, self->fs);
    // Return code is non-zero.
    assert_int_not_equal(0, g_cap.return_codes[g_cap.return_code_count - 1].rc);
    // Not dropping.
    assert_false(self->signal_ack.dropping);
}

static void test_dwnN_drop_until_ack_single(void ** state) {
    struct js320_drv_s * self = *state;
    // Keep the default publish_rate (20 Hz) so 25-sample frames stay well
    // under the flush threshold and msg_in survives the append.
    g_cap.return_code_count = 0;
    g_cap.backend_send_count = 0;

    // Trigger a dwnN change.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_true(self->signal_ack.dropping);
    assert_int_equal(1, self->signal_ack.acks_outstanding);

    // 25 current samples while ack is still outstanding: must be dropped.
    float samples[25];
    for (uint32_t k = 0; k < 25; ++k) { samples[k] = (float) k; }
    push_current_frame(self, 100ULL, samples, 25);
    // Nothing buffered (dropped).
    assert_null(self->ports[5].msg_in);

    // Ack arrives naming a high-water sample_id.
    push_dwnN_ack(self, 1000ULL);
    assert_int_equal(0, self->signal_ack.acks_outstanding);
    // Still dropping — the data path clears it after seeing an at-or-after frame.
    assert_true(self->signal_ack.dropping);

    // Pre-ack frame (sample_id < 1000): dropped.
    push_current_frame(self, 500ULL, samples, 25);
    assert_null(self->ports[5].msg_in);
    assert_true(self->signal_ack.dropping);

    // At-or-after frame: accepted; drop window closes.
    push_current_frame(self, 1000ULL, samples, 25);
    assert_false(self->signal_ack.dropping);
    assert_non_null(self->ports[5].msg_in);
}

static void test_dwnN_drop_until_ack_multiple(void ** state) {
    struct js320_drv_s * self = *state;
    g_cap.return_code_count = 0;
    g_cap.backend_send_count = 0;

    // Three rapid dwnN changes -> 3 outstanding acks.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(2));
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(8));
    assert_int_equal(3, self->signal_ack.acks_outstanding);
    assert_true(self->signal_ack.dropping);

    // Acks arrive with increasing sample_ids; latest wins.
    push_dwnN_ack(self, 100ULL);
    assert_int_equal(2, self->signal_ack.acks_outstanding);
    assert_true(self->signal_ack.dropping);
    push_dwnN_ack(self, 500ULL);
    assert_int_equal(1, self->signal_ack.acks_outstanding);

    float samples[4] = {0, 0, 0, 0};

    // Frame at sample_id=400 while acks still outstanding: dropped.
    push_current_frame(self, 400ULL, samples, 4);
    assert_null(self->ports[5].msg_in);

    push_dwnN_ack(self, 900ULL);
    assert_int_equal(0, self->signal_ack.acks_outstanding);
    assert_int_equal(900ULL, self->signal_ack.drop_until_sample_id);

    // Frame at sample_id=800 < 900: dropped.
    push_current_frame(self, 800ULL, samples, 4);
    assert_null(self->ports[5].msg_in);
    // Frame at sample_id=900: accepted; window closes.
    push_current_frame(self, 900ULL, samples, 4);
    assert_false(self->signal_ack.dropping);
    assert_non_null(self->ports[5].msg_in);
}

static void test_dwnN_drop_does_not_affect_gpi(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_true(self->signal_ack.dropping);

    // A GPI frame arrives during the drop window: must NOT be dropped.
    uint32_t payload[1] = {0};
    uint32_t buf[3];
    uint8_t length = build_frame(buf, /*sample_id=*/100ULL, payload, 1);
    self->drv.handle_app(&self->drv, NULL, /*ch=*/8U, buf, length);
    assert_non_null(self->ports[8].msg_in);
}

static void test_dwnN_ack_timeout_resumes(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_true(self->signal_ack.dropping);

    // Force the deadline into the past.
    self->signal_ack.drop_timeout_utc = jsdrv_time_utc() - 1;
    self->drv.on_timeout(&self->drv, NULL);
    assert_false(self->signal_ack.dropping);
    assert_int_equal(0, self->signal_ack.acks_outstanding);

    // After timeout, data frames are accepted regardless of sample_id.
    float samples[4] = {0};
    push_current_frame(self, 1ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

static void test_unsupported_channel_dropped(void ** state) {
    struct js320_drv_s * self = *state;
    uint32_t buf[3] = {0, 0, 0};
    // Channel 3 is unused -> data_topic NULL -> drop.
    self->drv.handle_app(&self->drv, NULL, /*ch=*/3, buf, 3);
    assert_int_equal(0, g_cap.backend_send_count);
}


// --- Tests: closed-loop s/gpi/+/dwnN/!ack + drop-until-ack ---

static void push_gpi_ack(struct js320_drv_s * self, uint64_t sample_id) {
    struct jsdrv_union_s v = jsdrv_union_u64(sample_id);
    bool handled = self->drv.handle_publish(&self->drv, NULL,
        "s/gpi/+/dwnN/!ack", &v);
    assert_true(handled);
}

// Push a GPI frame on ch 8.  Each u32 carries 32 packed 1-bit samples per
// PORT_DEFS[8], but for drop testing only the sample_id + channel matter.
static void push_gpi_frame(struct js320_drv_s * self,
                            uint64_t sample_id,
                            uint32_t payload_u32) {
    uint32_t buf[3];
    uint8_t length = build_frame(buf, sample_id, &payload_u32, 1);
    self->drv.handle_app(&self->drv, NULL, /*ch=*/8U, buf, length);
}

static void test_gpi_dwnN_mode_drop_until_ack(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/mode",
        &jsdrv_union_u32_r(1));
    assert_true(self->gpi_ack.dropping);
    assert_int_equal(1, self->gpi_ack.acks_outstanding);

    // Pre-ack GPI frame: dropped.
    push_gpi_frame(self, 50ULL, 0);
    assert_null(self->ports[8].msg_in);

    push_gpi_ack(self, 200ULL);
    assert_int_equal(0, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);

    // Frame below ack sample_id: dropped.
    push_gpi_frame(self, 100ULL, 0);
    assert_null(self->ports[8].msg_in);

    // Frame at target: accepted; window closes.
    push_gpi_frame(self, 200ULL, 0);
    assert_false(self->gpi_ack.dropping);
    assert_non_null(self->ports[8].msg_in);
}

static void test_gpi_dwnN_n_drop_until_ack(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N",
        &jsdrv_union_u32_r(32));
    assert_true(self->gpi_ack.dropping);
    assert_int_equal(1, self->gpi_ack.acks_outstanding);

    push_gpi_ack(self, 10ULL);
    // First frame at or after target is accepted.
    push_gpi_frame(self, 10ULL, 0);
    assert_false(self->gpi_ack.dropping);
    assert_non_null(self->ports[8].msg_in);
}

static void test_gpi_dwnN_multiple_acks(void ** state) {
    struct js320_drv_s * self = *state;
    // mode + N back-to-back: two outstanding GPI acks.
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/mode",
        &jsdrv_union_u32_r(3));
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N",
        &jsdrv_union_u32_r(32));
    assert_int_equal(2, self->gpi_ack.acks_outstanding);

    push_gpi_ack(self, 100ULL);
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);

    // Frame while still outstanding: dropped.
    push_gpi_frame(self, 500ULL, 0);
    assert_null(self->ports[8].msg_in);

    push_gpi_ack(self, 600ULL);
    assert_int_equal(0, self->gpi_ack.acks_outstanding);
    assert_int_equal(600ULL, self->gpi_ack.drop_until_sample_id);

    push_gpi_frame(self, 600ULL, 0);
    assert_false(self->gpi_ack.dropping);
    assert_non_null(self->ports[8].msg_in);
}

static void test_gpi_dwnN_does_not_affect_signal(void ** state) {
    struct js320_drv_s * self = *state;
    // Start a GPI drop window.
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/mode",
        &jsdrv_union_u32_r(1));
    assert_true(self->gpi_ack.dropping);
    assert_false(self->signal_ack.dropping);

    // An i/v/p frame arrives during the GPI drop window: must be accepted.
    float samples[4] = {0};
    push_current_frame(self, 42ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

static void test_gpi_dwnN_ack_timeout_resumes(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N",
        &jsdrv_union_u32_r(32));
    assert_true(self->gpi_ack.dropping);

    self->gpi_ack.drop_timeout_utc = jsdrv_time_utc() - 1;
    self->drv.on_timeout(&self->drv, NULL);
    assert_false(self->gpi_ack.dropping);
    assert_int_equal(0, self->gpi_ack.acks_outstanding);

    push_gpi_frame(self, 1ULL, 0);
    assert_non_null(self->ports[8].msg_in);
}

// A stale signal !ack (no signal_ack window open) must NOT leak into or
// disturb the GPI window.
static void test_dwnN_signal_ack_independent_of_gpi(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N",
        &jsdrv_union_u32_r(32));
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    // Simulate a spurious s/dwnN/!ack arriving: should NOT touch gpi_ack.
    push_dwnN_ack(self, 999ULL);
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);
}


// --- Test runner ---

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_h_fp_default,                   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_h_fp_set,                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_h_fp_clamp_zero,                test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_enable_i_only,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_iv_no_compute,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_ivp_high_rate_host_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_ivp_low_rate_device_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_fs_transition_clears_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_no_double_forward,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_frame_combining_rate_budget,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_group_alignment_ivp,            test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_compute_power_correctness,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_tracked_and_forwarded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_passthrough_codes,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_mode_off,              test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_n_change,              test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_fs_to_dwn_n_mapping,            test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_unified_forwards_dwnN,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_unified_rejects_invalid_rate, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_drop_until_ack_single,     test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_drop_until_ack_multiple,   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_drop_does_not_affect_gpi,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_ack_timeout_resumes,       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_unsupported_channel_dropped,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwnN_mode_drop_until_ack,   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwnN_n_drop_until_ack,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwnN_multiple_acks,         test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwnN_does_not_affect_signal, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwnN_ack_timeout_resumes,   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_ack_independent_of_gpi, test_setup, test_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
