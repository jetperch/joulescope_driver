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
#include "jsdrv.h"
#include "jsdrv_prv/pubsub.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv/cstr.h"
#include <stdarg.h>
#include <stdio.h>

static const char * META1 = "{"
    "\"dtype\": \"bool\","
    "\"brief\": \"Enable software LED control.\","
    "\"default\": 0"
"}";

static const char * META2 = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"Control something good.\","
    "\"default\": 0"
"}";


#define SETUP() \
    (void) state;                                              \
    struct jsdrv_pubsub_s * p = jsdrv_pubsub_initialize(NULL); \
    assert_non_null(p)


#define TEARDOWN() \
    (void) state;  \
    jsdrv_pubsub_finalize(p)

#define check_expected_ptr_value(parameter) \
    _check_expected(__func__, "value", __FILE__, __LINE__, \
                    cast_ptr_to_largest_integral_type(parameter))

#define check_expected_value(parameter) \
    _check_expected(__func__, "value", __FILE__, __LINE__, \
                    cast_to_largest_integral_type(parameter))

static uint8_t on_subscribe_internal(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    char * topic = msg->topic;
    uint8_t type = msg->value.type;
    check_expected_ptr(topic);
    check_expected(type);
    switch (type) {
        case JSDRV_UNION_STR: check_expected_ptr_value(msg->value.value.str); break;
        case JSDRV_UNION_JSON: check_expected_ptr_value(msg->value.value.str); break;
        case JSDRV_UNION_U32: check_expected_value(msg->value.value.u32); break;
        case JSDRV_UNION_I32: check_expected_value(msg->value.value.i32); break;
        default:
            assert_true(false);
    }
    return 0;
}

#define expect_publish_internal(topic_, value__) {                                                  \
    struct jsdrv_union_s * value_ = (value__);                                                        \
    expect_string(on_subscribe_internal, topic, topic_);                                            \
    expect_value(on_subscribe_internal, type, value_->type);                                        \
    switch (value_->type) {                                                                         \
        case JSDRV_UNION_STR:  expect_string(on_subscribe_internal, value, value_->value.str); break; \
        case JSDRV_UNION_JSON: expect_string(on_subscribe_internal, value, value_->value.str); break; \
        case JSDRV_UNION_U32:  expect_value(on_subscribe_internal, value, value_->value.u32); break;  \
        case JSDRV_UNION_I32:  expect_value(on_subscribe_internal, value, value_->value.i32); break;  \
        default: assert_true(false); break;                                                         \
    }                                                                                               \
}

static void on_subscribe_external(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    uint8_t type = value->type;
    check_expected_ptr(topic);
    check_expected(type);
    switch (type) {
        case JSDRV_UNION_STR: check_expected_ptr_value(value->value.str); break;
        case JSDRV_UNION_JSON: check_expected_ptr_value(value->value.str); break;
        case JSDRV_UNION_U32: check_expected_value(value->value.u32); break;
        case JSDRV_UNION_I32: check_expected_value(value->value.i32); break;
        default:
            assert_true(false);
    }
}

#define expect_publish_external(topic_, value__) {                                                  \
    struct jsdrv_union_s * value_ = (value__);                                                        \
    expect_string(on_subscribe_external, topic, topic_);                                            \
    expect_value(on_subscribe_external, type, value_->type);                                        \
    switch (value_->type) {                                                                         \
        case JSDRV_UNION_STR:  expect_string(on_subscribe_external, value, value_->value.str); break; \
        case JSDRV_UNION_JSON: expect_string(on_subscribe_external, value, value_->value.str); break; \
        case JSDRV_UNION_U32:  expect_value(on_subscribe_external, value, value_->value.u32); break;  \
        case JSDRV_UNION_I32:  expect_value(on_subscribe_external, value, value_->value.i32); break;  \
        default: assert_true(false); break;                                                         \
    }                                                                                               \
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s *m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    return m;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    (void) context;
    if (msg) {
        jsdrv_free(msg);
    }
}

