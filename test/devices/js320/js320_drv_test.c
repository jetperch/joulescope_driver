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
#include "jsdrv_prv/devices/js320/js320_fwup.h"
#include "jsdrv_prv/devices/js320/js320_jtag.h"
#include "jsdrv_prv/devices/js320/js320_stats.h"
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
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

struct frontend_send_s {
    char topic[64];
    uint8_t type;
    uint8_t flags;
    const char * str;
    uint32_t u32;
};

struct test_capture_s {
    struct jsdrvp_msg_s * backend_sends[CAPTURE_MAX];
    uint32_t backend_send_count;

    struct device_publish_s device_publishes[CAPTURE_MAX];
    uint32_t device_publish_count;

    struct return_code_s return_codes[CAPTURE_MAX];
    uint32_t return_code_count;

    struct frontend_send_s frontend_sends[CAPTURE_MAX];
    uint32_t frontend_send_count;

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
    (void) dev;
    if (g_cap.frontend_send_count < CAPTURE_MAX) {
        struct frontend_send_s * f = &g_cap.frontend_sends[g_cap.frontend_send_count++];
        jsdrv_cstr_copy(f->topic, subtopic, sizeof(f->topic));
        f->type = value->type;
        f->flags = value->flags;
        f->str = value->value.str;
        f->u32 = value->value.u32;
    }
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

const struct jsdrv_time_map_s * jsdrvp_mb_dev_time_map(struct jsdrvp_mb_dev_s * dev,
                                                        char prefix) {
    (void) dev;
    (void) prefix;
    // Match production semantics: NULL until populated.
    if (g_cap.time_map.counter_rate <= 0.0) {
        return NULL;
    }
    return &g_cap.time_map;
}

void jsdrvp_mb_dev_state_fetch_start(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
}

bool jsdrvp_mb_dev_instance_state_sync(struct jsdrvp_mb_dev_s * dev, char prefix, bool emit_open) {
    (void) dev;
    (void) prefix;
    (void) emit_open;
    return true;
}

void jsdrvp_mb_dev_open_complete(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
}

void jsdrvp_mb_dev_host_replay(struct jsdrvp_mb_dev_s * dev, char prefix) {
    (void) dev;
    (void) prefix;
}

void jsdrvp_mb_dev_topic_replay(struct jsdrvp_mb_dev_s * dev, const char * subtopic) {
    (void) dev;
    (void) subtopic;
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

struct js320_cal_s { int dummy; };
static struct js320_cal_s g_cal_stub;
struct js320_cal_s * js320_cal_alloc(void) { return &g_cal_stub; }
void js320_cal_free(struct js320_cal_s * self) { (void) self; }
void js320_cal_on_open(struct js320_cal_s * self, struct jsdrvp_mb_dev_s * dev) { (void) self; (void) dev; }
void js320_cal_on_close(struct js320_cal_s * self) { (void) self; }
void js320_cal_on_timeout(struct js320_cal_s * self) { (void) self; }
bool js320_cal_handle_cmd(struct js320_cal_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    (void) self; (void) subtopic; (void) value;
    return false;
}
bool js320_cal_handle_publish(struct js320_cal_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
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
#include "../../../src/devices/js320/js320_drv.c"
#include "../../../src/devices/js320/js320_params.c"


// --- Test fixtures ---

static struct js320_drv_s * make_drv(void) {
    struct jsdrvp_mb_drv_s * drv = NULL;
    int32_t rc = js320_drv_factory(&drv);
    assert_int_equal(0, rc);
    return (struct js320_drv_s *) drv;
}

static int test_setup(void ** state) {
    capture_reset();
    // Pre-populate the test fixture's time_map so
    // jsdrvp_mb_dev_time_map() returns non-NULL during the test.
    // In production, firmware publishes !map at open; in tests, we
    // seed these values directly since there's no backend.
    g_cap.time_map.offset_time = 0;
    g_cap.time_map.offset_counter = 0;
    g_cap.time_map.counter_rate = 16000000.0;  // JS320 native 16 MHz
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

// Enable a signal-family channel (5=i, 6=v, 7=p) so that subsequent dwnN
// changes arm the drop-until-ack window.  Without this or a recent frame,
// dwnN changes are a no-op on the ack state (see js320_ack_begin).
static void enable_signal_stream(struct js320_drv_s * self) {
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
}

// Mark i, v and p enabled without forwarding ctrl to the device: frames
// for a disabled i/v/p port are dropped as the tail of a stopped stream.
static void enable_ivp_ports(struct js320_drv_s * self) {
    for (uint8_t ch = 5U; ch <= 7U; ++ch) {
        self->ports[ch].enabled = true;
    }
}

// Enable a gpi-family channel so that subsequent gpi dwnN changes arm the
// drop-until-ack window.
static void enable_gpi_stream(struct js320_drv_s * self) {
    self->ports[8].enabled = true;
}


// --- Tests: host metadata ---

static void test_on_open_publishes_param_meta(void ** state) {
    struct js320_drv_s * self = *state;
    struct mb_link_identity_s identity = {.vendor_id = 0x1234, .product_id = 0x0003};
    self->drv.on_open(&self->drv, NULL, &identity);
    static const char * expect[] = {"h/fs$", "h/fp$", "h/i_scale$", "h/v_scale$"};
    assert_int_equal(JSDRV_ARRAY_SIZE(expect), g_cap.frontend_send_count);
    for (uint32_t i = 0; i < JSDRV_ARRAY_SIZE(expect); ++i) {
        assert_string_equal(expect[i], g_cap.frontend_sends[i].topic);
        assert_int_equal(JSDRV_UNION_JSON, g_cap.frontend_sends[i].type);
        assert_ptr_equal(js320_params[i].meta, g_cap.frontend_sends[i].str);
    }
}


// The 'h' replay completion publishes the effective h/* values, so the
// host reports them even when it never set them.
static void test_open_publishes_host_values(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(2000));
    g_cap.frontend_send_count = 0;
    self->drv.on_host_replayed(&self->drv, NULL);
    static const char * expect[] = {"h/fs", "h/fp", "h/i_scale", "h/v_scale"};
    assert_int_equal(JSDRV_ARRAY_SIZE(expect), g_cap.frontend_send_count);
    for (uint32_t i = 0; i < JSDRV_ARRAY_SIZE(expect); ++i) {
        assert_string_equal(expect[i], g_cap.frontend_sends[i].topic);
        assert_true(g_cap.frontend_sends[i].flags & JSDRV_UNION_FLAG_RETAIN);
    }
    assert_int_equal(2000, g_cap.frontend_sends[0].u32);
    assert_int_equal(20, g_cap.frontend_sends[1].u32);
    assert_int_equal(JSDRV_UNION_F32, g_cap.frontend_sends[2].type);
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


// --- Tests: dwnN register validation ---

static void test_dwn_n_reject_too_large(void ** state) {
    // register max is 1000; larger values were silently forwarded and
    // produced a rate the host reported but the gateware cannot deliver
    struct js320_drv_s * self = *state;
    uint32_t n_prev = self->signal_dwn_n;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(5000));
    assert_true(handled);
    assert_int_equal(n_prev, self->signal_dwn_n);
    assert_int_equal(0, count_publishes("s/dwnN/N"));
    assert_int_equal(1, g_cap.return_code_count);
    assert_int_not_equal(0, g_cap.return_codes[0].rc);
}

static void test_dwn_n_accept_max(void ** state) {
    struct js320_drv_s * self = *state;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(1000));
    assert_true(handled);
    assert_int_equal(1000, self->signal_dwn_n);
    assert_int_equal(1, count_publishes("s/dwnN/N"));
    assert_int_equal(0, g_cap.return_codes[0].rc);
}

static void test_gpi_dwn_mode_reject_invalid(void ** state) {
    struct js320_drv_s * self = *state;
    uint32_t mode_prev = self->gpi_dwn_mode;
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/mode", &jsdrv_union_u32_r(4));
    assert_true(handled);
    assert_int_equal(mode_prev, self->gpi_dwn_mode);
    assert_int_equal(0, count_publishes("s/gpi/+/dwnN/mode"));
    assert_int_equal(1, g_cap.return_code_count);
    assert_int_not_equal(0, g_cap.return_codes[0].rc);
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
    // 250 kHz (factor 4) is a valid rate; 500 kHz (factor 2) is now rejected.
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(250000));
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

    // Drop to 250 kHz (factor 4; 500 kHz/factor 2 is now rejected): device
    // must take over power streaming.
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(250000));
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

// Stream data can arrive after on_close (the device is still flushing),
// which starts a new accumulator.  finalize must free it without a dev.
// The leak only shows under LeakSanitizer (CI sanitizers job).
static void test_finalize_frees_data_after_close(void ** state) {
    struct js320_drv_s * self = *state;
    struct mb_link_identity_s identity = {.vendor_id = 0x1234, .product_id = 0x0003};
    float samples[10] = {0};
    self->drv.on_open(&self->drv, NULL, &identity);
    self->drv.on_close(&self->drv, NULL);
    self->ports[5].enabled = true;  // a frame for a disabled port is dropped
    push_current_frame(self, 0ULL, samples, 10);
    assert_non_null(self->ports[5].msg_in);
    self->drv.finalize(&self->drv);
    *state = NULL;
}

static void test_frame_combining_rate_budget(void ** state) {
    struct js320_drv_s * self = *state;
    // publish_rate=20 -> element_count_max=50000 (1 MHz / 20).  Pick a
    // smaller publish_rate so the test fits in a few calls.
    self->drv.handle_cmd(&self->drv, NULL, "h/fp", &jsdrv_union_u32_r(20000));
    g_cap.return_code_count = 0;
    enable_ivp_ports(self);
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
    enable_ivp_ports(self);

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
    enable_signal_stream(self);
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
    // 3 is forced to factor 4 (gateware clamps 2 & 3 to 4) -> runtime = 64.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(3));
    assert_int_equal(JS320_DECIMATE * 4, js320_runtime_decimate(self, 5));
    // 2 is forced to factor 4 -> runtime = 64.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(2));
    assert_int_equal(JS320_DECIMATE * 4, js320_runtime_decimate(self, 5));
}

static void test_dwnN_gpi_mode_off(void ** state) {
    struct js320_drv_s * self = *state;
    enable_gpi_stream(self);
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
    enable_gpi_stream(self);
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

static void test_fs_to_decimation_mapping(void ** state) {
    (void) state;
    uint32_t n = 99U;
    uint32_t r = 99U;
    assert_int_equal(0, js320_fs_to_decimation(1000000U, &n, &r));
    assert_int_equal(0U, n);
    assert_int_equal(1U, r);
    // fs=500000 needs factor 2, which gateware forces to 4: reject to avoid
    // a silent rate mismatch.
    assert_int_not_equal(0, js320_fs_to_decimation(500000U, &n, &r));
    assert_int_equal(0, js320_fs_to_decimation(250000U, &n, &r));
    assert_int_equal(4U, n);
    assert_int_equal(1U, r);
    assert_int_equal(0, js320_fs_to_decimation(1000U, &n, &r));
    assert_int_equal(1000U, n);
    assert_int_equal(1U, r);
    // Below 1 kHz: instrument pinned to 1 kHz, residual factor on host.
    assert_int_equal(0, js320_fs_to_decimation(500U, &n, &r));
    assert_int_equal(1000U, n);
    assert_int_equal(2U, r);
    assert_int_equal(0, js320_fs_to_decimation(100U, &n, &r));
    assert_int_equal(1000U, n);
    assert_int_equal(10U, r);
    assert_int_equal(0, js320_fs_to_decimation(1U, &n, &r));
    assert_int_equal(1000U, n);
    assert_int_equal(1000U, r);
    // Sub-1 kHz rates that do not divide 1 kHz evenly are rejected.
    assert_int_not_equal(0, js320_fs_to_decimation(3U, &n, &r));
    assert_int_not_equal(0, js320_fs_to_decimation(300U, &n, &r));
    // fs=333333 doesn't divide 1 MHz evenly.
    assert_int_not_equal(0, js320_fs_to_decimation(333333U, &n, &r));
    // Factor 2 & 3 rejected (gateware forces them to factor 4).
    assert_int_not_equal(0, js320_fs_to_decimation(1000000U / 3U, &n, &r));
    // fs > 1 MHz rejected.
    assert_int_not_equal(0, js320_fs_to_decimation(2000000U, &n, &r));
    // fs=0 rejected.
    assert_int_not_equal(0, js320_fs_to_decimation(0U, &n, &r));
}

static void test_hfs_unified_forwards_dwnN(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
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

// --- Tests: sub-1 kHz h/fs host decimation ---

static void test_hfs_sub1khz_configures_host_filters(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(100));
    assert_true(handled);
    assert_int_equal(0, g_cap.return_codes[g_cap.return_code_count - 1].rc);
    assert_int_equal(100, self->fs);
    assert_int_equal(10, self->signal_host_factor);
    // The instrument is pinned to its 1 kHz floor.
    const struct device_publish_s * p = find_publish("s/dwnN/N");
    assert_non_null(p);
    assert_int_equal(1000, p->value_u32);
    // Device step 16*1000 native ticks; combined includes the host factor.
    assert_int_equal(16000, js320_device_decimate(self, 5));
    assert_int_equal(160000, js320_runtime_decimate(self, 5));
    // Filters allocated for all of i/v/p with the default sinc1 mode.
    for (uint8_t ch = 5U; ch <= 7U; ++ch) {
        assert_non_null(self->ports[ch].host_filter);
        assert_int_equal(10, jsdrv_downsample_sinc_decimate_factor(self->ports[ch].host_filter));
        assert_int_equal(1, jsdrv_downsample_sinc_order(self->ports[ch].host_filter));
    }
    assert_true(self->signal_ack.dropping);
}

static void test_hfs_sub1khz_stream_decimates(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(100));  // R=10
    push_dwnN_ack(self, 160000ULL);
    g_cap.backend_send_count = 0;

    float samples[10];
    for (uint32_t k = 0; k < 10; ++k) { samples[k] = 2.0f; }
    // First frame at native id 160000: input tick 10, aligned (10 % 10 == 0).
    // 10 device samples = one full host block -> exactly 1 output sample.
    push_current_frame(self, 160000ULL, samples, 10);
    assert_non_null(self->ports[5].msg_in);
    struct jsdrv_stream_signal_s * sig =
        (struct jsdrv_stream_signal_s *) self->ports[5].msg_in->payload.bin;
    assert_int_equal(1, sig->element_count);
    assert_int_equal(16000000, sig->sample_rate);
    assert_int_equal(160000, sig->decimate_factor);
    // Output stamped at the window start (the frame's first sample).
    assert_int_equal(160000ULL, sig->sample_id);
    const float * out = (const float *)
        (self->ports[5].msg_in->payload.bin + JSDRV_STREAM_HEADER_SIZE);
    assert_true(out[0] == 2.0f);

    // Contiguous next frame: one more output, ids keep stepping by the
    // combined factor (no discontinuity flush).
    push_current_frame(self, 160000ULL + 10ULL * 16000ULL, samples, 10);
    assert_int_equal(2, sig->element_count);
    assert_int_equal(0, g_cap.backend_send_count);
}

static void test_hfs_sub1khz_gap_restarts_filter(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(100));  // R=10
    push_dwnN_ack(self, 160000ULL);
    g_cap.backend_send_count = 0;

