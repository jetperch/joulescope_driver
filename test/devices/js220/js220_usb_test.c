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

// js220_usb unit tests.  These exercise the upper-level JS220 device
// (memory operations and host-side parameters) by including the source
// file directly, following the mb_device_test pattern.  The test links
// jsdrv_support_objlib for cstr/topic/union/downsample/stats and the REAL
// msg_queue, so frames the device sends to the instrument are captured by
// popping d->ll.cmd_q.  Frames from the instrument are injected with
// handle_stream_in_frame().  jsdrv_alloc and jsdrv_free are renamed in
// the device source to count the memory buffer allocations.

// Define before any jsdrv header so jsdrv_prv/log.h does not apply its
// default first; matches js220_usb.c's own definition exactly.
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
#include "jsdrv/topic.h"
#include "jsdrv/union.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/platform.h"

// --- Counted allocator: rename the calls in js220_usb.c only ---

void * js220_usb_test_alloc(size_t size_bytes);
void js220_usb_test_free(void * ptr);
#define jsdrv_alloc js220_usb_test_alloc
#define jsdrv_free js220_usb_test_free
#include "../../../src/devices/js220/js220_usb.c"
#undef jsdrv_alloc
#undef jsdrv_free

static int32_t alloc_count_;
static int32_t free_count_;

void * js220_usb_test_alloc(size_t size_bytes) {
    ++alloc_count_;
    return jsdrv_alloc(size_bytes);
}

void js220_usb_test_free(void * ptr) {
    ++free_count_;
    jsdrv_free(ptr);
}

#define PREFIX "u/js220/test"
#define BACKEND_MSG_MAX (64)

// --- Stubs (frontend-side services not linked from jsdrv.c) ---

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_bin(m->payload.bin, 0);
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

// Messages the device sends to the frontend, kept until the next setup.
static struct jsdrvp_msg_s * backend_msg_[BACKEND_MSG_MAX];
static uint32_t backend_msg_count_;

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    if (backend_msg_count_ < BACKEND_MSG_MAX) {
        backend_msg_[backend_msg_count_++] = msg;
    } else {
        jsdrvp_msg_free(context, msg);
    }
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

static void backend_clear(void) {
    for (uint32_t i = 0; i < backend_msg_count_; ++i) {
        jsdrvp_msg_free(NULL, backend_msg_[i]);
        backend_msg_[i] = NULL;
    }
    backend_msg_count_ = 0;
}

// Find the last message the device sent to the frontend on
// "{PREFIX}/{subtopic}{suffix}", where suffix 0 means no suffix.
static struct jsdrvp_msg_s * backend_find_suffix(const char * subtopic, char suffix) {
    const size_t prefix_sz = strlen(PREFIX);
    const size_t subtopic_sz = strlen(subtopic);
    for (uint32_t i = backend_msg_count_; i > 0; --i) {
        const char * t = backend_msg_[i - 1]->topic;
        if ((0 == strncmp(t, PREFIX, prefix_sz)) && (t[prefix_sz] == '/')
                && (0 == strncmp(t + prefix_sz + 1, subtopic, subtopic_sz))
                && (t[prefix_sz + 1 + subtopic_sz] == suffix)
                && ((0 == suffix) || (0 == t[prefix_sz + 2 + subtopic_sz]))) {
            return backend_msg_[i - 1];
        }
    }
    return NULL;
}

static struct jsdrvp_msg_s * backend_find(const char * subtopic) {
    return backend_find_suffix(subtopic, 0);
}