static struct jsdrvp_msg_s * subscribe_msg(struct jsdrv_pubsub_s * p, const char * topic, uint8_t flags, const char * op) {
    (void) p;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(NULL);
    jsdrv_cstr_copy(m->topic, op, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic, sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.internal_fn = on_subscribe_internal;
    m->payload.sub.subscriber.user_data = NULL;
    m->payload.sub.subscriber.flags = flags;
    return m;
}

static void subscribe_internal(struct jsdrv_pubsub_s * p, const char * name, uint8_t flags) {
    jsdrv_pubsub_publish(p, subscribe_msg(p, name, flags, JSDRV_PUBSUB_SUBSCRIBE));
}

static void unsubscribe_internal(struct jsdrv_pubsub_s * p, const char * name) {
    jsdrv_pubsub_publish(p, subscribe_msg(p, name, 0, JSDRV_PUBSUB_UNSUBSCRIBE));
}

static void unsubscribe_all_internal(struct jsdrv_pubsub_s * p, const char * name) {
    jsdrv_pubsub_publish(p, subscribe_msg(p, name, 0, JSDRV_PUBSUB_UNSUBSCRIBE_ALL));
}

static void subscribe_external_common(struct jsdrv_pubsub_s * p, const char * topic, uint8_t flags, const char * op) {
    struct jsdrvp_msg_s * m = subscribe_msg(p, topic, flags, op);
    m->payload.sub.subscriber.is_internal = 0;
    m->payload.sub.subscriber.external_fn = on_subscribe_external;
    m->source = 1;
    jsdrv_pubsub_publish(p, m);
}

static void subscribe_external(struct jsdrv_pubsub_s * p, const char * topic, uint8_t flags) {
    subscribe_external_common(p, topic, flags, JSDRV_PUBSUB_SUBSCRIBE);
}

static void unsubscribe_external(struct jsdrv_pubsub_s * p, const char * topic) {
    subscribe_external_common(p, topic, 0, JSDRV_PUBSUB_UNSUBSCRIBE);
}

static void unsubscribe_external_all(struct jsdrv_pubsub_s * p, const char * topic) {
    subscribe_external_common(p, topic, 0, JSDRV_PUBSUB_UNSUBSCRIBE_ALL);
}

static void publish_str(struct jsdrv_pubsub_s * p, const char * name, const char * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(NULL);
    jsdrv_cstr_copy(m->topic, name, sizeof(m->topic));
    m->value.type = JSDRV_UNION_STR;
    m->value.value.str = m->payload.str;
    jsdrv_cstr_copy(m->payload.str, value, sizeof(m->payload.str));
    jsdrv_pubsub_publish(p, m);
}

static void publish(struct jsdrv_pubsub_s * p, const char * topic, struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(NULL, topic, value);
    m->source = 1;
    jsdrv_pubsub_publish(p, m);
}

static void test_subscribe_then_publish(void ** state) {
    SETUP();
    subscribe_internal(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB);
    jsdrv_pubsub_process(p);
    publish_str(p, "u/js110/123456/hello", "world");
    expect_publish_internal("u/js110/123456/hello", &jsdrv_union_str("world"));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_subscribe_parent_then_publish(void ** state) {
    SETUP();
    subscribe_internal(p, "u/js110/123456", JSDRV_SFLAG_PUB);
    subscribe_internal(p, "", JSDRV_SFLAG_PUB);
    jsdrv_pubsub_process(p);
    publish_str(p, "u/js110/123456/hello", "world");
    expect_publish_internal("u/js110/123456/hello", &jsdrv_union_str("world"));
    expect_publish_internal("u/js110/123456/hello", &jsdrv_union_str("world"));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_publish_subscribe_no_retain(void ** state) {
    SETUP();
    publish(p, "u/js110/123456/hello", &jsdrv_union_u32_r(0));
    struct jsdrvp_msg_s * m = subscribe_msg(p, "", JSDRV_SFLAG_PUB, JSDRV_PUBSUB_SUBSCRIBE);
    jsdrv_pubsub_publish(p, m);
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_publish_subscribe_retain(void ** state) {
    SETUP();
    publish(p, "u/js110/123456/hello", &jsdrv_union_u32_r(0));
    struct jsdrvp_msg_s * m = subscribe_msg(p, "", JSDRV_SFLAG_PUB | JSDRV_SFLAG_RETAIN, JSDRV_PUBSUB_SUBSCRIBE);
    jsdrv_pubsub_publish(p, m);
    expect_publish_internal("u/js110/123456/hello", &jsdrv_union_u32_r(0));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_subscribe_publish_nopub(void ** state) {
    SETUP();
    struct jsdrvp_msg_s * m = subscribe_msg(p, "", 0, JSDRV_PUBSUB_SUBSCRIBE);
    jsdrv_pubsub_publish(p, m);
    publish(p, "u/js110/123456/hello", &jsdrv_union_u32_r(0));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_unsubscribe(void ** state) {
    SETUP();
    subscribe_internal(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB);
    unsubscribe_internal(p, "u/js110/123456/hello");
    jsdrv_pubsub_process(p);
    publish_str(p, "u/js110/123456/hello", "world");
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_unsubscribe_all(void ** state) {
    SETUP();
    subscribe_internal(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB);
    unsubscribe_all_internal(p, "");
    jsdrv_pubsub_process(p);
    publish_str(p, "u/js110/123456/hello", "world");
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_external_subscribe_publish_unsubscribe(void ** state) {
    SETUP();
    subscribe_external(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB);
    publish_str(p, "u/js110/123456/hello", "world");
    expect_publish_external("u/js110/123456/hello", &jsdrv_union_str("world"));
    jsdrv_pubsub_process(p);

    unsubscribe_external(p, "u/js110/123456/hello");
    publish_str(p, "u/js110/123456/hello", "there");
    jsdrv_pubsub_process(p);

    TEARDOWN();
}

static void test_external_subscribe_publish_unsubscribe_all(void ** state) {
    SETUP();
    subscribe_external(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB);
    publish_str(p, "u/js110/123456/hello", "world");
    expect_publish_external("u/js110/123456/hello", &jsdrv_union_str("world"));
    jsdrv_pubsub_process(p);

    unsubscribe_external_all(p, "u/js110/123456/hello");
    publish_str(p, "u/js110/123456/hello", "there");
    jsdrv_pubsub_process(p);

    TEARDOWN();
}

static void test_external_retain(void ** state) {
    SETUP();
    publish(p, "u/js110/123456/hello", &jsdrv_union_u32_r(1));
    subscribe_external(p, "u/js110/123456/hello", JSDRV_SFLAG_PUB | JSDRV_SFLAG_RETAIN);
    expect_publish_external("u/js110/123456/hello", &jsdrv_union_u32_r(1));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

static void test_return_code(void ** state) {
    SETUP();
    subscribe_internal(p, "u/js110/123456/hello", JSDRV_SFLAG_RETURN_CODE);
    jsdrv_pubsub_process(p);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(NULL, "u/js110/123456/hello#", &jsdrv_union_i32(1));
    jsdrv_pubsub_publish(p, m);
    expect_publish_internal("u/js110/123456/hello#", &jsdrv_union_i32(1));
    jsdrv_pubsub_process(p);

    // but should not get normal updates with NOPUB flag.
    publish_str(p, "u/js110/123456/hello", "world");
    jsdrv_pubsub_process(p);

    TEARDOWN();
}

static void test_meta(void ** state) {
    SETUP();
    publish(p, "u/hello$", &jsdrv_union_json(META1));
    subscribe_external(p, "", JSDRV_SFLAG_METADATA_RSP | JSDRV_SFLAG_RETAIN);
    publish(p, "u/world$", &jsdrv_union_json(META2));
    expect_publish_external("u/hello$", &jsdrv_union_json(META1));  // always get retained metadata
    expect_publish_external("u/world$", &jsdrv_union_json(META2));
    jsdrv_pubsub_process(p);
    TEARDOWN();
}

#define query(topic__, buf__)                                                           \
    m = jsdrvp_msg_alloc_value(NULL, JSDRV_PUBSUB_QUERY, &jsdrv_union_i32(0));            \
    jsdrv_cstr_copy(m->payload.query.topic, topic__, sizeof(m->payload.query.topic));     \
    v = jsdrv_union_str(buf__);                                                           \
    v.size = sizeof(buf__);                                                             \
    m->payload.query.value = &v;                                                        \
    jsdrv_pubsub_publish(p, m);

static void test_query(void ** state) {
    SETUP();
    struct jsdrvp_msg_s * m;
    struct jsdrv_union_s v;

    const char * str1 = "hello world";
    char buf[256];

    publish(p, "u/hello", &jsdrv_union_cstr_r(str1));
    publish(p, "u/there", &jsdrv_union_u32_r(42));
    jsdrv_pubsub_process(p);

    query("u/hello", buf);
    jsdrv_pubsub_process(p);
    assert_string_equal(str1, buf);

    query("u/there", buf);
    jsdrv_pubsub_process(p);
    assert_int_equal(42, v.value.u32);

    TEARDOWN();
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_subscribe_then_publish),
            cmocka_unit_test(test_subscribe_parent_then_publish),
            cmocka_unit_test(test_publish_subscribe_no_retain),
            cmocka_unit_test(test_publish_subscribe_retain),
            cmocka_unit_test(test_subscribe_publish_nopub),
            cmocka_unit_test(test_unsubscribe),
            cmocka_unit_test(test_unsubscribe_all),
            cmocka_unit_test(test_external_subscribe_publish_unsubscribe),
            cmocka_unit_test(test_external_subscribe_publish_unsubscribe_all),
            cmocka_unit_test(test_external_retain),
            cmocka_unit_test(test_return_code),
            cmocka_unit_test(test_meta),
            cmocka_unit_test(test_query),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
