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
#include "jsdrv_prv/thread.h"
#include "tinyprintf.h"
//#include "test.inc"

// copied from jsdrv.c, line 55
static const size_t STREAM_MSG_SZ = sizeof(struct jsdrvp_msg_s) - sizeof(union jsdrvp_payload_u) + sizeof(struct jsdrv_stream_signal_s);


struct sub_s {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    uint8_t flags;
    jsdrv_pubsub_subscribe_fn cbk_fn;
    void * cbk_user_data;
    struct jsdrv_list_s item;
};

struct jsdrv_context_s {
    struct jsdrv_list_s msg_sent;
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

static void msg_sent_free(struct jsdrv_context_s * context) {
    struct jsdrv_list_s * item;
    while (jsdrv_list_length(&context->msg_sent)) {
        item = jsdrv_list_remove_head(&context->msg_sent);
        struct jsdrvp_msg_s * m = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
        jsdrvp_msg_free(context, m);
    }
}

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    if (0 == strcmp(JSDRV_PUBSUB_SUBSCRIBE, msg->topic)) {
        subscribe(context, msg);
        return;
    } else if (0 == strcmp(JSDRV_PUBSUB_UNSUBSCRIBE, msg->topic)) {
        unsubscribe(context, msg);
        return;
    } else if (jsdrv_cstr_ends_with(msg->topic, "$")) {
        const char * meta_topic = msg->topic;
        check_expected_ptr(meta_topic);
    } else if (0 == strcmp(JSDRV_BUFFER_MGR_MSG_ACTION_LIST, msg->topic)) {
        const size_t buf_list_length = msg->value.size;
        const uint8_t *buf_list_buffers = msg->value.value.bin;
        check_expected(buf_list_length);
        check_expected_ptr(buf_list_buffers);
    } else if (jsdrv_cstr_starts_with(msg->topic, "m/+/")) {
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
        if (0 == strcmp(JSDRV_BUFFER_MSG_LIST, ch)) {
            const size_t sig_list_length = msg->value.size;
            const uint8_t *sig_list_buffers = msg->value.value.bin;
            check_expected(sig_list_length);
            check_expected_ptr(sig_list_buffers);
        } else {
            // unknown topic, not supported
            assert_true(0);
        }
    } else {
        // ???
        jsdrv_list_add_tail(&context->msg_sent, &msg->item);
        return;
    }
}

#define expect_meta(topic__) expect_string(jsdrvp_backend_send, meta_topic, topic__)

#define expect_buf_list(ex_list, ex_len) \
    expect_value(jsdrvp_backend_send, buf_list_length, ex_len); \
    expect_memory(jsdrvp_backend_send, buf_list_buffers, ex_list, ex_len)

#define expect_sig_list(ex_list, ex_len) \
    expect_value(jsdrvp_backend_send, sig_list_length, ex_len); \
    expect_memory(jsdrvp_backend_send, sig_list_buffers, ex_list, ex_len)

struct jsdrv_context_s * initialize() {
    uint8_t ex_list_buffer[] = {0};
    struct jsdrv_context_s * context = malloc(sizeof(struct jsdrv_context_s));
    memset(context, 0, sizeof(*context));
    jsdrv_list_initialize(&context->subscribers);
    jsdrv_list_initialize(&context->msg_sent);
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_ADD "$");
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE "$");
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_LIST "$");
    expect_buf_list(ex_list_buffer, sizeof(ex_list_buffer));
    assert_int_equal(0, jsdrv_buffer_initialize(context));
    assert_int_equal(0, jsdrv_list_length(&context->msg_sent));
    return context;
}

void finalize(struct jsdrv_context_s * context) {
    struct jsdrv_list_s * item;
    jsdrv_buffer_finalize();
    while (jsdrv_list_length(&context->subscribers)) {
        item = jsdrv_list_remove_head(&context->subscribers);
        struct sub_s * sub = JSDRV_CONTAINER_OF(item, struct sub_s, item);
        free(sub);
    }
    msg_sent_free(context);
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
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(3)));
    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(3)));
    //finalize(context);
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
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(buffer_id)));

    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_ADD);
    expect_sig_list(ex_list_sig1, sizeof(ex_list_sig1));
    publish(context, msg);
    // todo waitfor list_sig

    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_str("u/js220/0123456/s/i/!data"));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/s/%03u/s/topic", buffer_id, signal_id);
    // expect subscribe
    publish(context, msg);

    // tear down
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_REMOVE);
    // expect unsubscribe
    expect_sig_list(ex_list_sig0, sizeof(ex_list_sig0));
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MSG_ACTION_SIGNAL_REMOVE, &jsdrv_union_u8(5)));
    // todo waitfor list_sig

    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(3)));
    //finalize(context);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize_finalize),
            cmocka_unit_test(test_add_remove),
            cmocka_unit_test(test_one_signal),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