static int32_t backend_return_code(const char * subtopic) {
    struct jsdrvp_msg_s * m = backend_find_suffix(subtopic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    if (NULL == m) {
        fail_msg("return code %s%c not sent", subtopic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    assert_int_equal(JSDRV_UNION_I32, m->value.type);
    return m->value.value.i32;
}

// --- Device fixture ---

static int setup(void ** state) {
    struct dev_s * d = jsdrv_alloc_clr(sizeof(struct dev_s));
    jsdrv_cstr_copy(d->ll.prefix, PREFIX, sizeof(d->ll.prefix));
    d->ll.cmd_q = msg_queue_init();
    d->ll.rsp_q = msg_queue_init();
    d->publish_rate = PUB_RATE_DEFAULT;
    d->i_scale = 1.0f;
    d->v_scale = 1.0f;
    on_sampling_frequency(d, &jsdrv_union_u32_r(SAMPLING_FREQUENCY));
    d->state = ST_OPEN;
    d->stream_in_port_enable = 0x000f;  // ports 0, 1, 2, 3, as d_open
    alloc_count_ = 0;
    free_count_ = 0;
    backend_clear();
    *state = d;
    return 0;
}

static void cmd_q_clear(struct dev_s * d) {
    struct jsdrvp_msg_s * m;
    while (NULL != (m = msg_queue_pop_immediate(d->ll.cmd_q))) {
        jsdrvp_msg_free(d->context, m);
    }
}

static int teardown(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    if (NULL != d->mem_data) {
        jsdrv_free(d->mem_data);
    }
    for (uint32_t idx = 0; idx < JSDRV_ARRAY_SIZE(d->ports); ++idx) {
        struct port_s * p = &d->ports[idx];
        if (NULL != p->msg_in) {
            jsdrvp_msg_free(d->context, p->msg_in);
        }
        if (NULL != p->downsample) {
            jsdrv_downsample_free(p->downsample);
        }
    }
    cmd_q_clear(d);
    msg_queue_finalize(d->ll.cmd_q, d->context);
    msg_queue_finalize(d->ll.rsp_q, d->context);
    jsdrv_free(d);
    backend_clear();
    return 0;
}

// --- Host -> device helpers ---

static void cmd_publish(struct dev_s * d, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, "", value);
    snprintf(m->topic, sizeof(m->topic), "%s/%s", PREFIX, subtopic);
    assert_true(handle_cmd(d, m));  // frees m
}

// Pop the next port 3 frame sent to the instrument.
static struct js220_port3_msg_s * port3_pop(struct dev_s * d, struct jsdrvp_msg_s ** msg) {
    struct jsdrvp_msg_s * m = msg_queue_pop_immediate(d->ll.cmd_q);
    assert_non_null(m);
    assert_string_equal(JSDRV_USBBK_MSG_BULK_OUT_DATA, m->topic);
    struct js220_port3_msg_s * p = (struct js220_port3_msg_s *) m->value.value.bin;
    assert_int_equal(3, p->frame_hdr.h.port_id);
    *msg = m;
    return p;
}

static void port3_expect(struct dev_s * d, uint8_t op, uint32_t offset, uint32_t length) {
    struct jsdrvp_msg_s * m = NULL;
    struct js220_port3_msg_s * p = port3_pop(d, &m);
    assert_int_equal(op, p->hdr.op);
    assert_int_equal(offset, p->hdr.offset);
    assert_int_equal(length, p->hdr.length);
    jsdrvp_msg_free(d->context, m);
}

// --- Instrument -> host helpers ---

static void frame_in(struct dev_s * d, uint8_t port_id, const void * payload, uint16_t length) {
    uint32_t frame[FRAME_SIZE_U32];
    memset(frame, 0, sizeof(frame));
    frame[0] = js220_frame_hdr_pack(d->in_frame_id, length, port_id);
    memcpy(&frame[1], payload, length);
    handle_stream_in_frame(d, frame);
}

static void port3_in(struct dev_s * d, uint8_t op, uint8_t arg, uint8_t status,
                     uint32_t offset, const uint8_t * data, uint32_t length) {
    struct js220_port3_msg_s msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.op = op;
    msg.hdr.arg = arg;
    msg.hdr.status = status;
    msg.hdr.offset = offset;
    msg.hdr.length = length;
    if (data) {
        memcpy(msg.data, data, length);
    }
    uint16_t sz = (uint16_t) (sizeof(msg.hdr) + (data ? length : 0));
    frame_in(d, 3, &msg.hdr, sz);
}

static void ack_in(struct dev_s * d, uint8_t op, uint8_t status, uint32_t offset) {
    port3_in(d, JS220_PORT3_OP_ACK, op, status, offset, NULL, 0);
}

static void assert_mem_idle(struct dev_s * d) {
    struct js220_port3_header_s zero;
    memset(&zero, 0, sizeof(zero));
    assert_memory_equal(&zero, &d->mem_hdr, sizeof(zero));
    assert_int_equal(0, d->mem_offset_valid);
    assert_int_equal(0, d->mem_offset_sent);
    assert_null(d->mem_data);
    assert_int_equal(0, d->mem_topic.length);
    assert_int_equal(0, d->mem_topic.topic[0]);
    assert_int_equal(alloc_count_, free_count_);
}

static void fill_pattern(uint8_t * data, uint32_t length, uint8_t seed) {
    for (uint32_t i = 0; i < length; ++i) {
        data[i] = (uint8_t) (seed + i * 7U);
    }
}

// --- Memory operation tests ---

static void test_mem_read(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[256];
    fill_pattern(data, sizeof(data), 3);

    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(sizeof(data)));
    assert_int_equal(1, alloc_count_);
    struct jsdrvp_msg_s * m = NULL;
    struct js220_port3_msg_s * p = port3_pop(d, &m);
    assert_int_equal(JS220_PORT3_OP_READ_REQ, p->hdr.op);
    assert_int_equal(JS220_PORT3_REGION_CTRL_PERSONALITY, p->hdr.region);
    assert_int_equal(sizeof(data), p->hdr.length);
    jsdrvp_msg_free(d->context, m);

    port3_in(d, JS220_PORT3_OP_READ_DATA, 0, 0, 0, data, sizeof(data));
    assert_int_equal(sizeof(data), d->mem_offset_valid);
    ack_in(d, JS220_PORT3_OP_READ_REQ, 0, 0);

    struct jsdrvp_msg_s * rdata = backend_find("h/mem/c/pers/!rdata");
    assert_non_null(rdata);
    assert_int_equal(JSDRV_UNION_BIN, rdata->value.type);
    assert_int_equal(sizeof(data), rdata->value.size);
    assert_memory_equal(data, rdata->value.value.bin, sizeof(data));
    assert_int_equal(0, backend_return_code("h/mem/c/pers/!read"));
    assert_mem_idle(d);  // fails with memset(sizeof(mem_topic)): mem_data leaks
}

static void test_mem_read_short(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[100];
    fill_pattern(data, sizeof(data), 11);

    cmd_publish(d, "h/mem/s/cal_a/!read", &jsdrv_union_u32(400));
    port3_expect(d, JS220_PORT3_OP_READ_REQ, 0, 400);
    port3_in(d, JS220_PORT3_OP_READ_DATA, 0, 0, 0, data, sizeof(data));
    ack_in(d, JS220_PORT3_OP_READ_REQ, 0, 0);

    struct jsdrvp_msg_s * rdata = backend_find("h/mem/s/cal_a/!rdata");
    assert_non_null(rdata);
    assert_int_equal(sizeof(data), rdata->value.size);  // truncated to the data received
    assert_memory_equal(data, rdata->value.value.bin, sizeof(data));
    assert_int_equal(0, backend_return_code("h/mem/s/cal_a/!read"));
    assert_mem_idle(d);
}

static void test_mem_read_error(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(256));
    port3_expect(d, JS220_PORT3_OP_READ_REQ, 0, 256);
    ack_in(d, JS220_PORT3_OP_READ_REQ, JSDRV_ERROR_IO, 0);
    assert_null(backend_find("h/mem/c/pers/!rdata"));
    assert_int_equal(JSDRV_ERROR_IO, backend_return_code("h/mem/c/pers/!read"));
    assert_mem_idle(d);
}