    float samples[10];
    for (uint32_t k = 0; k < 10; ++k) { samples[k] = 1.0f; }
    push_current_frame(self, 160000ULL, samples, 10);
    assert_non_null(self->ports[5].msg_in);

    // Gap: jump ahead one extra frame.  The pending message flushes and
    // the filter realigns on the new (aligned) stream.
    push_current_frame(self, 160000ULL + 20ULL * 16000ULL, samples, 10);
    assert_int_equal(1, g_cap.backend_send_count);
    assert_non_null(self->ports[5].msg_in);
    struct jsdrv_stream_signal_s * sig =
        (struct jsdrv_stream_signal_s *) self->ports[5].msg_in->payload.bin;
    assert_int_equal(1, sig->element_count);
    assert_int_equal(160000ULL + 20ULL * 16000ULL, sig->sample_id);
}

static void test_hfs_sub1khz_mode_change_reallocates(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(10));  // R=100
    assert_non_null(self->ports[5].host_filter);
    assert_int_equal(1, jsdrv_downsample_sinc_order(self->ports[5].host_filter));
    assert_int_equal(10, self->fs);
    // sinc3: filters reallocate with the new order, same factor.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(3));
    assert_non_null(self->ports[6].host_filter);
    assert_int_equal(3, jsdrv_downsample_sinc_order(self->ports[6].host_filter));
    assert_int_equal(100, jsdrv_downsample_sinc_decimate_factor(self->ports[6].host_filter));
    assert_int_equal(10, self->fs);
    // Bypass: host decimation disabled along with the instrument's.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(0));
    assert_null(self->ports[5].host_filter);
    assert_int_equal(16, js320_runtime_decimate(self, 5));
    assert_int_equal(1000000, self->fs);
    // Leaving bypass restores the stored host factor at the new order.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(2));
    assert_non_null(self->ports[7].host_filter);
    assert_int_equal(2, jsdrv_downsample_sinc_order(self->ports[7].host_filter));
    assert_int_equal(10, self->fs);
    assert_int_equal(1600000, js320_runtime_decimate(self, 5));
}

