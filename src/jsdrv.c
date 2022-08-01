/*
 * Copyright 2014-2022 Jetperch LLC
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

/**
 * @file
 *
 * @brief Joulescope driver frontend
 */

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/pubsub.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv/cstr.h"
#include "jsdrv/time.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"


#define DEVICE_COUNT_MAX    (256U)  // 255 Joulescopes attached to 1 host should be enough
#define BACKEND_COUNT_MAX   (127U)  // Allow prefixes 0-9, a-z, A-Z
#define DEVICE_LOOKUP_MAX   (BACKEND_COUNT_MAX * DEVICE_COUNT_MAX)
#define API_TIMEOUT_MS      (300000)   // todo put back to 3000

#ifndef UNITTEST
#define UNITTEST 0
#endif


JSDRV_STATIC_ASSERT(DEVICE_LOOKUP_MAX < UINT16_MAX, too_many_devices);
JSDRV_STATIC_ASSERT(JSDRV_STREAM_HEADER_SIZE == offsetof(struct jsdrv_stream_signal_s, data), jsdrv_stream_signal_s_header_size);
JSDRV_STATIC_ASSERT(JSDRV_STREAM_DATA_SIZE == (sizeof(struct jsdrv_stream_signal_s) - JSDRV_STREAM_HEADER_SIZE), sizeof_jsdrv_stream_signal_s);
static const size_t STREAM_MSG_SZ = sizeof(struct jsdrvp_msg_s) - sizeof(union jsdrvp_payload_u) + sizeof(struct jsdrv_stream_signal_s);

struct frontend_dev_s {
    char prefix[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_context_s * context;
    struct jsdrvp_ul_device_s * device;
    struct jsdrv_list_s item;
};


enum state_e {
    ST_INIT_AWAITING_FRONTEND,
    ST_INIT_AWAITING_BACKEND,
    ST_ACTIVE,
    ST_SHUTDOWN,
};

struct jsdrv_context_s {
    struct msg_queue_s * msg_free;
    struct msg_queue_s * msg_free_data;
    struct msg_queue_s * msg_cmd;       // from API (any thread) to jsdrv thread
    struct msg_queue_s * msg_backend;   // backend thread(s) to jsdrv thread

    const struct jsdrv_arg_s * args;
    enum state_e state;
    int32_t init_status;  // 0 or first reported backend error code.
    struct jsdrvbk_s * backends[BACKEND_COUNT_MAX];
    struct jsdrv_pubsub_s * pubsub;
    struct jsdrv_list_s devices;          // frontend_dev_s
    struct jsdrv_list_s cmd_timeouts;
    jsdrv_thread_t thread;