static void test_mem_read_sequence_error(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[64];
    fill_pattern(data, sizeof(data), 5);
    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(256));
    port3_expect(d, JS220_PORT3_OP_READ_REQ, 0, 256);
    port3_in(d, JS220_PORT3_OP_READ_DATA, 0, 0, 64, data, sizeof(data));  // skipped offset 0
    ack_in(d, JS220_PORT3_OP_READ_REQ, 0, 0);
    assert_null(backend_find("h/mem/c/pers/!rdata"));
    assert_int_equal(JSDRV_ERROR_SEQUENCE, backend_return_code("h/mem/c/pers/!read"));
    assert_mem_idle(d);
}

static void test_mem_write(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    const uint32_t chunk = JS220_PORT3_DATA_SIZE_MAX;
    uint8_t data[1000];
    fill_pattern(data, sizeof(data), 17);
    assert_true(sizeof(data) > 2 * chunk);  // three data frames

    cmd_publish(d, "h/mem/s/cal_a/!write", &jsdrv_union_bin(data, sizeof(data)));
    assert_int_equal(1, alloc_count_);
    struct jsdrvp_msg_s * m = NULL;
    struct js220_port3_msg_s * p = port3_pop(d, &m);
    assert_int_equal(JS220_PORT3_OP_WRITE_START, p->hdr.op);
    assert_int_equal(JS220_PORT3_REGION_SENSOR_CAL_ACTIVE, p->hdr.region);
    assert_int_equal(sizeof(data), p->hdr.length);
    jsdrvp_msg_free(d->context, m);
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));

    ack_in(d, JS220_PORT3_OP_WRITE_START, 0, 0);
    uint32_t offset = 0;
    while (offset < sizeof(data)) {
        uint32_t sz = sizeof(data) - offset;
        sz = (sz > chunk) ? chunk : sz;
        p = port3_pop(d, &m);
        assert_int_equal(JS220_PORT3_OP_WRITE_DATA, p->hdr.op);
        assert_int_equal(offset, p->hdr.offset);
        assert_int_equal(sz, p->hdr.length);
        assert_memory_equal(data + offset, p->data, sz);
        jsdrvp_msg_free(d->context, m);
        offset += sz;
    }
    port3_expect(d, JS220_PORT3_OP_NONE, 0, sizeof(data));  // write end
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));

    ack_in(d, JS220_PORT3_OP_WRITE_DATA, 0, sizeof(data));
    port3_expect(d, JS220_PORT3_OP_WRITE_FINALIZE, 0, sizeof(data));
    assert_null(backend_find("h/mem/s/cal_a/!write#"));

    ack_in(d, JS220_PORT3_OP_WRITE_FINALIZE, 0, 0);
    assert_int_equal(0, backend_return_code("h/mem/s/cal_a/!write"));
    assert_mem_idle(d);
}