static void test_dwnN_direct_write_clears_host_factor(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(100));
    assert_int_equal(10, self->signal_host_factor);
    assert_non_null(self->ports[5].host_filter);
    // Raw register write: passthrough semantics, host decimation off.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(100));
    assert_int_equal(1, self->signal_host_factor);
    assert_null(self->ports[5].host_filter);
    assert_int_equal(10000, self->fs);
    assert_int_equal(1600, js320_runtime_decimate(self, 5));
}

// --- Tests: s/dwnN/mode tracking ---

static void test_dwn_mode_tracked_and_forwarded(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    assert_int_equal(1, self->signal_dwn_mode);  // default sinc1
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(3));
    assert_true(handled);
    assert_int_equal(3, self->signal_dwn_mode);
    const struct device_publish_s * p = find_publish("s/dwnN/mode");
    assert_non_null(p);
    assert_int_equal(3, p->value_u32);
    // The firmware acks mode changes like N changes: drop window armed.
    assert_int_equal(1, self->signal_ack.acks_outstanding);
    assert_true(self->signal_ack.dropping);
    assert_int_equal(0, g_cap.return_codes[g_cap.return_code_count - 1].rc);
    // The ack releases the outstanding count.
    push_dwnN_ack(self, 500ULL);
    assert_int_equal(0, self->signal_ack.acks_outstanding);
}

