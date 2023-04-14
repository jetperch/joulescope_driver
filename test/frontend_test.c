/*
 * Copyright 2022 Jetperch LLC
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <inttypes.h>
#include "jsdrv.h"
#include "jsdrv/log.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/assert.h"
#include "js220_api.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include <stdio.h>

#define DEVICE_PREFIX "t/js220/123456"
#define SUB_TIMEOUT_MS   (2000)
#define DEV_TIMEOUT_MS   (2000)

struct test_s {
    struct jsdrv_context_s * context;
    struct msg_queue_s * sub_msgs;
    struct jsdrvbk_s backend;
    struct jsdrvp_ll_device_s ll_dev1;
    jsdrv_thread_t thread;
    volatile uint32_t thread_quit;
};

struct test_s self_;

#define SETUP() \
    memset(&self_, 0, sizeof(self_));                                                       \
    struct test_s * self = &self_;                                                          \
    *state = self;                                                                          \
    self->sub_msgs = msg_queue_init();                                                      \
    jsdrv_cstr_copy(self->ll_dev1.prefix, DEVICE_PREFIX, sizeof(self->ll_dev1.prefix));     \
    self->ll_dev1.cmd_q = msg_queue_init();                                                 \
    self->ll_dev1.rsp_q = msg_queue_init();                                                 \
    assert_int_equal(0, jsdrv_initialize(&self->context, NULL, 1000));                      \
    assert_int_equal(0, jsdrv_subscribe(self->context, "@", JSDRV_SFLAG_PUB, subscribe_cmd_fn, self, 1000))


#define TEARDOWN() \
    assert_int_equal(0, jsdrv_unsubscribe(self->context, "@", subscribe_cmd_fn, self, 1000)); \
    jsdrv_finalize(self->context, 1000);        \
    msg_queue_finalize(self->sub_msgs);         \
    msg_queue_finalize(self->ll_dev1.cmd_q);    \
    msg_queue_finalize(self->ll_dev1.rsp_q);    \
    memset(&self_, 0, sizeof(self_))

#if 0
/*expect_function_call(bk_finalize);*/  \
#endif


static void subscribe_cmd_fn(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    char value_str[128];
    struct test_s * t = (struct test_s *) user_data;
    if (jsdrv_cstr_ends_with(topic, "/!data")) {
        return;  // handled separately
    }
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(t->context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    JSDRV_ASSERT(value->type < 16);
    jsdrv_union_value_to_str(value, value_str, sizeof(value_str), 1);
    printf("subscribe_cmd_fn(%s) %s\n", topic, value_str);
    switch (value->type) {
        case JSDRV_UNION_JSON:  // intentional fall-through
        case JSDRV_UNION_STR:
            jsdrv_cstr_copy(m->payload.str, value->value.str, sizeof(m->payload.str));
            m->value.value.str = m->payload.str;
            break;
        case JSDRV_UNION_BIN:
            jsdrv_memcpy(m->payload.bin, value->value.bin, value->size);
            m->value.value.bin = m->payload.bin;
            break;
        default:
            break;
    }
    msg_queue_push(t->sub_msgs, m);
}

#if 0
#define expect_subscribe_str(parameter_, str_) do {                         \
    struct jsdrvp_msg_s * m;                                                \
    assert_int_equal(0, msg_queue_pop(self_.sub_msgs, &m, SUB_TIMEOUT_MS)); \
    assert_string_equal(parameter_, msg->name);                             \
    assert_int_equal(JSDRV_UNION_STR, msg->value.type);                       \
    assert_string_equal(str_, msg->value.value.str);                        \
} while (0)
#else
static void expect_subscribe_cmd(struct test_s * t, const char * parameter_, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(t->sub_msgs, &msg, SUB_TIMEOUT_MS));
    assert_string_equal(parameter_, msg->topic);
    if (NULL != value) {
        assert_int_equal(value->type, msg->value.type);
        assert_true(jsdrv_union_eq(value, &msg->value));
    }
    jsdrvp_msg_free(t->context, msg);
}

//static void expect_subscribe_cmd_str(struct test_s * t_, const char * parameter_, const char * str_) {
#define expect_subscribe_cmd_str(t_, parameter_, str_) do { \
    struct test_s * t = (t_); \
    struct jsdrvp_msg_s * msg;                              \
    assert_int_equal(0, msg_queue_pop(t->sub_msgs, &msg, SUB_TIMEOUT_MS)); \
    assert_string_equal((parameter_), msg->topic); \
    assert_int_equal(JSDRV_UNION_STR, msg->value.type); \
    assert_string_equal((str_), msg->value.value.str); \
    jsdrvp_msg_free(t->context, msg); \
} while (0)