static void test_mem_write_start_error(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[32];
    fill_pattern(data, sizeof(data), 1);
    cmd_publish(d, "h/mem/c/app/!write", &jsdrv_union_bin(data, sizeof(data)));
    port3_expect(d, JS220_PORT3_OP_WRITE_START, 0, sizeof(data));
    ack_in(d, JS220_PORT3_OP_WRITE_START, JSDRV_ERROR_IO, 0);
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));
    assert_int_equal(JSDRV_ERROR_IO, backend_return_code("h/mem/c/app/!write"));
    assert_mem_idle(d);
}

static void test_mem_write_ack_not_sent(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[32];
    fill_pattern(data, sizeof(data), 1);
    cmd_publish(d, "h/mem/c/app/!write", &jsdrv_union_bin(data, sizeof(data)));
    ack_in(d, JS220_PORT3_OP_WRITE_START, 0, 0);
    cmd_q_clear(d);
    ack_in(d, JS220_PORT3_OP_WRITE_DATA, 0, sizeof(data) + 4);
    assert_int_equal(JSDRV_ERROR_SEQUENCE, backend_return_code("h/mem/c/app/!write"));
    assert_mem_idle(d);
}

static void test_mem_write_too_big(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    uint8_t data[4] = {1, 2, 3, 4};
    struct jsdrv_union_s value = jsdrv_union_bin(data, MEM_SIZE_MAX + 1);  // size checked before use
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    snprintf(m->topic, sizeof(m->topic), "%s/%s", PREFIX, "h/mem/c/app/!write");
    m->value = value;
    assert_true(handle_cmd(d, m));
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));
    assert_int_equal(0, d->out_frame_id);
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/mem/c/app/!write"));
    assert_mem_idle(d);
}

static void test_mem_erase(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/storage/!erase", &jsdrv_union_u32(0));
    struct jsdrvp_msg_s * m = NULL;
    struct js220_port3_msg_s * p = port3_pop(d, &m);
    assert_int_equal(JS220_PORT3_OP_ERASE, p->hdr.op);
    assert_int_equal(JS220_PORT3_REGION_CTRL_STORAGE, p->hdr.region);
    jsdrvp_msg_free(d->context, m);
    ack_in(d, JS220_PORT3_OP_ERASE, 0, 0);
    assert_int_equal(0, backend_return_code("h/mem/c/storage/!erase"));
    assert_int_equal(0, alloc_count_);
    assert_mem_idle(d);
}