static void test_dwn_mode_invalid_rejected(void ** state) {
    struct js320_drv_s * self = *state;
    uint32_t before = count_publishes("s/dwnN/mode");
    bool handled = self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(4));
    assert_true(handled);
    assert_int_equal(1, self->signal_dwn_mode);  // unchanged
    assert_int_equal(before, count_publishes("s/dwnN/mode"));
    assert_int_not_equal(0, g_cap.return_codes[g_cap.return_code_count - 1].rc);
    assert_false(self->signal_ack.dropping);
}

static void test_dwn_mode_bypass_semantics(void ** state) {
    struct js320_drv_s * self = *state;
    // N=1000 with the default sinc1 mode: 1 kHz output.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(1000));
    assert_int_equal(16000, js320_runtime_decimate(self, 5));
    assert_int_equal(1000, self->fs);
    // Bypass: the device streams 1 Msps regardless of N.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(0));
    assert_int_equal(16, js320_runtime_decimate(self, 5));
    assert_int_equal(1000000, self->fs);
    // Leaving bypass: N applies again.
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/mode", &jsdrv_union_u32_r(1));
    assert_int_equal(16000, js320_runtime_decimate(self, 5));
    assert_int_equal(1000, self->fs);
}

static void test_dwnN_drop_until_ack_single(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
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
    enable_signal_stream(self);
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
    enable_signal_stream(self);
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
    enable_signal_stream(self);
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
    enable_gpi_stream(self);
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
    enable_gpi_stream(self);
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
    enable_gpi_stream(self);
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
    enable_gpi_stream(self);
    // Start a GPI drop window.
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/mode",
        &jsdrv_union_u32_r(1));
    assert_true(self->gpi_ack.dropping);
    assert_false(self->signal_ack.dropping);

    // An i/v/p frame arrives during the GPI drop window: must be accepted.
    self->ports[5].enabled = true;
    float samples[4] = {0};
    push_current_frame(self, 42ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

static void test_gpi_dwnN_ack_timeout_resumes(void ** state) {
    struct js320_drv_s * self = *state;
    enable_gpi_stream(self);
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

// With no signal channel streaming and no recent frame, a dwnN change must
// forward to the device but must NOT arm the drop-until-ack window.  This is the
// settings-replay-before-stream-enable path seen on UI hot-plug.
static void test_dwnN_signal_skips_ack_when_idle(void ** state) {
    struct js320_drv_s * self = *state;
    // No enable_signal_stream() call -> ports[5..7].enabled == false.
    bool handled = self->drv.handle_cmd(&self->drv, NULL,
        "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_true(handled);
    assert_int_equal(4, self->signal_dwn_n);
    // Device still receives the new decimation value.
    const struct device_publish_s * p = find_publish("s/dwnN/N");
    assert_non_null(p);
    assert_int_equal(4, p->value_u32);
    // But the ack window stays idle: no outstanding ack, not dropping.
    assert_int_equal(0, self->signal_ack.acks_outstanding);
    assert_false(self->signal_ack.dropping);
}

// Same as above for the GPI family.
static void test_dwnN_gpi_skips_ack_when_idle(void ** state) {
    struct js320_drv_s * self = *state;
    // No enable_gpi_stream() call -> ports[8..12].enabled == false.
    bool handled = self->drv.handle_cmd(&self->drv, NULL,
        "s/gpi/+/dwnN/mode", &jsdrv_union_u32_r(1));
    assert_true(handled);
    assert_int_equal(1, self->gpi_dwn_mode);
    const struct device_publish_s * p = find_publish("s/gpi/+/dwnN/mode");
    assert_non_null(p);
    assert_int_equal(1, p->value_u32);
    assert_int_equal(0, self->gpi_ack.acks_outstanding);
    assert_false(self->gpi_ack.dropping);
}

// A dwnN change just after a stop must arm the drop window: frames sent
// before the stop are still in flight and would get the new rate.
static void test_dwnN_signal_drop_after_stop(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    enable_signal_stream(self);
    push_current_frame(self, 100ULL, samples, 4);
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(0));
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_int_equal(1, self->signal_ack.acks_outstanding);
    assert_true(self->signal_ack.dropping);

    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(1));
    push_dwnN_ack(self, 1000ULL);
    push_current_frame(self, 164ULL, samples, 4);  // in-flight old-rate tail
    assert_null(self->ports[5].msg_in);
    push_current_frame(self, 1000ULL, samples, 4);
    assert_false(self->signal_ack.dropping);
    assert_non_null(self->ports[5].msg_in);
}

// A tail frame after a close must not arm the dwnN window: the next
// open replays values the device may already hold, which it never acks.
static void test_dwnN_tail_after_close_no_window(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    enable_signal_stream(self);
    push_current_frame(self, 100ULL, samples, 4);
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(0));
    self->drv.on_close(&self->drv, NULL);
    push_current_frame(self, 164ULL, samples, 4);  // in-flight tail
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    assert_false(self->signal_ack.dropping);
}