    volatile bool do_exit;
};

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    struct jsdrvp_msg_s * m = msg_queue_pop_immediate(context->msg_free);
    if (!m) {
        m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
        JSDRV_LOGD3("jsdrvp_msg_alloc %p", m);
        jsdrv_list_initialize(&m->item);
    }
    m->inner_msg_type = JSDRV_MSG_TYPE_NORMAL;
    m->topic[0] = 0;
    m->source = 0;
    m->u32_a = 0;
    m->u32_b = 0;
    memset(&m->value, 0, sizeof(m->value));
    m->payload.str[0] = 0;
    memset(&m->extra, 0, sizeof(m->extra));
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    struct jsdrvp_msg_s * m = msg_queue_pop_immediate(context->msg_free_data);
    if (!m) {
        m = jsdrv_alloc_clr(STREAM_MSG_SZ);
        JSDRV_LOGD3("jsdrvp_msg_alloc_data %p sz=%zu", m, STREAM_MSG_SZ);
        jsdrv_list_initialize(&m->item);
    }
    m->inner_msg_type = JSDRV_MSG_TYPE_DATA;
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_bin(&m->payload.bin[0], 0);
    m->u32_a = 0;
    memset(&m->extra, 0, sizeof(m->extra));
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_clone(struct jsdrv_context_s * context, const struct jsdrvp_msg_s * msg_src) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    *m = *msg_src;
    switch (m->value.type) {
        case JSDRV_UNION_JSON:  // intentional fall-through
        case JSDRV_UNION_STR:
            m->value.value.str = m->payload.str;
            break;
        case JSDRV_UNION_BIN:
            m->value.value.bin = m->payload.bin;
            break;
        default:
            break;
    }
    jsdrv_list_initialize(&m->item);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    switch (value->type) {
        case JSDRV_UNION_BIN:
            if (value->size > sizeof(m->payload.bin)) {
                JSDRV_LOGE("bin payload too big");
                jsdrvp_msg_free(context, m);
                return NULL;
            }
            if (value->value.bin) {
                jsdrv_memcpy(m->payload.bin, value->value.bin, value->size);
            }
            m->value.value.bin = m->payload.bin;
            break;
        case JSDRV_UNION_JSON:  /** intentional fall-through. */
        case JSDRV_UNION_STR:
            jsdrv_cstr_copy(m->payload.str, value->value.str, sizeof(m->payload.str));
            m->value.value.str = m->payload.str;
            break;
        default:
            break;
    }
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_str(struct jsdrv_context_s * context, const char * topic, const char * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    jsdrv_cstr_copy(m->payload.str, value, sizeof(m->payload.str));
    m->value = jsdrv_union_cstr_r(m->payload.str);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_i32(struct jsdrv_context_s * context, const char * topic, int32_t value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_i32_r(value);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_u32(struct jsdrv_context_s * context, const char * topic, uint32_t value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_u32_r(value);
    return m;
}

static int32_t timeout_next_ms(struct jsdrv_context_s * c) {
    struct jsdrv_list_s * item;
    struct jsdrvp_api_timeout_s * timeout;
    int64_t t = jsdrv_time_utc();
    item = jsdrv_list_peek_head(&c->cmd_timeouts);
    if (!item) {
        return 1000;  // maximum polling delay
    }
    timeout = JSDRV_CONTAINER_OF(item, struct jsdrvp_api_timeout_s, item);
    int64_t t_delta = timeout->timeout - t;
    if (t_delta <= 0) {
        return 0;
    }
    return (int32_t) JSDRV_TIME_TO_COUNTER(t_delta, 1000LL);
}

static void timeout_add(struct jsdrv_context_s * c, struct jsdrvp_api_timeout_s * timeout) {
    struct jsdrv_list_s * item;
    struct jsdrvp_api_timeout_s * t;
    jsdrv_list_add_tail(&c->cmd_timeouts, &timeout->item);
    jsdrv_list_foreach(&c->cmd_timeouts, item) {
        t = JSDRV_CONTAINER_OF(item, struct jsdrvp_api_timeout_s, item);
        if (timeout->timeout < t->timeout) {
            jsdrv_list_remove(&timeout->item);
            jsdrv_list_insert_before(item, &timeout->item);
            break;
        }
    }
}

static void timeout_process(struct jsdrv_context_s * c) {
    struct jsdrv_list_s * item;
    struct jsdrvp_api_timeout_s * t;
    int64_t t_now = jsdrv_time_utc();
    jsdrv_list_foreach(&c->cmd_timeouts, item) {
        t = JSDRV_CONTAINER_OF(item, struct jsdrvp_api_timeout_s, item);
        if (t->timeout <= t_now) {
            jsdrv_list_remove(item);
            t->return_code = JSDRV_ERROR_TIMED_OUT;
            jsdrv_os_event_signal(t->ev);
        } else {
            break;
        }
    }
}

static int32_t timeout_complete(struct jsdrv_context_s * c, const char * topic, int32_t rc) {
    struct jsdrv_list_s * item;
    struct jsdrvp_api_timeout_s * t;
    JSDRV_LOGI("timeout_complete %s %d", topic, rc);

    jsdrv_list_foreach(&c->cmd_timeouts, item) {
        t = JSDRV_CONTAINER_OF(item, struct jsdrvp_api_timeout_s, item);
        if (0 == strcmp(t->topic, topic)) {
            jsdrv_list_remove(item);
            t->return_code = rc;
            jsdrv_os_event_signal(t->ev);
            return 0;
        }
    }
    JSDRV_LOGI("timeout_complete not found: %s", topic);
    return JSDRV_ERROR_NOT_FOUND;
}

static uint8_t on_return_code(void * user_data, struct jsdrvp_msg_s * msg) {
    struct jsdrv_context_s * c = (struct jsdrv_context_s *) user_data;
    char value_str[128];
    jsdrv_union_value_to_str(&msg->value, value_str, sizeof(value_str), 1);
    JSDRV_LOGI("on_return_code(%s) %s", msg->topic, value_str);
    if (msg->value.type != JSDRV_UNION_I32) {
        JSDRV_LOGW("on_return_code %s unsupported type %d", msg->topic, msg->value.type);
        return 0;
    }

    timeout_complete(c, msg->topic,msg->value.value.i32);
    return 0;
}

static void subscribe_return_code(struct jsdrv_context_s * c) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(c);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_SUBSCRIBE, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, "", sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.internal_fn = on_return_code;
    m->payload.sub.subscriber.user_data = c;
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.flags = JSDRV_SFLAG_RETURN_CODE;
    jsdrv_pubsub_publish(c->pubsub, m);
}

static bool is_backend_initialization_complete(struct jsdrv_context_s * c) {
    for (uint32_t i = 0; i < BACKEND_COUNT_MAX; ++i) {
        if (c->backends[i] && (c->backends[i]->status == JSDRVBK_STATUS_INITIALIZING)) {
            return false;
        }
    }
    return true;
}

static void init_complete(struct jsdrv_context_s * c) {
    JSDRV_ASSERT(c->state == ST_INIT_AWAITING_BACKEND);
    if (is_backend_initialization_complete(c)) {
        c->args = NULL;  // only valid for the duration of INITIALIZE
        c->state = ST_ACTIVE;
        JSDRV_LOGI("init_complete");
        timeout_complete(c, JSDRV_MSG_INITIALIZE "#", c->init_status);
    }
}

static bool handle_cmd_msg(struct jsdrv_context_s * c, struct jsdrvp_msg_s * msg) {
    if (!msg) {
        return false;
    }
    JSDRV_LOGI("handle_cmd_msg %s", msg->topic);
    if (msg->timeout) {
        timeout_add(c, msg->timeout);
        msg->timeout = NULL;
    }

    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_INITIALIZE, msg->topic)) {
            if (c->state == ST_INIT_AWAITING_FRONTEND) {
                c->state = ST_INIT_AWAITING_BACKEND;
                init_complete(c);
            }
            jsdrvp_msg_free(c, msg);
            return true;
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            // all timeouts, including this one, expire on finalize
            jsdrvp_msg_free(c, msg);
            JSDRV_LOGI("USB backend finalize");
            c->do_exit = true;
            return false;
        }
    }
    jsdrv_pubsub_publish(c->pubsub, msg);  // msg ownership relinquished
    return true;
}