static void test_mem_erase_error(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/storage/!erase", &jsdrv_union_u32(0));
    port3_expect(d, JS220_PORT3_OP_ERASE, 0, 0);
    ack_in(d, JS220_PORT3_OP_ERASE, JSDRV_ERROR_BUSY, 0);
    assert_int_equal(JSDRV_ERROR_BUSY, backend_return_code("h/mem/c/storage/!erase"));
    assert_mem_idle(d);
}

static void test_mem_new_command_aborts_active(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(256));
    port3_expect(d, JS220_PORT3_OP_READ_REQ, 0, 256);
    assert_int_equal(1, alloc_count_);

    cmd_publish(d, "h/mem/c/storage/!erase", &jsdrv_union_u32(0));
    assert_int_equal(JSDRV_ERROR_ABORTED, backend_return_code("h/mem/c/pers/!read"));
    assert_int_equal(1, free_count_);
    assert_null(d->mem_data);
    port3_expect(d, JS220_PORT3_OP_ERASE, 0, 0);
    assert_int_equal(JS220_PORT3_OP_ERASE, d->mem_hdr.op);

    ack_in(d, JS220_PORT3_OP_ERASE, 0, 0);
    assert_int_equal(0, backend_return_code("h/mem/c/storage/!erase"));
    assert_mem_idle(d);
}

static void test_mem_unexpected_op_aborts(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(256));
    port3_expect(d, JS220_PORT3_OP_READ_REQ, 0, 256);
    ack_in(d, JS220_PORT3_OP_ERASE, 0, 0);  // ack for an op that is not active
    assert_int_equal(JSDRV_ERROR_ABORTED, backend_return_code("h/mem/c/pers/!read"));
    assert_mem_idle(d);
}

static void test_mem_invalid_region(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/bogus/!read", &jsdrv_union_u32(256));
    cmd_publish(d, "h/mem/x/pers/!read", &jsdrv_union_u32(256));
    cmd_publish(d, "h/mem/c/pers", &jsdrv_union_u32(256));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/mem/c/bogus/!read"));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/mem/x/pers/!read"));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/mem/c/pers"));
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));
    assert_mem_idle(d);
}

static void test_mem_invalid_op(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/mem/c/pers/!bogus", &jsdrv_union_u32(0));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/mem/c/pers/!bogus"));
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));
    assert_int_equal(0, d->out_frame_id);
    assert_mem_idle(d);
}

static void test_mem_closed(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    d->state = ST_CLOSED;
    cmd_publish(d, "h/mem/c/pers/!read", &jsdrv_union_u32(256));
    assert_int_equal(JSDRV_ERROR_CLOSED, backend_return_code("h/mem/c/pers/!read"));
    assert_null(msg_queue_pop_immediate(d->ll.cmd_q));
    assert_mem_idle(d);
}

// --- Host-side parameter tests ---

static void connect_in(struct dev_s * d, uint32_t fw_version, uint32_t fpga_version) {
    uint8_t buf[sizeof(struct js220_port0_header_s) + sizeof(struct js220_port0_connect_s)];
    memset(buf, 0, sizeof(buf));
    struct js220_port0_header_s * hdr = (struct js220_port0_header_s *) buf;
    struct js220_port0_connect_s * c = (struct js220_port0_connect_s *) &hdr[1];
    hdr->op = JS220_PORT0_OP_CONNECT;
    c->protocol_version = JSDRV_VERSION_ENCODE_U32(JS220_PROTOCOL_VERSION_MAJOR, 0, 0);
    c->fw_version = fw_version;
    c->hw_version = JSDRV_VERSION_ENCODE_U32(1, 0, 0);
    c->fpga_version = fpga_version;
    frame_in(d, 0, buf, (uint16_t) sizeof(buf));
}

static void test_fs(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/fs", &jsdrv_union_u32(0));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/fs"));
    cmd_publish(d, "h/fs", &jsdrv_union_u32(SAMPLING_FREQUENCY * 2));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/fs"));
    cmd_publish(d, "h/fs", &jsdrv_union_u32(3));  // not a divisor of 2 MHz
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/fs"));
    cmd_publish(d, "h/fs", &jsdrv_union_str("fast"));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/fs"));
    assert_int_equal(SAMPLING_FREQUENCY, d->fs);

    cmd_publish(d, "h/fs", &jsdrv_union_u32(1000));
    assert_int_equal(0, backend_return_code("h/fs"));
    assert_int_equal(1000, d->fs);
    assert_non_null(d->ports[PORT_ID_CURRENT & 0x0f].downsample);
}