// Same as above for the GPI family.
static void test_dwnN_gpi_drop_after_stop(void ** state) {
    struct js320_drv_s * self = *state;
    enable_gpi_stream(self);
    push_gpi_frame(self, 100ULL, 0);
    self->ports[8].enabled = false;
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N", &jsdrv_union_u32_r(32));
    assert_int_equal(1, self->gpi_ack.acks_outstanding);
    assert_true(self->gpi_ack.dropping);
}

// An ack that arrives with no frames after it must time out quietly.
static void test_dwnN_acked_idle_timeout_clears(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    self->drv.handle_cmd(&self->drv, NULL, "s/dwnN/N", &jsdrv_union_u32_r(4));
    push_dwnN_ack(self, 1000ULL);
    assert_true(self->signal_ack.dropping);
    self->signal_ack.drop_timeout_utc = jsdrv_time_utc() - 1;
    self->drv.on_timeout(&self->drv, NULL);
    assert_false(self->signal_ack.dropping);
    assert_int_equal(0, self->signal_ack.acks_outstanding);
}

// The device ignores a repeated dwnN value and sends no !ack, so a
// change of only the host factor must not wait for one.
static void test_dwnN_unchanged_device_value_no_window(void ** state) {
    struct js320_drv_s * self = *state;
    enable_signal_stream(self);
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(5));
    push_dwnN_ack(self, 100ULL);
    float samples[4] = {0};
    push_current_frame(self, 200ULL, samples, 4);
    assert_false(self->signal_ack.dropping);
    self->drv.handle_cmd(&self->drv, NULL, "h/fs", &jsdrv_union_u32_r(10));  // same N
    assert_int_equal(0, self->signal_ack.acks_outstanding);
    assert_false(self->signal_ack.dropping);
    enable_gpi_stream(self);
    self->drv.handle_cmd(&self->drv, NULL, "s/gpi/+/dwnN/N", &jsdrv_union_u32_r(self->gpi_dwn_n));
    assert_false(self->gpi_ack.dropping);
}