static struct frontend_dev_s * device_lookup(struct jsdrv_context_s * c, const char * topic) {
    struct jsdrv_topic_s t;
    struct frontend_dev_s * d = NULL;
    struct jsdrv_list_s * item;

    jsdrv_topic_set(&t, topic);
    int count = 0;
    int i = 0;
    for (i = 0; t.topic[i]; ++i) {
        if (t.topic[i] == '/') {
            ++count;
            if (count == 3) {
                break;
            }
        }
    }
    t.topic[i] = 0;

    jsdrv_list_foreach(&c->devices, item) {
        d = JSDRV_CONTAINER_OF(item, struct frontend_dev_s, item);
        if (0 == strcmp(d->prefix, t.topic)) {
            return d;
        }
    }
    JSDRV_LOGW("device_lookup(%s) => %s failed", topic, t.topic);
    return NULL;
}

static uint8_t device_subscriber(void * user_data, struct jsdrvp_msg_s * msg) {
    struct frontend_dev_s * d = (struct frontend_dev_s *) user_data;
    struct jsdrvp_msg_s * m = jsdrvp_msg_clone(d->context, msg);
    msg_queue_push(d->device->cmd_q, m);
    return 0;
}

static void device_sub(struct frontend_dev_s * d, const char * op) {
    struct jsdrvp_msg_s * sub_msg = jsdrvp_msg_alloc(d->context);
    jsdrv_cstr_copy(sub_msg->topic, op, sizeof(sub_msg->topic));
    sub_msg->value.type = JSDRV_UNION_BIN;
    sub_msg->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    sub_msg->value.value.bin = (uint8_t *) &sub_msg->payload.sub;
    jsdrv_cstr_copy(sub_msg->payload.sub.topic, d->prefix, sizeof(sub_msg->payload.sub.topic));
    sub_msg->payload.sub.subscriber.is_internal = 1;
    sub_msg->payload.sub.subscriber.internal_fn = device_subscriber;
    sub_msg->payload.sub.subscriber.user_data = d;
    sub_msg->payload.sub.subscriber.flags = JSDRV_SFLAG_PUB;
    jsdrv_pubsub_publish(d->context->pubsub, sub_msg);
}