static void expect_subscribe_cmd_json(struct test_s * t, const char * parameter_, const char * str_) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(t->sub_msgs, &msg, SUB_TIMEOUT_MS));
    assert_string_equal(parameter_, msg->topic);
    assert_int_equal(JSDRV_UNION_JSON, msg->value.type);
    if (str_) {
        assert_string_equal(str_, msg->value.value.str);
    }
    jsdrvp_msg_free(t->context, msg);
}

#if 0
static void expect_subscribe_cmd_bin(struct test_s * t, const char * parameter_, void * data, uint32_t sz) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(t->sub_msgs, &msg, SUB_TIMEOUT_MS));
    assert_string_equal(parameter_, msg->topic);
    assert_int_equal(JSDRV_UNION_BIN, msg->value.type);
    assert_int_equal(sz, msg->value.size);
    if (data) {
        assert_memory_equal(data, msg->value.value.bin, sz);
    }
    jsdrvp_msg_free(t->context, msg);
}
#endif
#endif

static void bk_finalize(struct jsdrvbk_s * backend) {
    msg_queue_finalize(backend->cmd_q);
    // function_called();
}

#if 0
static void dev_expect_bin_ptr(struct jsdrvp_ll_device_s * d, const char * parameter_, uint8_t * p_expect, uint32_t sz_expect) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(d->cmd_q, &msg, DEV_TIMEOUT_MS));
    assert_string_equal(parameter_, msg->topic);
    assert_int_equal(JSDRV_UNION_BIN, msg->value.type);
    assert_ptr_equal(p_expect, msg->value.value.bin);
    assert_int_equal(sz_expect, msg->value.size);
    jsdrvp_msg_free(self_.context, msg);
}

static void dev_expect_i32(struct jsdrvp_ll_device_s * d, const char * parameter_, int32_t v_expect) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(d->cmd_q, &msg, DEV_TIMEOUT_MS));
    assert_string_equal(parameter_, msg->topic);
    assert_int_equal(JSDRV_UNION_I32, msg->value.type);
    assert_int_equal(v_expect, msg->value.value.i32);
    jsdrvp_msg_free(self_.context, msg);
}

static void dev_send_i32(struct test_s * t, struct jsdrvp_ll_device_s * d, const char * parameter_, int32_t v_expect) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_value(t->context, parameter_, &jsdrv_union_i32(v_expect));
    msg_queue_push(d->rsp_q, msg);
}

static void dev_expect_ctrl_in(struct jsdrvp_ll_device_s * d, const void * rsp, uint16_t sz) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(d->cmd_q, &msg, DEV_TIMEOUT_MS));
    assert_string_equal(JSDRV_USBBK_MSG_CTRL_IN, msg->topic);
    assert_int_equal(JSDRV_UNION_BIN, msg->value.type);
    if (msg->extra.bkusb_ctrl.setup.s.wLength > sz) {
        msg->value.size = sz;
    } else {
        msg->value.size = msg->extra.bkusb_ctrl.setup.s.wLength;
    }
    memcpy(msg->payload.bin, rsp, msg->value.size);
    msg->extra.bkusb_ctrl.status = 0;
    msg_queue_push(d->rsp_q, msg);
}

static void dev_expect_ctrl_out(struct jsdrvp_ll_device_s * d, const void * expect, uint16_t sz) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(d->cmd_q, &msg, DEV_TIMEOUT_MS));
    assert_string_equal(JSDRV_USBBK_MSG_CTRL_OUT, msg->topic);
    assert_int_equal(JSDRV_UNION_BIN, msg->value.type);
    assert_int_equal(sz, msg->value.size);
    assert_memory_equal(expect, msg->payload.bin, sz);
    memcpy(msg->payload.bin, expect, sz);
    msg->extra.bkusb_ctrl.status = 0;
    msg_queue_push(d->rsp_q, msg);
}