static void push_ctrl_ack(struct js320_drv_s * self, const char * topic, uint64_t sample_id) {
    struct jsdrv_union_s v = jsdrv_union_u64(sample_id);
    assert_true(self->drv.handle_publish(&self->drv, NULL, topic, &v));
}

static void set_i_ctrl(struct js320_drv_s * self, uint32_t value) {
    self->drv.handle_cmd(&self->drv, NULL, "s/i/ctrl", &jsdrv_union_u32_r(value));
}

// Frames that arrive after a stop are the old stream's tail: dropped.
static void test_disabled_port_drops_tail(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    set_i_ctrl(self, 1);
    set_i_ctrl(self, 0);
    push_current_frame(self, 100ULL, samples, 4);
    assert_null(self->ports[5].msg_in);
}

// Older firmware never publishes s/i/!ack: a restart must not wait for it.
static void test_ctrl_ack_unsupported_no_window(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    set_i_ctrl(self, 1);
    set_i_ctrl(self, 0);
    set_i_ctrl(self, 1);
    assert_false(self->ctrl_ack[0].dropping);
    push_current_frame(self, 100ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

// With the ack seen, a restart drops frames that end at or before it.
static void test_ctrl_ack_restart_drops_tail(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    uint64_t step = js320_device_decimate(self, 5);
    set_i_ctrl(self, 1);  // first enable after open: state unknown, no window
    assert_false(self->ctrl_ack[0].dropping);
    push_ctrl_ack(self, "s/i/!ack", 500ULL);
    assert_true(self->ctrl_ack_supported);
    set_i_ctrl(self, 0);
    push_ctrl_ack(self, "s/i/!ack", 600ULL);  // stop ack: no window to close
    set_i_ctrl(self, 1);
    assert_int_equal(1, self->ctrl_ack[0].acks_outstanding);
    assert_true(self->ctrl_ack[0].dropping);
    assert_false(self->ctrl_ack[1].dropping);  // v untouched

    push_current_frame(self, 100ULL, samples, 4);  // before the ack: dropped
    assert_null(self->ports[5].msg_in);
    push_ctrl_ack(self, "s/i/!ack", 1000ULL);
    push_current_frame(self, 1000ULL - 4 * step, samples, 4);  // ends at the ack
    assert_null(self->ports[5].msg_in);
    push_current_frame(self, 1000ULL - 3 * step, samples, 4);  // ends after it
    assert_false(self->ctrl_ack[0].dropping);
    assert_non_null(self->ports[5].msg_in);
}

// The open metadata declares s/i/!ack, so a restart within the first
// stream arms the window before any ack arrived.
static void test_ctrl_ack_supported_from_meta(void ** state) {
    struct js320_drv_s * self = *state;
    self->drv.on_topic_meta(&self->drv, NULL, "s/i/!ack", "{}");
    assert_true(self->ctrl_ack_supported);
    set_i_ctrl(self, 1);
    set_i_ctrl(self, 0);
    set_i_ctrl(self, 1);
    assert_true(self->ctrl_ack[0].dropping);
}

// Quick cycles: the window must not close on an early ack.  The first
// enable after open is uncounted, so the window closes on the ack for the
// last stop; nothing is committed between that stop and the enable.
static void test_ctrl_ack_quick_cycles(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    uint64_t step = js320_device_decimate(self, 5);
    self->drv.on_topic_meta(&self->drv, NULL, "s/i/!ack", "{}");
    set_i_ctrl(self, 1);  // unknown state: not counted
    set_i_ctrl(self, 0);  // ack 200
    set_i_ctrl(self, 1);  // ack 300
    set_i_ctrl(self, 0);  // ack 400
    set_i_ctrl(self, 1);  // ack 500
    assert_int_equal(4, self->ctrl_ack[0].acks_outstanding);
    push_ctrl_ack(self, "s/i/!ack", 100ULL);
    push_ctrl_ack(self, "s/i/!ack", 200ULL);
    push_ctrl_ack(self, "s/i/!ack", 300ULL);
    push_current_frame(self, 300ULL, samples, 4);  // second cycle: dropped
    assert_null(self->ports[5].msg_in);
    push_ctrl_ack(self, "s/i/!ack", 400ULL);
    push_current_frame(self, 400ULL - 4 * step, samples, 4);  // ends at the stop
    assert_null(self->ports[5].msg_in);
    push_ctrl_ack(self, "s/i/!ack", 500ULL);  // nothing outstanding: ignored
    push_current_frame(self, 500ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

// A lost ack must not block the stream past the timeout.
static void test_ctrl_ack_timeout_resumes(void ** state) {
    struct js320_drv_s * self = *state;
    float samples[4] = {0};
    set_i_ctrl(self, 1);
    push_ctrl_ack(self, "s/i/!ack", 500ULL);
    set_i_ctrl(self, 0);
    set_i_ctrl(self, 1);
    assert_true(self->ctrl_ack[0].dropping);
    self->ctrl_ack[0].drop_timeout_utc = jsdrv_time_utc() - 1;
    self->drv.on_timeout(&self->drv, NULL);
    assert_false(self->ctrl_ack[0].dropping);
    push_current_frame(self, 100ULL, samples, 4);
    assert_non_null(self->ports[5].msg_in);
}

// A stale signal !ack (no signal_ack window open) must NOT leak into or
// disturb the GPI window.
static void test_dwnN_signal_ack_independent_of_gpi(void ** state) {
    struct js320_drv_s * self = *state;
    enable_gpi_stream(self);
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
        cmocka_unit_test_setup_teardown(test_on_open_publishes_param_meta,   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_h_fp_default,                   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_h_fp_set,                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_h_fp_clamp_zero,                test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwn_n_reject_too_large,         test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwn_n_accept_max,               test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_gpi_dwn_mode_reject_invalid,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_enable_i_only,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_iv_no_compute,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_ivp_high_rate_host_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_ivp_low_rate_device_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_fs_transition_clears_compute, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_smart_power_no_double_forward,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_frame_combining_rate_budget,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_finalize_frees_data_after_close, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_group_alignment_ivp,            test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_compute_power_correctness,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_tracked_and_forwarded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_passthrough_codes,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_mode_off,              test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_n_change,              test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_fs_to_decimation_mapping,       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_unified_forwards_dwnN,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_unified_rejects_invalid_rate, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_sub1khz_configures_host_filters, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_sub1khz_stream_decimates,   test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_sub1khz_gap_restarts_filter, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_hfs_sub1khz_mode_change_reallocates, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_direct_write_clears_host_factor, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwn_mode_tracked_and_forwarded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwn_mode_invalid_rejected,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwn_mode_bypass_semantics,      test_setup, test_teardown),
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
        cmocka_unit_test_setup_teardown(test_dwnN_signal_skips_ack_when_idle, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_skips_ack_when_idle,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_signal_drop_after_stop,     test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_tail_after_close_no_window, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_open_publishes_host_values,      test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_unchanged_device_value_no_window, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_disabled_port_drops_tail,        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_ack_unsupported_no_window,  test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_ack_restart_drops_tail,     test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_ack_timeout_resumes,        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_ack_supported_from_meta,    test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_ack_quick_cycles,           test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_gpi_drop_after_stop,        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_dwnN_acked_idle_timeout_clears,  test_setup, test_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