static void device_prefix_to_model(const char * prefix, char * model) {
    const char * p = prefix;
    while (*p && *p != '/') ++p;
    while (*p && *p == '/') ++p;
    while (*p && *p != '/') {
        *model++ = *p++;
    }
    *model = 0;
}

static void device_add_msg(struct jsdrv_context_s * c, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(c && msg);
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    JSDRV_ASSERT(msg->value.app == JSDRV_PAYLOAD_TYPE_DEVICE);
    JSDRV_LOGI("device add sn=%s", msg->payload.device.prefix);
    char model[JSDRV_TOPIC_LENGTH_MAX];
    device_prefix_to_model(msg->payload.device.prefix, model);

    struct frontend_dev_s * d = jsdrv_alloc_clr(sizeof(struct frontend_dev_s));
    d->context = c;
    jsdrv_list_initialize(&d->item);
    jsdrv_cstr_copy(d->prefix, msg->payload.device.prefix, sizeof(d->prefix));
    jsdrv_list_add_tail(&c->devices, &d->item);

    int rv;
    if (0 == strcmp("js220", model))  {
        rv = jsdrvp_ul_js220_usb_factory(&d->device, c, &msg->payload.device);
    } else if (0 == strcmp("js110", model)) {
        rv = jsdrvp_ul_js110_usb_factory(&d->device, c, &msg->payload.device);
#if UNITTEST == 0
    //} else if (0 == strcmp("emu", model)) {
    //    rv = jsdrvp_ul_emu_factory(&d->device, c, &msg->payload.device);
#endif
    }
    if (rv) {
        JSDRV_LOGE("device_add failed with %d", rv);
        jsdrvp_msg_free(c, msg);
        jsdrv_free(d);
        // todo indicate device failure?
        return;
    }

    msg->value.flags = 0;
    jsdrv_pubsub_publish(c->pubsub, msg);  // transfers msg ownership
    device_sub(d, JSDRV_PUBSUB_SUBSCRIBE);
}

static void device_remove(struct jsdrv_context_s * c, struct frontend_dev_s * d) {
    (void) c;
    if (!d) {
        return;
    }
    JSDRV_LOGI("device remove sn=%s", d->prefix);
    d->device->join(d->device);
    device_sub(d, JSDRV_PUBSUB_UNSUBSCRIBE);

    // todo update state

    jsdrv_list_remove(&d->item);
    jsdrv_free(d);
}

static void device_remove_all(struct jsdrv_context_s * c) {
    while (!jsdrv_list_is_empty(&c->devices)) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(&c->devices);
        struct frontend_dev_s * d = JSDRV_CONTAINER_OF(item, struct frontend_dev_s, item);
        device_remove(c, d);
    }
}

static void device_remove_msg(struct jsdrv_context_s * c, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(c && msg);
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_STR);
    JSDRV_ASSERT(msg->value.app == 0);

    struct frontend_dev_s * d = device_lookup(c, msg->value.value.str);
    if (!d) {
        JSDRV_LOGW("device remove sn=%s, but not found", msg->value.value.str);
        jsdrvp_msg_free(c, msg);
        return;
    }
    device_remove(c, d);
    jsdrv_pubsub_publish(c->pubsub, msg);  // transfers msg ownership
}

static void handle_backend_init_msg(struct jsdrv_context_s * c, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(c && msg);
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_I32);
    char prefix = msg->payload.str[0];
    if ((prefix >= BACKEND_COUNT_MAX) || !c->backends[prefix]) {
        JSDRV_LOGE("invalid backend: %d = %c", (int) prefix, prefix);
    } else {
        JSDRV_LOGI("backend '%c' initialization complete: %ld", prefix, msg->value.value.i32);
        if (msg->value.value.i32) {
            if (c->init_status == 0) {
                c->init_status = msg->value.value.i32;  // store first backend error code
            }
            c->backends[prefix]->status = JSDRVBK_STATUS_FATAL_ERROR;
        } else {
            c->backends[prefix]->status = JSDRVBK_STATUS_READY;
        }
    }
    jsdrvp_msg_free(c, msg);
    if (c->state == ST_INIT_AWAITING_BACKEND) {
        init_complete(c);
    }
}

