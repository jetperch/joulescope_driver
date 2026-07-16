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

// mb_device unit tests.  These exercise the static helpers in
// mb_device.c (open-restore !state SET chunking, link-silence
// supervision, revalidation ladder) by including the source file
// directly, following the js320_drv_test pattern.  The test links
// jsdrv_support_objlib for cstr/topic/union/meta/posix and the REAL
// msg_queue, so frames the device layer emits are captured by popping
// d->ll.cmd_q.  Time is controlled by renaming jsdrv_time_utc to a
// test-owned clock before including the source.

// Define before any jsdrv header so jsdrv_prv/log.h does not apply its
// default first; matches mb_device.c's own definition exactly, so the
// re-definition inside the included source is identical (no C4005).
#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jsdrv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv/union.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/platform.h"

// --- Mock clock: rename every jsdrv_time_utc call site in mb_device.c ---

int64_t mb_device_test_time_utc(void);
#define jsdrv_time_utc mb_device_test_time_utc
#include "../../../src/devices/mb_device/mb_device.c"
#undef jsdrv_time_utc

static int64_t time_now_;

int64_t mb_device_test_time_utc(void) {
    return time_now_;
}

// --- Stubs (frontend-side services not linked from jsdrv.c) ---

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context,
                                             const char * topic,
                                             const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    switch (value->type) {
        case JSDRV_UNION_STR:  // fall-through
        case JSDRV_UNION_JSON:
            jsdrv_cstr_copy(m->payload.str, value->value.str, sizeof(m->payload.str));
            m->value.value.str = m->payload.str;
            break;
        case JSDRV_UNION_BIN:
            if (value->size <= sizeof(m->payload.bin)) {
                memcpy(m->payload.bin, value->value.bin, value->size);
                m->value.value.bin = m->payload.bin;
            }
            break;
        default:
            break;
    }
    return m;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    (void) context;
    jsdrv_free(msg);
}

static uint32_t backend_send_count_;

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    ++backend_send_count_;
    jsdrvp_msg_free(context, msg);
}

void jsdrvp_send_finalize_msg(struct jsdrv_context_s * context, struct msg_queue_s * q,
                              const char * topic) {
    (void) context; (void) q; (void) topic;
}

void jsdrvp_device_subscribe(struct jsdrv_context_s * context, const char * dev_topic,
                             const char * topic, uint8_t flags) {
    (void) context; (void) dev_topic; (void) topic; (void) flags;
}

void jsdrvp_device_unsubscribe(struct jsdrv_context_s * context, const char * dev_topic,
                               const char * topic, uint8_t flags) {
    (void) context; (void) dev_topic; (void) topic; (void) flags;
}

// --- Device fixture ---

static struct jsdrvp_mb_dev_s * device_alloc(void) {
    struct jsdrvp_mb_dev_s * d = jsdrv_alloc_clr(sizeof(struct jsdrvp_mb_dev_s));
    jsdrv_cstr_copy(d->ll.prefix, "u/js320/test", sizeof(d->ll.prefix));
    d->ll.cmd_q = msg_queue_init();
    d->ll.rsp_q = msg_queue_init();
    d->keepalive_en = true;
    time_now_ = JSDRV_TIME_SECOND * 1000000;  // arbitrary nonzero epoch
    backend_send_count_ = 0;
    return d;
}

static void device_free(struct jsdrvp_mb_dev_s * d) {
    struct jsdrvp_msg_s * m;
    while (NULL != (m = msg_queue_pop_immediate(d->ll.cmd_q))) {
        jsdrvp_msg_free(d->context, m);
    }
    msg_queue_finalize(d->ll.cmd_q, d->context);
    msg_queue_finalize(d->ll.rsp_q, d->context);
    jsdrv_free(d);
}

// --- Frame inspection helpers (device -> host wire format) ---

static uint32_t frame_length_u32(const struct jsdrvp_msg_s * m) {
    return ((uint32_t) m->payload.bin[4]) + 1U;
}

static bool frame_is_data(const struct jsdrvp_msg_s * m) {
    return (m->payload.bin[3] >> 3) == MB_FRAME_FT_DATA;
}