static void dev_expect_bulk_out(struct jsdrvp_ll_device_s * d, uint16_t frame_id, const char * topic, const struct jsdrv_union_s * expect) {
    struct jsdrvp_msg_s * msg;
    assert_int_equal(0, msg_queue_pop(d->cmd_q, &msg, DEV_TIMEOUT_MS));
    assert_string_equal(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic);
    assert_int_equal(JSDRV_UNION_BIN, msg->value.type);
    assert_int_equal(4 + sizeof(struct js220_publish_s) + sizeof(union jsdrv_union_inner_u), msg->value.size);
    uint32_t * p32 = (uint32_t *) msg->value.value.bin;
    assert_int_equal(msg->value.size - 4, js220_frame_hdr_extract_length(*p32));
    assert_int_equal(1, js220_frame_hdr_extract_port_id(*p32));
    assert_int_equal(frame_id, js220_frame_hdr_extract_frame_id(*p32));
    struct js220_publish_s * p = (struct js220_publish_s *) (p32 + 1);
    assert_int_equal(expect->type, p->type);
    assert_string_equal(topic, p->topic);
    union jsdrv_union_inner_u * inner = (union jsdrv_union_inner_u *) (p + 1);
    struct jsdrv_union_s pv = {.type = p->type, .flags = p->flags, .value = *inner};
    assert_true(jsdrv_union_eq(expect, &pv));
    msg_queue_push(d->rsp_q, msg);
}
#endif

int32_t jsdrv_unittest_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend) {
    struct test_s * self = &self_;
    assert_ptr_equal(self->context, context);
    self->backend.prefix = 't';
    self->backend.finalize = bk_finalize;
    self->backend.cmd_q = msg_queue_init();
    *backend = &self->backend;

    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_value(context, JSDRV_MSG_INITIALIZE, &jsdrv_union_i32(0));
    msg->payload.str[0] = self->backend.prefix;
    jsdrvp_backend_send(context, msg);
    return 0;
}

#if 0
static void publish_to_dev(struct test_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    snprintf(topic, sizeof(topic), DEVICE_PREFIX "/%s", subtopic);
    assert_int_equal(0, jsdrv_publish(self->context, topic, value, 0));
    expect_subscribe_cmd(self, topic, value);
    dev_expect_bulk_out(&self->ll_dev1, 0, subtopic, value);
}

static void publish_from_dev(struct test_s * self, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(self->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));
    uint32_t * p32 = (uint32_t *) m->payload.bin;
    uint32_t sz = sizeof(value->value);
    if (jsdrv_union_is_type_ptr(value)) {
        sz = value->size;
        if (!sz && ((value->type == JSDRV_UNION_STR) || (value->type == JSDRV_UNION_JSON))) {
            sz = (uint32_t) strlen(value->value.str);
        }
    }

    p32[0] = js220_frame_hdr_pack(1, sizeof(struct js220_publish_s), 1);
    struct js220_publish_s * p = (struct js220_publish_s *) &p32[1];
    jsdrv_cstr_copy(p->topic, subtopic, sizeof(p->topic));
    p->type = value->type;
    p->flags = value->flags;
    p->op = value->op;
    p->app = value->app;
    if (jsdrv_union_is_type_ptr(value)) {
        memcpy(p->data, value->value.bin, sz);
    } else {
        memcpy(p->data, &value->value, sz);
    }
    m->extra.bkusb_stream.endpoint = JS220_USB_EP_BULK_IN;
    msg_queue_push(self->ll_dev1.rsp_q, m);

    dev_expect_bin_ptr(&self->ll_dev1, JSDRV_USBBK_MSG_STREAM_IN_DATA, m->payload.bin, m->value.size);
}
#endif

static void device1_add(struct test_s * self) {
    assert_int_equal(0, jsdrv_subscribe(self->context, DEVICE_PREFIX,
                                        JSDRV_SFLAG_PUB | JSDRV_SFLAG_RETAIN | JSDRV_SFLAG_RETURN_CODE | JSDRV_SFLAG_METADATA_RSP,
                                        subscribe_cmd_fn, self, 1000));

    struct jsdrvp_msg_s *msg = jsdrvp_msg_alloc(self->context);
    jsdrv_cstr_copy(msg->topic, JSDRV_MSG_DEVICE_ADD, sizeof(msg->topic));
    msg->value = jsdrv_union_bin((const uint8_t *) &msg->payload.device, sizeof(msg->payload.device));
    msg->value.app = JSDRV_PAYLOAD_TYPE_DEVICE;
    msg->payload.device = self->ll_dev1;
    jsdrvp_backend_send(self->context, msg);
    expect_subscribe_cmd_str(self, JSDRV_MSG_DEVICE_ADD, DEVICE_PREFIX);
    expect_subscribe_cmd_str(self, JSDRV_MSG_DEVICE_LIST, DEVICE_PREFIX);

    expect_subscribe_cmd_json(self, DEVICE_PREFIX "/h/state$", NULL);
    expect_subscribe_cmd(self, DEVICE_PREFIX "/h/state", &jsdrv_union_u32_r(1));  // closed
}