static void device_list_publish(struct jsdrv_context_s * c) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(c, JSDRV_MSG_DEVICE_LIST, &jsdrv_union_cstr_r(""));
    char * p = m->payload.str;
    char * p_end = m->payload.str + sizeof(m->payload.str) - JSDRV_TOPIC_LENGTH_MAX - 2;
    struct jsdrv_list_s * item;
    struct frontend_dev_s * d;
    bool first = true;
    jsdrv_list_foreach(&c->devices, item) {
        if (p > p_end) {
            JSDRV_LOGW("too may devices to publish list");
            break;
        }
        d = JSDRV_CONTAINER_OF(item, struct frontend_dev_s, item);
        if (!first) {
            *p++ = ',';
        }
        first = false;
        char * prefix = d->prefix;
        while (*prefix) {
            *p++ = *prefix++;
        }
    }
    *p++ = 0;
    m->value.size = (uint32_t) (p - m->payload.str);
    jsdrv_pubsub_publish(c->pubsub, m);  // transfers msg ownership
}

static bool handle_backend_msg(struct jsdrv_context_s * c, struct jsdrvp_msg_s * msg) {
    if (!msg) {
        return false;
    }
    JSDRV_LOGD3("handle_backend_msg %s", msg->topic);
    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_DEVICE_ADD, msg->topic)) {
            device_add_msg(c, msg);
            device_list_publish(c);
        } else if (0 == strcmp(JSDRV_MSG_DEVICE_REMOVE, msg->topic)) {
            device_remove_msg(c, msg);
            device_list_publish(c);
        } else if (0 == strcmp(JSDRV_MSG_INITIALIZE, msg->topic)) {
            handle_backend_init_msg(c, msg);
        } else {
            JSDRV_LOGW("unhandled %s", msg->topic);
        }
    } else {
        switch (msg->inner_msg_type) {
            case JSDRV_MSG_TYPE_NORMAL: break;
            case JSDRV_MSG_TYPE_DATA: break;
            default:
                JSDRV_LOGE("invalid message type - data corruption?");
                jsdrvp_msg_free(c, msg);
                return true;
        }
        struct frontend_dev_s * d = device_lookup(c, msg->topic);
        if (d) {
            msg->extra.frontend.subscriber.internal_fn = device_subscriber;
            msg->extra.frontend.subscriber.user_data = d;
            msg->extra.frontend.subscriber.is_internal = 1;
        } else {
            JSDRV_LOGW("no device match for %s", msg->topic);
        }
        jsdrv_pubsub_publish(c->pubsub, msg);
    }
    return true;
}

static void backends_finalize(struct jsdrv_context_s * c) {
    for (uint32_t i = 0; i < BACKEND_COUNT_MAX; ++i) {
        if (c->backends[i]) {
            c->backends[i]->finalize(c->backends[i]);
            c->backends[i] = NULL;
        }
    }
}

static void timeouts_finalize(struct jsdrv_context_s * c) {
    struct jsdrv_list_s * item;
    struct jsdrvp_api_timeout_s * timeout;
    while (1) {
        item = jsdrv_list_remove_head(&c->cmd_timeouts);
        if (!item) {
            break;
        }
        timeout = JSDRV_CONTAINER_OF(item, struct jsdrvp_api_timeout_s, item);
        timeout->return_code = JSDRV_ERROR_ABORTED;
        jsdrv_os_event_signal(timeout->ev);
    }
}