static uint8_t frame_service_type(const struct jsdrvp_msg_s * m) {
    return m->payload.bin[7] >> 4;
}

static uint16_t frame_metadata(const struct jsdrvp_msg_s * m) {
    const uint16_t * u16 = (const uint16_t *) m->payload.bin;
    return u16[3] & 0x0fffU;
}

static bool frame_is_ctrl(const struct jsdrvp_msg_s * m, uint8_t ctrl) {
    return (m->value.size == 8)
        && (m->payload.bin[3] == (MB_FRAME_FT_CONTROL << 3))
        && (m->payload.bin[2] == ctrl);
}

// Returns true when the frame is an outer PUBLISH stdmsg to `topic`
// wrapping an inner STATE SET_CMD.  *inner and *inner_size are set to
// the inner STATE message bounds.
static bool frame_is_state_set(const struct jsdrvp_msg_s * m, const char * topic,
                               const uint8_t ** inner, uint32_t * inner_size) {
    if (!frame_is_data(m) || (frame_service_type(m) != MB_FRAME_ST_STDMSG)) {
        return false;
    }
    const uint8_t * p = &m->payload.bin[FRAME_HEADER_SIZE_U8];
    const struct mb_stdmsg_header_s * hdr = (const struct mb_stdmsg_header_s *) p;
    if (hdr->type != MB_STDMSG_PUBLISH) {
        return false;
    }
    if (0 != strcmp((const char *) (p + sizeof(*hdr)), topic)) {
        return false;
    }
    const uint8_t * value = p + sizeof(*hdr) + MB_TOPIC_SIZE_MAX;
    const struct mb_stdmsg_header_s * state_hdr = (const struct mb_stdmsg_header_s *) value;
    if (state_hdr->type != MB_STDMSG_STATE) {
        return false;
    }
    if (value[sizeof(*state_hdr) + 4] != MB_STDMSG_STATE_TYPE_SET_CMD) {
        return false;
    }
    *inner = value;
    *inner_size = (frame_length_u32(m) << 2) - (uint32_t) (sizeof(*hdr) + MB_TOPIC_SIZE_MAX);
    return true;
}

// Walk the inner STATE SET_CMD entries the same way the firmware's
// state_set_cmd does; returns the entry count and (optionally) records
// each entry topic into `topics`.
static uint32_t state_set_entry_walk(const uint8_t * inner, uint32_t inner_size,
                                     char topics[][MB_TOPIC_SIZE_MAX], uint32_t topic_offset) {
    uint32_t off = (uint32_t) sizeof(struct mb_stdmsg_header_s) + 8U;
    uint32_t count = 0;
    while ((off + 48U) <= inner_size) {
        const uint8_t * ent = inner + off;
        uint16_t value_size;
        memcpy(&value_size, ent + 34, 2);
        uint16_t padded = (uint16_t) ((value_size + 7u) & ~7u);
        if (padded < 8) { padded = 8; }
        uint32_t entry_size = 40u + padded;
        if ((off + entry_size) > inner_size) {
            break;
        }
        if (topics) {
            jsdrv_cstr_copy(topics[topic_offset + count], (const char *) ent, MB_TOPIC_SIZE_MAX);
        }
        ++count;
        off += entry_size;
    }
    return count;
}

// --- Test: !state SET chunking respects the post-wrap frame limit ---

