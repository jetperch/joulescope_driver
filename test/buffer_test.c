/*
 * Copyright 2023 Jetperch LLC
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jsdrv.h"
#include "jsdrv_prv/buffer.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/msg_queue.h"
#include "tinyprintf.h"
//#include "test.inc"

// copied from jsdrv.c, line 55
static const size_t STREAM_MSG_SZ = sizeof(struct jsdrvp_msg_s) - sizeof(union jsdrvp_payload_u) + sizeof(struct jsdrv_stream_signal_s);
static const uint32_t TIMEOUT_MS = 100000;  // todo 100


struct sub_s {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    uint8_t flags;
    jsdrv_pubsub_subscribe_fn cbk_fn;
    void * cbk_user_data;
    struct jsdrv_list_s item;
};

struct jsdrv_context_s {
    struct msg_queue_s * msg_sent;
    struct jsdrv_list_s subscribers;
};

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = calloc(1, sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    m->inner_msg_type = JSDRV_MSG_TYPE_NORMAL;
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    (void) context;
    struct jsdrvp_msg_s * m = calloc(1, STREAM_MSG_SZ);
    jsdrv_list_initialize(&m->item);
    m->inner_msg_type = JSDRV_MSG_TYPE_DATA;
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_bin(&m->payload.bin[0], 0);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    m->value.flags &= ~JSDRV_UNION_FLAG_HEAP_MEMORY;

    switch (value->type) {
        case JSDRV_UNION_JSON:  /* intentional fall-through */
        case JSDRV_UNION_STR:
            if (m->value.size == 0) {
                m->value.size = (uint32_t) (strlen(value->value.str) + 1);
            }
            /* intentional fall-through */
        case JSDRV_UNION_BIN:
            if (value->size > sizeof(m->payload.bin)) {
                uint8_t * ptr = malloc(value->size);
                memcpy(ptr, value->value.bin, value->size);
                m->value.value.bin = ptr;
                m->value.flags |= JSDRV_UNION_FLAG_HEAP_MEMORY;
            } else {
                m->value.value.bin = m->payload.bin;
                memcpy(m->payload.bin, value->value.bin, m->value.size);
            }
            break;
        default:
            break;
    }
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_clone(struct jsdrv_context_s * context, const struct jsdrvp_msg_s * msg_src) {
    struct jsdrvp_msg_s * m;
    if (msg_src->inner_msg_type == JSDRV_MSG_TYPE_DATA) {
        m = jsdrvp_msg_alloc_data(context, msg_src->topic);
        m->value = msg_src->value;
        m->value.value.bin = &m->payload.bin[0];
        m->payload = msg_src->payload;
    } else {
        m = jsdrvp_msg_alloc(context);
        *m = *msg_src;
        switch (m->value.type) {
            case JSDRV_UNION_JSON:  // intentional fall-through
            case JSDRV_UNION_STR:
                m->value.value.str = m->payload.str;
                break;
            case JSDRV_UNION_BIN:
                if (m->value.flags & JSDRV_UNION_FLAG_HEAP_MEMORY) {
                    uint8_t *ptr = malloc(m->value.size);
                    memcpy(ptr, m->value.value.bin, m->value.size);
                    m->value.value.bin = ptr;
                } else {
                    m->value.value.bin = m->payload.bin;
                }
                break;
            default:
                break;
        }
    }
    jsdrv_list_initialize(&m->item);
    return m;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    (void) context;
    if (msg->value.flags & JSDRV_UNION_FLAG_HEAP_MEMORY) {
        free((void *) msg->value.value.bin);
    }
    free(msg);
}

static void subscribe(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct sub_s * s = calloc(1, sizeof(struct sub_s));
    jsdrv_cstr_copy(s->topic, msg->payload.sub.topic, sizeof(s->topic));
    s->cbk_fn = msg->payload.sub.subscriber.internal_fn;
    s->cbk_user_data = msg->payload.sub.subscriber.user_data;
    s->flags = msg->payload.sub.subscriber.flags;
    jsdrv_list_initialize(&s->item);
    jsdrv_list_add_tail(&context->subscribers, &s->item);
}