static void device1_remove(struct test_s * self) {
    assert_int_equal(0, jsdrv_unsubscribe(self->context, DEVICE_PREFIX,
                                          subscribe_cmd_fn, self, 1000));

    struct jsdrvp_msg_s *msg = jsdrvp_msg_alloc(self->context);
    jsdrv_cstr_copy(msg->topic, JSDRV_MSG_DEVICE_REMOVE, sizeof(msg->topic));
    msg->value = jsdrv_union_str(DEVICE_PREFIX);
    jsdrvp_backend_send(self->context, msg);

    expect_subscribe_cmd_str(self, JSDRV_MSG_DEVICE_REMOVE, DEVICE_PREFIX);
    expect_subscribe_cmd_str(self, JSDRV_MSG_DEVICE_LIST, "");
}

#if 0
static void device1_open(struct test_s * self) {
    assert_int_equal(0, jsdrv_publish(self->context, DEVICE_PREFIX "/" JSDRV_MSG_OPEN, &jsdrv_union_i32(0), 0));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/" JSDRV_MSG_OPEN, &jsdrv_union_i32(0));

    dev_expect_i32(&self->ll_dev1, JSDRV_MSG_OPEN, 0);
    dev_send_i32(self, &self->ll_dev1, JSDRV_MSG_OPEN, 0);
    dev_expect_i32(&self->ll_dev1, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, 0);
    dev_send_i32(self, &self->ll_dev1, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, 0);

    uint8_t rsp[] = {0};
    dev_expect_ctrl_in(&self->ll_dev1, &rsp, sizeof(rsp));

    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(self->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));

    struct js220_port0_msg_s * p0 = (struct js220_port0_msg_s *) m->payload.bin;
    p0->frame_hdr.u32 = js220_frame_hdr_pack(0, sizeof(*p0) - 4, 0);
    p0->port0_hdr.op = JS220_PORT0_OP_CONNECT;
    p0->port0_hdr.status = 0;
    p0->port0_hdr.arg = 0;
    p0->payload.connect.protocol_version = ((uint32_t) JS220_PROTOCOL_VERSION_MAJOR) << 24;
    p0->payload.connect.app_id = 0;
    p0->payload.connect.fw_version   = 0x00010002;
    p0->payload.connect.hw_version   = 0x01000000;
    p0->payload.connect.fpga_version = 0x00010000;
    m->value = jsdrv_union_bin((uint8_t *) p0, 512U);
    m->extra.bkusb_stream.endpoint = JS220_USB_EP_BULK_IN;
    msg_queue_push(self->ll_dev1.rsp_q, m);

    expect_subscribe_cmd(self, DEVICE_PREFIX "/h/state" , &jsdrv_union_u32_r(2));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/fw/version$" , NULL);
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/hw/version$" , NULL);
    expect_subscribe_cmd(self, DEVICE_PREFIX "/h/!reset$" , NULL);
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/fw/version" , &jsdrv_union_u32_r(0x00010002));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/hw/version" , &jsdrv_union_u32_r(0x01000000));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/s/fpga/version" , &jsdrv_union_u32_r(0x00010000));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/" JSDRV_MSG_OPEN "#", &jsdrv_union_i32(0));

    // upper-level driver returns memory to lower-level driver
    dev_expect_bin_ptr(&self->ll_dev1, JSDRV_USBBK_MSG_STREAM_IN_DATA, m->payload.bin, m->value.size);

    publish_to_dev(self, "$", &jsdrv_union_null());
    publish_to_dev(self, "c/!ping", &jsdrv_union_u32(1));
    publish_from_dev(self, "c/!pong", &jsdrv_union_u32(1));
    publish_to_dev(self, "?", &jsdrv_union_null());
    publish_to_dev(self, "c/!ping", &jsdrv_union_u32(1));
    publish_from_dev(self, "c/!pong", &jsdrv_union_u32(1));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/h/state", &jsdrv_union_u32_r(2));
}

static void device1_close(struct test_s * self) {
    assert_int_equal(0, jsdrv_publish(self->context, DEVICE_PREFIX "/" JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), 0));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/" JSDRV_MSG_CLOSE, &jsdrv_union_i32(0));

    dev_expect_i32(&self->ll_dev1, JSDRV_MSG_CLOSE, 0);
    dev_send_i32(self, &self->ll_dev1, JSDRV_MSG_CLOSE, 0);
    expect_subscribe_cmd(self, DEVICE_PREFIX "/h/state", &jsdrv_union_u32_r(1));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/" JSDRV_MSG_CLOSE "#", &jsdrv_union_i32(0));
}
#endif