static void test_fp(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    cmd_publish(d, "h/fp", &jsdrv_union_u32(0));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/fp"));
    assert_int_equal(PUB_RATE_DEFAULT, d->publish_rate);
    cmd_publish(d, "h/fp", &jsdrv_union_u32(100));
    assert_int_equal(0, backend_return_code("h/fp"));
    assert_int_equal(100, d->publish_rate);
}

static void test_filter_requires_fw_1_3(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    connect_in(d, JSDRV_VERSION_ENCODE_U32(1, 2, 9), JSDRV_VERSION_ENCODE_U32(1, 3, 0));
    assert_null(backend_find("h/filter$"));
    cmd_publish(d, "h/filter", &jsdrv_union_u32(DOWNSAMPLE_SINC1));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, backend_return_code("h/filter"));
    assert_int_equal(DOWNSAMPLE_WIDEBAND, d->signal_downsample_filter);
    cmd_publish(d, "h/filter", &jsdrv_union_u32(DOWNSAMPLE_WIDEBAND));
    assert_int_equal(0, backend_return_code("h/filter"));
}

static void test_filter(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    connect_in(d, JSDRV_VERSION_ENCODE_U32(1, 3, 0), JSDRV_VERSION_ENCODE_U32(1, 3, 0));
    assert_non_null(backend_find("h/filter$"));
    cmd_publish(d, "h/filter", &jsdrv_union_u32(2));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, backend_return_code("h/filter"));
    assert_int_equal(DOWNSAMPLE_WIDEBAND, d->signal_downsample_filter);
    cmd_publish(d, "h/filter", &jsdrv_union_u32(DOWNSAMPLE_SINC1));
    assert_int_equal(0, backend_return_code("h/filter"));
    assert_int_equal(DOWNSAMPLE_SINC1, d->signal_downsample_filter);
}

static void test_scale(void ** state) {
    struct dev_s * d = (struct dev_s *) *state;
    connect_in(d, JSDRV_VERSION_ENCODE_U32(1, 3, 0), JSDRV_VERSION_ENCODE_U32(1, 3, 0));
    struct jsdrvp_msg_s * meta = backend_find("h/i_scale$");
    assert_non_null(meta);
    assert_int_equal(JSDRV_UNION_JSON, meta->value.type);
    assert_non_null(backend_find("h/v_scale$"));
    struct jsdrvp_msg_s * value = backend_find("h/i_scale");
    assert_non_null(value);
    assert_int_equal(JSDRV_UNION_F32, value->value.type);

    cmd_publish(d, "h/i_scale", &jsdrv_union_f32(2.5f));
    assert_int_equal(0, backend_return_code("h/i_scale"));
    assert_float_equal(2.5f, d->i_scale, 0.0f);
    cmd_publish(d, "h/v_scale", &jsdrv_union_i32(3));
    assert_int_equal(0, backend_return_code("h/v_scale"));
    assert_float_equal(3.0f, d->v_scale, 0.0f);
    cmd_publish(d, "h/v_scale", &jsdrv_union_str("big"));
    assert_int_not_equal(0, backend_return_code("h/v_scale"));
    assert_float_equal(3.0f, d->v_scale, 0.0f);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test_setup_teardown(test_mem_read, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_read_short, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_read_error, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_read_sequence_error, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_write, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_write_start_error, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_write_ack_not_sent, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_write_too_big, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_erase, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_erase_error, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_new_command_aborts_active, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_unexpected_op_aborts, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_invalid_region, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_invalid_op, setup, teardown),
            cmocka_unit_test_setup_teardown(test_mem_closed, setup, teardown),
            cmocka_unit_test_setup_teardown(test_fs, setup, teardown),
            cmocka_unit_test_setup_teardown(test_fp, setup, teardown),
            cmocka_unit_test_setup_teardown(test_filter_requires_fw_1_3, setup, teardown),
            cmocka_unit_test_setup_teardown(test_filter, setup, teardown),
            cmocka_unit_test_setup_teardown(test_scale, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