static void unsubscribe(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    struct sub_s * s;
    jsdrv_list_foreach(&context->subscribers, item) {
        s = JSDRV_CONTAINER_OF(item, struct sub_s, item);
        if ((0 == strcmp(s->topic, msg->payload.sub.topic)) &&
                (s->cbk_fn == msg->extra.frontend.subscriber.internal_fn) &&
                (s->cbk_user_data == msg->extra.frontend.subscriber.user_data)) {
            jsdrv_list_remove(item);
            free(s);
        }
    }
}

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    msg_queue_push(context->msg_sent, msg);
}

static void msg_send_process_next(struct jsdrv_context_s * context, uint32_t timeout_ms) {
    struct jsdrvp_msg_s * msg = NULL;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    char return_code_suffix[2] = {JSDRV_TOPIC_SUFFIX_RETURN_CODE, 0};
    while (1) {
        assert_int_equal(0, msg_queue_pop(context->msg_sent, &msg, timeout_ms));
        jsdrv_cstr_copy(topic, msg->topic, sizeof(topic));
        if (!jsdrv_cstr_ends_with(msg->topic, return_code_suffix)) {
            break;
        }
    }
    if (0 == strcmp(JSDRV_PUBSUB_SUBSCRIBE, msg->topic)) {
        jsdrv_cstr_copy(topic, msg->payload.sub.topic, sizeof(topic));
        check_expected_ptr(topic);
        subscribe(context, msg);
    } else if (0 == strcmp(JSDRV_PUBSUB_UNSUBSCRIBE, msg->topic)) {
        jsdrv_cstr_copy(topic, msg->payload.sub.topic, sizeof(topic));
        check_expected_ptr(topic);
        unsubscribe(context, msg);
    } else if (jsdrv_cstr_ends_with(msg->topic, "$")) {
        const char * meta_topic = msg->topic;
        check_expected_ptr(meta_topic);
    } else if (0 == strcmp(JSDRV_BUFFER_MGR_MSG_ACTION_LIST, msg->topic)) {
        const size_t buf_list_length = msg->value.size;
        const uint8_t *buf_list_buffers = msg->value.value.bin;
        check_expected(buf_list_length);
        check_expected_ptr(buf_list_buffers);
    } else if (jsdrv_cstr_starts_with(msg->topic, "m/@/")) {
        // unknown topic, not supported
        assert_true(0);
    } else if (jsdrv_cstr_starts_with(msg->topic, "m/")) {
        char buffer_id_str[4] = {0, 0, 0, 0};
        uint32_t buffer_id = 0;
        char * ch = &msg->topic[2];
        for (int i = 0; i < 3; ++i) {
            buffer_id_str[i] = ch[i];
        }
        assert_int_equal('/', ch[3]);
        assert_int_equal(0, jsdrv_cstr_to_u32(buffer_id_str, &buffer_id));
        ch += 4;
        if ((ch[0] == 's') && (ch[1] == '/')) {
            ch += 2;
            char signal_id_str[4] = {0, 0, 0, 0};
            uint32_t signal_id = 0;
            for (int i = 0; i < 3; ++i) {
                signal_id_str[i] = ch[i];
            }
            assert_int_equal('/', ch[3]);
            assert_int_equal(0, jsdrv_cstr_to_u32(signal_id_str, &signal_id));
            ch += 4;
            if (0 == strcmp("info", ch)) {
                check_expected_ptr(topic);
            } else {
                // unknown topic, not supported
                assert_true(0);
            }
        } else if ((ch[0] == 'g') && (ch[1] == '/')) {
            if (0 == strcmp(JSDRV_BUFFER_MSG_LIST, ch)) {
                const size_t sig_list_length = msg->value.size;
                const uint8_t *sig_list_buffers = msg->value.value.bin;
                check_expected(sig_list_length);
                check_expected_ptr(sig_list_buffers);
            } else {
                // unknown topic, not supported
                assert_true(0);
            }
        } else  {
            // unknown topic, not supported
            assert_true(0);
        }
    } else if (jsdrv_cstr_ends_with(msg->topic, "!rsp")) {
        check_expected_ptr(topic);
    } else {
        // ???
        assert_true(0);
    }
    jsdrvp_msg_free(context, msg);
}