static void test_state_set_chunking(void ** state) {
    (void) state;
    struct jsdrvp_mb_dev_s * d = device_alloc();
    struct state_fetch_s * sf = &d->state_fetch;
    sf->prefix_buf[0] = 's';
    sf->prefix_buf[1] = 0;
    sf->prefixes = sf->prefix_buf;

    static char sent_topics[OPEN_SET_ENTRY_MAX][MB_TOPIC_SIZE_MAX];
    const uint16_t entry_count = 59;  // matches the JS320 sensor instance
    sf->set_count = entry_count;
    sf->set_cursor = 0;
    for (uint16_t i = 0; i < entry_count; ++i) {
        struct open_set_entry_s * e = &sf->set_entries[i];
        snprintf(e->topic, sizeof(e->topic), "s/test/topic%02u", (unsigned) i);
        e->value_type = JSDRV_UNION_U32;
        e->value_size = 4;
        uint32_t v = i;
        memcpy(e->value, &v, sizeof(v));
    }

    state_set_send_next(d);
    assert_int_equal(entry_count, sf->set_cursor);

    uint32_t total_entries = 0;
    uint32_t set_frames = 0;
    struct jsdrvp_msg_s * m;
    while (NULL != (m = msg_queue_pop_immediate(d->ll.cmd_q))) {
        // EVERY emitted frame must respect the frame payload limit; an
        // oversized frame is silently dropped in production (this is
        // the pre-fix failure: 10-entry chunks -> 134 u32 > 125).
        assert_true(frame_length_u32(m) <= PAYLOAD_SIZE_MAX_U32);
        const uint8_t * inner = NULL;
        uint32_t inner_size = 0;
        if (frame_is_state_set(m, "s/./!state", &inner, &inner_size)) {
            ++set_frames;
            total_entries += state_set_entry_walk(inner, inner_size,
                                                  sent_topics, total_entries);
        }
        jsdrvp_msg_free(d->context, m);
    }

    // Every entry must actually reach the device, across however many
    // chunks it takes, with content intact.
    assert_true(set_frames >= 2);
    assert_int_equal(entry_count, total_entries);
    for (uint16_t i = 0; i < entry_count; ++i) {
        char expect[MB_TOPIC_SIZE_MAX];
        snprintf(expect, sizeof(expect), "s/test/topic%02u", (unsigned) i);
        assert_string_equal(expect, sent_topics[i]);
    }

    device_free(d);
}

// --- Link-silence supervision helpers ---

// Pop all pending ll.cmd_q messages, counting LINK PING frames and
// CONNECT_REQ control frames; frees everything.
static void drain_cmd_q(struct jsdrvp_mb_dev_s * d, uint32_t * pings, uint32_t * connect_reqs) {
    struct jsdrvp_msg_s * m;
    while (NULL != (m = msg_queue_pop_immediate(d->ll.cmd_q))) {
        if (frame_is_data(m) && (frame_service_type(m) == MB_FRAME_ST_LINK)
                && (frame_metadata(m) == MB_LINK_MSG_PING)) {
            if (pings) { ++*pings; }
        } else if (frame_is_ctrl(m, MB_FRAME_CTRL_CONNECT_REQ)) {
            if (connect_reqs) { ++*connect_reqs; }
        }
        jsdrvp_msg_free(d->context, m);
    }
}

// Inject a device->host link PONG through the real RX frame parser
// (also proves RX updates rx_utc).
static void inject_pong(struct jsdrvp_mb_dev_s * d) {
    uint32_t frame[FRAME_SIZE_U8 >> 2];
    memset(frame, 0, sizeof(frame));
    uint8_t * u8 = (uint8_t *) frame;
    uint16_t * u16 = (uint16_t *) frame;
    u8[0] = MB_FRAME_SOF1;
    u8[1] = MB_FRAME_SOF2;
    u16[1] = (uint16_t) ((MB_FRAME_FT_DATA << 11) | (d->in_frame_id & MB_FRAME_FRAME_ID_MAX));
    u8[4] = 0;  // length_u32 - 1
    u8[5] = mb_frame_length_check(0);
    u16[3] = (uint16_t) ((MB_LINK_MSG_PONG & 0x0fffU) | (MB_FRAME_ST_LINK << 12));
    frame[2] = REVALIDATE_PING_PAYLOAD;

    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));
    memcpy(m->payload.bin, frame, FRAME_SIZE_U8);
    m->value = jsdrv_union_bin(m->payload.bin, FRAME_SIZE_U8);
    handle_stream_in(d, m);
    jsdrvp_msg_free(d->context, m);
}

// Enter a synthetic open session at time_now_.
static void enter_open(struct jsdrvp_mb_dev_s * d) {
    d->state = ST_OPEN;
    d->rx_utc = time_now_;
    d->keepalive_utc = time_now_ + JSDRV_TIME_HOUR;  // keep-alive out of the way
    d->revalidate_remaining = 0;
    d->revalidate_utc = 0;
}