#define ASSERT_QUEUES_EMPTY(self) do {                              \
    assert_true(msg_queue_is_empty(self->sub_msgs));                \
    if (self->ll_dev1.cmd_q) {                                      \
        assert_true(msg_queue_is_empty(self->ll_dev1.cmd_q));       \
    }                                                               \
} while (0)

static void test_discovery(void ** state) {
    for (int i = 0; i < 3; ++i) {
        SETUP();
        device1_add(self);

        //printf("Discovery: Get the retained state\n");
        //assert_int_equal(0, jsdrv_subscribe(self->context, DEVICE_PREFIX, JSDRV_SFLAG_PUB | JSDRV_SFLAG_RETAIN, subscribe_cmd_fn, self, 1000));
        //expect_subscribe_cmd(self, DEVICE_PREFIX "/h/state", &jsdrv_union_u32_r(1));
        //jsdrv_unsubscribe(self->context, DEVICE_PREFIX, subscribe_cmd_fn, self, 1000);

        device1_remove(self);

        ASSERT_QUEUES_EMPTY(self);
        TEARDOWN();
    }
}

#if 0
static void test_device_open(void ** state) {
    SETUP();
    device1_add(self);
    device1_open(self);
    publish_to_dev(self, "c/led/red", &jsdrv_union_u8(0x55));
    device1_close(self);
    ASSERT_QUEUES_EMPTY(self);
    TEARDOWN();
}

static void subscribe_adc0_data(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct test_s * self = (struct test_s *) user_data;
    assert_int_equal(JSDRV_UNION_BIN, value->type);
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_data(self->context, topic);
    memcpy(msg->payload.bin, value->value.bin, value->size);
    msg->value = jsdrv_union_bin(msg->payload.bin, value->size);
    msg_queue_push(self->sub_msgs, msg);
}

static void test_stream_raw_0(void ** state) {
    uint32_t payload[80][512 / 4];
    uint32_t expected_u32[(JSDRV_STREAM_HEADER_SIZE + 80 * 504) / 4];
    struct jsdrv_stream_signal_s * s = (struct jsdrv_stream_signal_s *) expected_u32;
    s->sample_id = 0;
    s->field_id = JSDRV_FIELD_RAW;
    s->index = 0;
    s->element_type = JSDRV_DATA_TYPE_INT;
    s->element_size_bits = 16;
    s->element_count = ((sizeof(expected_u32) - JSDRV_STREAM_HEADER_SIZE) << 3) / s->element_size_bits;
    uint16_t * expected_u16 = (uint16_t *) &s->data[0];
    union js220_frame_hdr_u hdr;

    SETUP();
    device1_add(self);
    device1_open(self);

    publish_to_dev(self, "s/adc/0/ctrl", &jsdrv_union_u32(1));

    uint32_t sample_id = 0;
    for (uint16_t i = 0; i < 80; ++i) {
        hdr.h.frame_id = i + 1;  // frame_id 0 is open reply
        hdr.h.length = 512 - 4;
        hdr.h.port_id = 16;
        uint32_t * p_u32 = &payload[i][0];
        p_u32[0] = hdr.u32;
        p_u32[1] = sample_id;
        uint16_t * p_u16 = (uint16_t *) &p_u32[2];
        for (uint16_t k = 0; k < (512 - 8) / 2; ++k) {
            p_u16[k] = i * (512 - 8) / 2 + k;
            *expected_u16++ = p_u16[k];
            ++sample_id;
        }
    }

    jsdrv_subscribe(self->context, DEVICE_PREFIX "/s/adc/0/!data", JSDRV_SFLAG_PUB, subscribe_adc0_data, self, 0);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(self->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));
    m->value = jsdrv_union_bin((uint8_t *) payload, sizeof(payload));
    m->extra.bkusb_stream.endpoint = JS220_USB_EP_BULK_IN;
    msg_queue_push(self->ll_dev1.rsp_q, m);
    dev_expect_bin_ptr(&self->ll_dev1, JSDRV_USBBK_MSG_STREAM_IN_DATA, (uint8_t *) payload, sizeof(payload));
    expect_subscribe_cmd_bin(self, DEVICE_PREFIX "/s/adc/0/!data", expected_u32, sizeof(expected_u32));

    device1_close(self);
    ASSERT_QUEUES_EMPTY(self);
    TEARDOWN();
}