static int32_t backend_init(struct jsdrv_context_s * c, jsdrv_backend_factory factory) {
    struct jsdrvbk_s * backend;
    if (factory(c, &backend)) {
        JSDRV_LOGE("backend factory failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    backend->status = JSDRVBK_STATUS_INITIALIZING;
    if (backend->prefix > BACKEND_COUNT_MAX) {
        JSDRV_LOGE("invalid backend prefix value: ord=%d", (int) backend->prefix);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (c->backends[backend->prefix]) {
        JSDRV_LOGE("duplicate backend prefix: %c", backend->prefix);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    c->backends[backend->prefix] = backend;
    return 0;
}

#define BACKEND_INIT(context_, factory_)                    \
    if (backend_init(context_, factory_)) {                 \
        JSDRV_LOGE("backend failed to initialize");         \
        jsdrv_finalize(context_, 0);                        \
        THREAD_RETURN();                                    \
    }                                                       \

static THREAD_RETURN_TYPE frontend_thread(THREAD_ARG_TYPE lpParam) {
    struct jsdrv_context_s * c = (struct jsdrv_context_s *) lpParam;
    int32_t timeout_ms;
    JSDRV_LOGI("USB frontend thread started");
    subscribe_return_code(c);

#if _WIN32
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count = 0;
    handles[handle_count++] = msg_queue_handle_get(c->msg_backend);
    handles[handle_count++] = msg_queue_handle_get(c->msg_cmd);
#else
    struct pollfd fds[2];
    fds[0].fd = msg_queue_handle_get(c->msg_backend);
    fds[0].events = POLLIN;
    fds[1].fd = msg_queue_handle_get(c->msg_cmd);
    fds[1].events = POLLIN;
#endif

#if UNITTEST
    BACKEND_INIT(c, jsdrv_unittest_backend_factory);
#else
    BACKEND_INIT(c, jsdrv_usb_backend_factory);
    // todo BACKEND_INIT(c, jsdrv_emulation_backend_factory);
#endif

    while (!c->do_exit) {
        timeout_ms = timeout_next_ms(c);
#if _WIN32
        WaitForMultipleObjects(handle_count, handles, false, timeout_ms);
#else
        poll(fds, 2, timeout_ms);
#endif
        //JSDRV_LOGD3("frontend_thread");
        // note: ResetEvent handled automatically by msg_queue_pop_immediate
        while (handle_backend_msg(c, msg_queue_pop_immediate(c->msg_backend))) {
            ; //
        }
        // note: ResetEvent handled automatically by msg_queue_pop_immediate
        while (handle_cmd_msg(c, msg_queue_pop_immediate(c->msg_cmd))) {
            ; //
        }
        jsdrv_pubsub_process(c->pubsub);
        timeout_process(c);
    }

    device_remove_all(c);
    backends_finalize(c);
    timeouts_finalize(c);
    THREAD_RETURN();
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    if (!msg) {
        return;  // NULL pointer, do nothing.
    }
    if (!jsdrv_list_is_empty(&msg->item)) {
        JSDRV_LOGW("jsdrvp_msg_free but still in list");
    }
    if (msg->value.flags & JSDRV_UNION_FLAG_HEAP_MEMORY) {
        switch (msg->value.type) {
            case JSDRV_UNION_STR:   /* intentional fall-through */
            case JSDRV_UNION_JSON:  /* intentional fall-through */
            case JSDRV_UNION_BIN:   /* intentional fall-through */
                if (msg->value.value.bin) {
                    jsdrv_free((void *) msg->value.value.bin);
                    msg->value.value.bin = NULL;
                }
                break;
            default:
                JSDRV_LOGE("Unexpected type uses heap, ignoring but could cause memory leak.");
                break;
        }
    }
    if (context->do_exit) {
        jsdrv_free(msg);
    } else if (msg->inner_msg_type == JSDRV_MSG_TYPE_DATA) {
        msg_queue_push(context->msg_free_data, msg);
    } else if (msg->inner_msg_type == JSDRV_MSG_TYPE_NORMAL) {
        msg_queue_push(context->msg_free, msg);
    } else {
        JSDRV_LOGE("corrupted message with invalid inner_msg_type");
        jsdrv_free(msg);
    }
}

static int32_t api_cmd(struct jsdrv_context_s * context, struct jsdrvp_msg_s * m, uint32_t timeout_ms) {
    struct jsdrvp_api_timeout_s timeout;
    volatile int32_t rc = 0;
    if (timeout_ms) {
        jsdrv_list_initialize(&timeout.item);
        jsdrv_cstr_join(timeout.topic, m->topic, "#", sizeof(timeout.topic));
        timeout.timeout = jsdrv_time_utc() + timeout_ms * JSDRV_TIME_MILLISECOND;
        timeout.ev = jsdrv_os_event_alloc();
        timeout.return_code = 0;
        m->timeout = &timeout;
        m->source = 1;
    }
    JSDRV_LOGD1("api_cmd(%s) start", m->topic);
    msg_queue_push(context->msg_cmd, m);
    if (timeout_ms) {
#if _WIN32
        switch (WaitForSingleObject(timeout.ev, INFINITE)) {  // timeout performed in main jsdrv thread
            case WAIT_ABANDONED: rc = JSDRV_ERROR_ABORTED; break;
            case WAIT_OBJECT_0: rc = timeout.return_code; break;
            case WAIT_TIMEOUT: rc = JSDRV_ERROR_TIMED_OUT; break;
            case WAIT_FAILED: rc = JSDRV_ERROR_UNSPECIFIED; break;
            default: rc = JSDRV_ERROR_UNSPECIFIED; break;
        }
#else
        struct pollfd fds = {
                .fd = timeout.ev->fd_poll,
                .events = timeout.ev->events,
                .revents = 0,
        };
        int prv = poll(&fds, 1, 1000000);  // timeout > main jsdrv thread timeout
        if (prv < 0) {
            rc = JSDRV_ERROR_UNSPECIFIED;
        } else if (prv == 0) {
            rc = JSDRV_ERROR_TIMED_OUT;
        } else {
            rc = timeout.return_code;
        }
#endif
        m->timeout = NULL;
        jsdrv_os_event_free(timeout.ev);
    }
    JSDRV_LOGD1("api_cmd(%s) done %lu", m->topic, rc);
    return rc;
}

int32_t jsdrv_publish(struct jsdrv_context_s * context,
        const char * name, const struct jsdrv_union_s * value,
        uint32_t timeout_ms) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, name, sizeof(m->topic));
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
                JSDRV_LOGI("publish %s size %lu using heap", name, value->size);
                uint8_t * ptr = jsdrv_alloc(value->size);
                if (ptr) {
                    jsdrvp_msg_free(context, m);
                    return JSDRV_ERROR_NOT_ENOUGH_MEMORY;
                }
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
    return api_cmd(context, m, timeout_ms);
}

int32_t jsdrv_query(struct jsdrv_context_s * context,
                    const char * topic, struct jsdrv_union_s * value,
                    uint32_t timeout_ms) {
    if (!topic || !topic[0] || !value) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (!timeout_ms) {
        timeout_ms = JSDRV_TIMEOUT_MS_DEFAULT;
    }
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_QUERY, sizeof(m->topic));
    jsdrv_cstr_copy(m->payload.query.topic, topic, sizeof(m->payload.str));
    m->payload.query.value = value;
    return api_cmd(context, m, timeout_ms);
}

static int32_t subscribe_common(struct jsdrv_context_s * p,
        const char * topic, uint8_t flags,
        const char * op, jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
                                uint32_t timeout_ms) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(p);
    jsdrv_cstr_copy(m->topic, op, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic, sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.external_fn = cbk_fn;
    m->payload.sub.subscriber.user_data = cbk_user_data;
    m->payload.sub.subscriber.is_internal = 0;
    m->payload.sub.subscriber.flags = flags;
    JSDRV_LOGD1("subscribe_common(%s, %s)", topic, op);
    return api_cmd(p, m, timeout_ms);
}

int32_t jsdrv_subscribe(struct jsdrv_context_s * context, const char * name, uint8_t flags,
                        jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
                        uint32_t timeout_ms) {
    return subscribe_common(context, name, flags, JSDRV_PUBSUB_SUBSCRIBE, cbk_fn, cbk_user_data, timeout_ms);
}

int32_t jsdrv_unsubscribe(struct jsdrv_context_s * context, const char * name,
                          jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
                          uint32_t timeout_ms) {
    return subscribe_common(context, name, 0, JSDRV_PUBSUB_UNSUBSCRIBE, cbk_fn, cbk_user_data, timeout_ms);
}

int32_t jsdrv_unsubscribe_all(struct jsdrv_context_s * context,
                              jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
                              uint32_t timeout_ms) {
    return subscribe_common(context, "", 0, JSDRV_PUBSUB_UNSUBSCRIBE_ALL, cbk_fn, cbk_user_data, timeout_ms);
}

#define MSG_QUEUE_ALLOC(context_, ptr_)         \
    ptr_ = msg_queue_init();                    \
    if (!ptr_) {                                \
        jsdrv_finalize(context_, 0);            \
        return JSDRV_ERROR_NOT_ENOUGH_MEMORY;     \
    }

#define MSG_QUEUE_FREE(ptr_)                    \
    if (ptr_) {                                 \
        msg_queue_finalize(ptr_);               \
        ptr_ = NULL;                            \
    }

int32_t jsdrv_initialize(struct jsdrv_context_s ** context, const struct jsdrv_arg_s * args, uint32_t timeout_ms) {
    JSDRV_LOGI("jsdrv_initialize: start");
    struct jsdrv_context_s * c = jsdrv_alloc_clr(sizeof(struct jsdrv_context_s));
    c->args = args;
    c->state = ST_INIT_AWAITING_FRONTEND;
    c->init_status = 0;
    jsdrv_list_initialize(&c->devices);
    jsdrv_list_initialize(&c->cmd_timeouts);

    MSG_QUEUE_ALLOC(c, c->msg_free);
    MSG_QUEUE_ALLOC(c, c->msg_free_data);
    MSG_QUEUE_ALLOC(c, c->msg_cmd);
    MSG_QUEUE_ALLOC(c, c->msg_backend);
    c->pubsub = jsdrv_pubsub_initialize(c);
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_u32(c, JSDRV_MSG_VERSION, JSDRV_VERSION_U32);
    jsdrv_pubsub_publish(c->pubsub, msg);
    msg = jsdrvp_msg_alloc_str(c, JSDRV_MSG_DEVICE_LIST, "");  // start with empty device list
    jsdrv_pubsub_publish(c->pubsub, msg);
    jsdrv_pubsub_process(c->pubsub);
    int32_t rv = jsdrv_thread_create(&c->thread, frontend_thread, c);
    if (rv) {
        jsdrv_finalize(c, 0);
        return rv;
    }

    msg = jsdrvp_msg_alloc_u32(c, JSDRV_MSG_INITIALIZE, 0);
    timeout_ms = timeout_ms ? timeout_ms : JSDRV_TIMEOUT_MS_INIT;
    *context = c;
    rv = api_cmd(c, msg, timeout_ms);
    JSDRV_LOGI("jsdrv_initialize: return %ld", rv);
    return rv;
}

void jsdrv_finalize(struct jsdrv_context_s * context, uint32_t timeout_ms) {
    JSDRV_LOGI("jsdrv_finalize %p", context);
    struct jsdrv_context_s * c = context;
    if (c && (NULL != context->msg_cmd)) {
        // send exit message to thread
        context->do_exit = true;
        struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc(context);
        jsdrv_cstr_copy(msg->topic, JSDRV_MSG_FINALIZE, sizeof(msg->topic));
        msg_queue_push(context->msg_cmd, msg);
        jsdrv_thread_join(&context->thread, API_TIMEOUT_MS);
        jsdrv_pubsub_finalize(c->pubsub);
        c->pubsub = NULL;

        MSG_QUEUE_FREE(c->msg_cmd);
        MSG_QUEUE_FREE(c->msg_backend);
        MSG_QUEUE_FREE(c->msg_free);
        MSG_QUEUE_FREE(c->msg_free_data);

        jsdrv_free(c);
    }
}

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    char buf[32];
    jsdrv_union_value_to_str(&msg->value, buf, (uint32_t) sizeof(buf), 1);
    if (context->msg_backend) {
        JSDRV_LOGI("jsdrvp_backend_send %s %s", msg->topic, buf);
        msg_queue_push(context->msg_backend, msg);
    } else {  // should never happen
        JSDRV_LOGW("jsdrvp_backend_send but no backend queue!");
        // memory leak
    }
}

void jsdrvp_send_finalize_msg(struct jsdrv_context_s * context, struct msg_queue_s * q, const char * topic) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(msg->topic, JSDRV_MSG_FINALIZE, sizeof(msg->topic));
    msg->value.type = JSDRV_UNION_STR;
    msg->value.value.str = msg->payload.str;
    jsdrv_cstr_copy(msg->payload.str, topic, sizeof(msg->payload.str));
    msg_queue_push(q, msg);
}

int32_t jsdrv_open(struct jsdrv_context_s * context, const char * device_prefix, int32_t mode) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device_prefix);
    jsdrv_topic_append(&t, JSDRV_MSG_OPEN);
    return jsdrv_publish(context, t.topic, &jsdrv_union_i32(mode), JSDRV_TIMEOUT_MS_DEFAULT);
}

int32_t jsdrv_close(struct jsdrv_context_s * context, const char * device_prefix) {
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device_prefix);
    jsdrv_topic_append(&t, JSDRV_MSG_CLOSE);
    return jsdrv_publish(context, t.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT);
}