// --- Test: silence triggers revalidation; pong stands it down ---

static void test_silence_revalidate_pong(void ** state) {
    (void) state;
    struct jsdrvp_mb_dev_s * d = device_alloc();
    enter_open(d);
    uint32_t pings = 0, connect_reqs = 0;

    // Below the threshold: no revalidation.
    time_now_ += LINK_SILENCE_TIMEOUT;
    driver_thread_timed_work(d);
    assert_int_equal(0, d->revalidate_remaining);

    // Past the threshold: revalidation starts with a PING.
    time_now_ += JSDRV_TIME_MILLISECOND * 100;
    driver_thread_timed_work(d);
    assert_int_equal(REVALIDATE_PING_RETRIES, d->revalidate_remaining);
    drain_cmd_q(d, &pings, &connect_reqs);
    assert_int_equal(1, pings);
    assert_int_equal(0, connect_reqs);

    // The device answers: revalidation stands down, rx_utc refreshes,
    // and the supervision does not immediately re-trigger.
    inject_pong(d);
    assert_int_equal(0, d->revalidate_remaining);
    assert_int_equal(time_now_, d->rx_utc);
    driver_thread_timed_work(d);
    assert_int_equal(0, d->revalidate_remaining);
    assert_int_equal(ST_OPEN, d->state);

    device_free(d);
}

// --- Test: sustained silence escalates to a handshake replay ---

static void test_silence_escalates_to_replay(void ** state) {
    (void) state;
    struct jsdrvp_mb_dev_s * d = device_alloc();
    enter_open(d);
    d->out_frame_id = 123;  // must reset to 0 on replay
    uint32_t pings = 0, connect_reqs = 0;

    time_now_ += LINK_SILENCE_TIMEOUT + JSDRV_TIME_MILLISECOND * 100;
    driver_thread_timed_work(d);  // first ping
    for (uint32_t k = 0; k < REVALIDATE_PING_RETRIES; ++k) {
        time_now_ += REVALIDATE_PING_INTERVAL;
        driver_thread_timed_work(d);  // retries, then replay
    }
    drain_cmd_q(d, &pings, &connect_reqs);
    assert_int_equal(REVALIDATE_PING_RETRIES, pings);
    assert_int_equal(1, connect_reqs);
    assert_int_equal(ST_LINK_REQUEST, d->state);
    assert_int_equal(0, d->out_frame_id);      // ids restart with the device
    assert_int_equal(0, d->revalidate_remaining);

    device_free(d);
}

// --- Test: h/link/keep=0 disables the supervision with the keep-alive ---

static void test_silence_disabled_with_keepalive(void ** state) {
    (void) state;
    struct jsdrvp_mb_dev_s * d = device_alloc();
    enter_open(d);
    d->keepalive_en = false;

    time_now_ += 10 * LINK_SILENCE_TIMEOUT;
    driver_thread_timed_work(d);
    assert_int_equal(0, d->revalidate_remaining);
    uint32_t pings = 0, connect_reqs = 0;
    drain_cmd_q(d, &pings, &connect_reqs);
    assert_int_equal(0, pings);
    assert_int_equal(0, connect_reqs);
    assert_int_equal(ST_OPEN, d->state);

    device_free(d);
}

// --- Test: supervision only runs in ST_OPEN ---

static void test_silence_only_in_open(void ** state) {
    (void) state;
    struct jsdrvp_mb_dev_s * d = device_alloc();
    enter_open(d);
    d->state = ST_LINK_REQUEST;

    time_now_ += 10 * LINK_SILENCE_TIMEOUT;
    driver_thread_timed_work(d);
    assert_int_equal(0, d->revalidate_remaining);

    device_free(d);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_state_set_chunking),
            cmocka_unit_test(test_silence_revalidate_pong),
            cmocka_unit_test(test_silence_escalates_to_replay),
            cmocka_unit_test(test_silence_disabled_with_keepalive),
            cmocka_unit_test(test_silence_only_in_open),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