#define expect_meta(topic__) expect_string(msg_send_process_next, meta_topic, topic__)

#define expect_subscribe(topic_) \
    expect_string(msg_send_process_next, topic, topic_)

#define expect_unsubscribe(topic_) \
    expect_string(msg_send_process_next, topic, topic_)

#define expect_buf_list(ex_list, ex_len) \
    expect_value(msg_send_process_next, buf_list_length, ex_len); \
    expect_memory(msg_send_process_next, buf_list_buffers, ex_list, ex_len)

#define expect_sig_list(ex_list, ex_len) \
    expect_value(msg_send_process_next, sig_list_length, ex_len); \
    expect_memory(msg_send_process_next, sig_list_buffers, ex_list, ex_len)

#define expect_info_any(topic_) \
    expect_string(msg_send_process_next, topic, topic_)

#define expect_rsp_any(topic_) \
    expect_string(msg_send_process_next, topic, topic_)


struct jsdrv_context_s * initialize() {
    uint8_t ex_list_buffer[] = {0};
    struct jsdrv_context_s * context = malloc(sizeof(struct jsdrv_context_s));
    memset(context, 0, sizeof(*context));
    context->msg_sent = msg_queue_init();
    assert_non_null(context->msg_sent);
    jsdrv_list_initialize(&context->subscribers);
    assert_int_equal(0, jsdrv_buffer_initialize(context));

    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_ADD "$");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE "$");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_LIST "$");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_subscribe(JSDRV_BUFFER_MGR_MSG_ACTION_ADD);
    msg_send_process_next(context, TIMEOUT_MS);
    expect_subscribe(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE);
    msg_send_process_next(context, TIMEOUT_MS);
    expect_buf_list(ex_list_buffer, sizeof(ex_list_buffer));
    msg_send_process_next(context, TIMEOUT_MS);

    return context;
}

void finalize(struct jsdrv_context_s * context) {
    struct jsdrv_list_s * item;
    jsdrv_buffer_finalize();
    expect_unsubscribe(JSDRV_BUFFER_MGR_MSG_ACTION_ADD);
    msg_send_process_next(context, TIMEOUT_MS);
    expect_unsubscribe(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE);
    msg_send_process_next(context, TIMEOUT_MS);

    while (jsdrv_list_length(&context->subscribers)) {
        item = jsdrv_list_remove_head(&context->subscribers);
        struct sub_s * sub = JSDRV_CONTAINER_OF(item, struct sub_s, item);
        free(sub);
    }
    free(context);
}

int32_t publish(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    char * t;
    jsdrv_cstr_copy(topic, msg->topic, sizeof(topic));
    t = &topic[strlen(topic)];
    while (1) {
        jsdrv_list_foreach(&context->subscribers, item) {
            struct sub_s *sub = JSDRV_CONTAINER_OF(item, struct sub_s, item);
            if (0 == strcmp(topic, sub->topic)) {
                sub->cbk_fn(sub->cbk_user_data, msg);
            }
        }
        while (1) {
            --t;
            if (t <= topic) {
                return 0;
            }
            if (*t == '/') {
                *t = 0;
                break;
            }
        }
    }
}

static void test_initialize_finalize(void **state) {
    (void) state;
    struct jsdrv_context_s * context = initialize();
    finalize(context);
}

static void test_add_remove(void **state) {
    (void) state;
    uint8_t ex_list_buffer0[] = {0};
    uint8_t ex_list_buffer1[] = {3, 0};
    struct jsdrv_context_s * context = initialize();
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(3)));

    expect_subscribe("m/003");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    msg_send_process_next(context, TIMEOUT_MS);

    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(3)));
    expect_unsubscribe("m/003");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    msg_send_process_next(context, TIMEOUT_MS);

    finalize(context);
}