static THREAD_RETURN_TYPE dev_pubsub_echo_task(THREAD_ARG_TYPE lpParam) {
    struct test_s * self = (struct test_s *) lpParam;
    struct jsdrvp_msg_s * msg;
    struct jsdrvp_msg_s * rsp;
    uint16_t frame_id = 1;
    while (!self->thread_quit) {
        if (JSDRV_ERROR_TIMED_OUT == msg_queue_pop(self->ll_dev1.cmd_q, &msg, 1)) {
            continue;
        }
        if (0 == strcmp(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic)) {
            uint32_t * p32 = (uint32_t *) msg->value.value.bin;
            struct js220_publish_s * p_src = (struct js220_publish_s *) (p32 + 1);

            // construct return code response
            rsp = jsdrvp_msg_alloc_value(self->context, JSDRV_USBBK_MSG_STREAM_IN_DATA, &jsdrv_union_bin(NULL, 512));
            rsp->value.value.bin = rsp->payload.bin;
            p32 = (uint32_t *) rsp->payload.bin;
            p32[0] = js220_frame_hdr_pack(frame_id, sizeof(struct js220_publish_s) + sizeof(union jsdrv_union_inner_u), 1);
            ++frame_id;
            struct js220_publish_s * p_dst = (struct js220_publish_s *) (p32 + 1);
            jsdrv_cstr_join(p_dst->topic, p_src->topic, "#", sizeof(p_dst->topic));
            p_dst->type = JSDRV_UNION_I32;
            p_dst->flags = 0;
            p_dst->op = 0;
            p_dst->app = 0;
            union jsdrv_union_inner_u * u = (union jsdrv_union_inner_u *) p_dst->data;
            u->u32 = 0;
            msg_queue_push(self->ll_dev1.rsp_q, rsp);
            msg_queue_push(self->ll_dev1.rsp_q, msg);
        } else if (0 == strcmp(JSDRV_USBBK_MSG_STREAM_IN_DATA, msg->topic)) {
            // returning memory
            jsdrvp_msg_free(self->context, msg);
        } else {
            assert_true(false);
        }
    }
    THREAD_RETURN();
}

static void dev_pubsub_echo_start(struct test_s * self) {
    self->thread_quit = 0;
    assert_int_equal(0, jsdrv_thread_create(&self->thread, dev_pubsub_echo_task, self));
}

static void dev_pubsub_echo_stop(struct test_s * self) {
    self->thread_quit = 1;
    // and wait for thread to exit.
    jsdrv_thread_join(&self->thread, 10000);
}

static void test_timeout(void ** state) {
    SETUP();
    device1_add(self);
    device1_open(self);

    assert_int_equal(JSDRV_ERROR_TIMED_OUT, jsdrv_publish(self->context, DEVICE_PREFIX "/c/hello", &jsdrv_union_u32_r(0), 1));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/hello", &jsdrv_union_u32_r(0));
    dev_expect_bulk_out(&self->ll_dev1, 0, "c/hello", &jsdrv_union_u32_r(0));

    dev_pubsub_echo_start(self);
    assert_int_equal(0, jsdrv_publish(self->context, DEVICE_PREFIX "/c/hello", &jsdrv_union_u32_r(1), 1000000));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/hello", &jsdrv_union_u32_r(1));
    expect_subscribe_cmd(self, DEVICE_PREFIX "/c/hello#", &jsdrv_union_i32(0));
    dev_pubsub_echo_stop(self);

    device1_close(self);
    ASSERT_QUEUES_EMPTY(self);
    TEARDOWN();
}
#endif

void log_recv(void * user_data, struct jsdrv_log_header_s const * header,
              const char * filename, const char * message) {
    (void) user_data;
    printf("%c %s[%d]: %s\n", jsdrv_log_level_to_char(header->level),
           filename, header->line, message);
}

int main(void) {
    int rv;
    jsdrv_log_initialize();
    jsdrv_log_level_set(JSDRV_LOG_LEVEL_ALL);
    jsdrv_log_register(log_recv, NULL);

    //setvbuf(stdout, NULL, _IONBF, 0);
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_discovery),
            //cmocka_unit_test(test_device_open),
            //cmocka_unit_test(test_stream_raw_0),
            //cmocka_unit_test(test_timeout),
    };

    rv = cmocka_run_group_tests(tests, NULL, NULL);
    jsdrv_log_finalize();
    return rv;
}