static struct jsdrvp_msg_s * generate_msg_data_i(struct jsdrv_context_s * context, uint64_t sample_id, uint32_t length) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_data(context, "u/js220/0123456/s/i/!data");
    struct jsdrv_stream_signal_s * s = (struct jsdrv_stream_signal_s *) msg->value.value.bin;
    uint32_t decimate_factor = 2;
    s->sample_id = sample_id * decimate_factor;
    s->field_id = JSDRV_FIELD_CURRENT;
    s->index = 0;
    s->element_type = JSDRV_DATA_TYPE_FLOAT;
    s->element_size_bits = 32;
    s->element_count = length;
    s->sample_rate = 2000000;
    s->decimate_factor = decimate_factor;
    float * sdata = (float *) s->data;
    for (uint32_t i = 0; i < s->element_count; ++i) {
        sdata[i] = ((float) (sample_id + i)) * 0.001f;
    }
    return msg;
}

static void test_one_signal(void **state) {
    (void) state;
    struct jsdrvp_msg_s * msg;
    const uint8_t buffer_id = 3;
    const uint8_t signal_id = 5;
    uint8_t ex_list_buffer0[] = {0};
    uint8_t ex_list_buffer1[] = {buffer_id, 0};
    uint8_t ex_list_sig0[] = {0};
    uint8_t ex_list_sig1[] = {signal_id, 0};

    struct jsdrv_context_s * context = initialize();
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(buffer_id)));
    expect_subscribe("m/003");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    msg_send_process_next(context, TIMEOUT_MS);

    // Add signal
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_ADD);
    publish(context, msg);
    expect_sig_list(ex_list_sig1, sizeof(ex_list_sig1));
    msg_send_process_next(context, TIMEOUT_MS);

    // and set source topic
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_str("u/js220/0123456/s/i/!data"));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/s/%03u/topic", buffer_id, signal_id);
    publish(context, msg);
    expect_subscribe("u/js220/0123456/s/i/!data");
    msg_send_process_next(context, TIMEOUT_MS);

    // set buffer size
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u64(1000000LLU));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_SIZE);
    publish(context, msg);

    // send first data frame
    msg = generate_msg_data_i(context, 10000LLU, 100);
    publish(context, msg);
    expect_info_any("m/003/s/005/info");
    msg_send_process_next(context, TIMEOUT_MS);

    // Send second data frame
    msg = generate_msg_data_i(context, 10100LLU, 100);
    publish(context, msg);
    expect_info_any("m/003/s/005/info");  // todo check range?
    msg_send_process_next(context, TIMEOUT_MS);

    // request sample data, expect response
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 10000LLU;
    req.time.samples.length = 200;
    jsdrv_cstr_copy(req.rsp_topic, "t/!rsp", sizeof(req.rsp_topic));
    req.rsp_id = 42;
    msg = jsdrvp_msg_alloc_value(context, "m/003/s/005/!req", &jsdrv_union_bin((uint8_t *) &req, sizeof(req)));
    publish(context, msg);
    expect_rsp_any("t/!rsp");
    msg_send_process_next(context, TIMEOUT_MS);

    // request summary data, expect response
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 10000LLU;
    req.time.samples.end = 10200LLU;
    req.time.samples.length = 20;
    jsdrv_cstr_copy(req.rsp_topic, "t/!rsp", sizeof(req.rsp_topic));
    req.rsp_id = 43;
    msg = jsdrvp_msg_alloc_value(context, "m/003/s/005/!req", &jsdrv_union_bin((uint8_t *) &req, sizeof(req)));
    publish(context, msg);
    expect_rsp_any("t/!rsp");
    msg_send_process_next(context, TIMEOUT_MS);

    // tear down
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_REMOVE);
    publish(context, msg);
    expect_unsubscribe("u/js220/0123456/s/i/!data");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_sig_list(ex_list_sig0, sizeof(ex_list_sig0));
    msg_send_process_next(context, TIMEOUT_MS);

    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(buffer_id)));
    expect_unsubscribe("m/003");
    msg_send_process_next(context, TIMEOUT_MS);
    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    msg_send_process_next(context, TIMEOUT_MS);

    finalize(context);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize_finalize),
            cmocka_unit_test(test_add_remove),
            cmocka_unit_test(test_one_signal),
            // test hold
            // test buffer wrap
            // test mode: fill
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
