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

#include "jsdrv_prv/buffer.h"
#include "jsdrv_prv/buffer_signal.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv.h"
#include "tinyprintf.h"


JSDRV_STATIC_ASSERT(20 == sizeof(struct jsdrv_summary_entry_s), entry_size_one);
JSDRV_STATIC_ASSERT(40 == sizeof(struct jsdrv_summary_entry_s[2]), entry_size_two);
JSDRV_STATIC_ASSERT(JSDRV_BUFSIG_COUNT_MAX <= 256, bufsig_fits_in_u8); // assumed for add/remove/list operations


static const char * action_add_meta = "{"
    "\"dtype\": \"u8\","
    "\"brief\": \"Add a memory buffer.\","  // any u8 value between 1 and 16, inclusive
"}";

static const char * action_remove_meta = "{"
    "\"dtype\": \"u8\","
    "\"brief\": \"Remove a memory buffer.\","
"}";

static const char * action_list_meta = "{"
    "\"dtype\": \"bin\","
    "\"brief\": \"The list of available buffers, 0 terminated.\","
"}";

/*
static const char * event_signal_add_meta = "{"
    "\"dtype\": \"str\","
    "\"brief\": \"Add a signal.\","
"}";

static const char * event_signal_remove_meta = "{"
    "\"dtype\": \"str\","
    "\"brief\": \"Remove a signal.\","
"}";
*/

struct buffer_s {
    uint8_t idx;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_context_s * context;
    uint64_t size;
    struct msg_queue_s * cmd_q;
    struct jsdrv_list_s pending;
    jsdrv_thread_t thread;
    volatile uint8_t do_exit;
    struct bufsig_s signals[JSDRV_BUFSIG_COUNT_MAX];  // 0 is reserved
};

struct buffer_mgr_s {
    struct jsdrv_context_s * context;
    struct buffer_s buffers[JSDRV_BUFFER_COUNT_MAX];
};


struct buffer_mgr_s instance_;

static uint8_t _buffer_recv_data(void * user_data, struct jsdrvp_msg_s * msg);

static void send_to_frontend(struct buffer_mgr_s * self, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, topic, value);
    jsdrvp_backend_send(self->context, m);
}

static int32_t subscribe(struct jsdrv_context_s * context, const char * topic, uint8_t flags,
                         jsdrv_pubsub_subscribe_fn cbk_fn, void * cbk_user_data) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_SUBSCRIBE, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic, sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.internal_fn = cbk_fn;
    m->payload.sub.subscriber.user_data = cbk_user_data;
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.flags = flags;
    jsdrvp_backend_send(context, m);
    return 0;
}

static int32_t unsubscribe(struct jsdrv_context_s * context, const char * topic, uint8_t flags,
                           jsdrv_pubsub_subscribe_fn cbk_fn, void * cbk_user_data) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_UNSUBSCRIBE, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic, sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.internal_fn = cbk_fn;
    m->payload.sub.subscriber.user_data = cbk_user_data;
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.flags = flags;
    jsdrvp_backend_send(context, m);
    return 0;
}

static void bufsig_unsub(struct bufsig_s * b) {
    if (b->topic[0]) {
        unsubscribe(b->parent->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv_data, b);
        b->topic[0] = 0;
    }
}

static void bufsig_sub(struct bufsig_s * b, const char * topic) {
    bufsig_unsub(b);
    jsdrv_cstr_copy(b->topic, topic, sizeof(b->topic));
    subscribe(b->parent->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv_data, b);
}

static void buf_publish_signal_list(struct buffer_s * self) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, "", &jsdrv_union_cbin_r(NULL, 0));
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s", self->topic, JSDRV_BUFFER_MSG_LIST);
    for (uint32_t i = 1; i < JSDRV_BUFSIG_COUNT_MAX; ++i) {
        if (self->signals[i].active) {
            m->payload.bin[m->value.size++] = i;
        }
    }
    m->payload.bin[m->value.size++] = 0;
    jsdrvp_backend_send(self->context, m);
}

bool handle_cmd_q(struct buffer_s * self) {
    bool rv = true;
    struct jsdrvp_msg_s * msg = msg_queue_pop_immediate(self->cmd_q);
    char idx_str[JSDRV_TOPIC_LENGTH_MAX];
    // note: ResetEvent handled automatically by msg_queue_pop_immediate
    if (NULL == msg) {
        return false;
    }

    const char * s = msg->topic;
    if (msg->u32_a != 0) {
        // todo should be data from signal source.
    } else if ((s[0] == 'a') && (s[1] == '/')) {
        s += 2;
        struct jsdrv_union_s v = msg->value;
        jsdrv_union_widen(&v);
        uint32_t idx = v.value.u32;
        struct bufsig_s * b = &self->signals[idx];

        if ((idx <= 0) || (idx >= JSDRV_BUFSIG_COUNT_MAX)) {
            JSDRV_LOGW("Invalid signal index: %s", msg->topic);
        } else if (0 == strcmp(s, "!add")) {
            if (b->active) {
                JSDRV_LOGW("signal already active: %u", idx);
            } else {
                b->active = true;
                buf_publish_signal_list(self);
            }
        } else if (0 == strcmp(s, "!remove")) {
            bufsig_unsub(b);
            b->active = false;
            buf_publish_signal_list(self);
        } else {
            JSDRV_LOGW("signal unsupported: %s", s);
        }
    } else if ((s[0] == 's') && (s[1] == '/')) {
        s += 2;
        uint32_t idx = 0;
        char * p = idx_str;
        while (*s != '/') {
            *p++ = *s++;
        }
        *p++ = 0;
        s++;
        if (0 != jsdrv_cstr_to_u32(idx_str, &idx)) {
            JSDRV_LOGW("Could not parse signal index: %s", msg->topic);
        } else if ((idx <= 0) || (idx >= JSDRV_BUFSIG_COUNT_MAX)) {
            JSDRV_LOGW("Invalid signal index: %s", msg->topic);
            // todo validate idx
        } else {
            struct bufsig_s * b = &self->signals[idx];
            if (0 == strcmp(s, "r/!spl")) {
                // todo - insert into pending
            } else if (0 == strcmp(s, "r/!sum")) {
                // todo - insert into pending
            } else if (0 == strcmp(s, "s/topic")) {
                bufsig_sub(b, msg->value.value.str);
            } else {
                JSDRV_LOGW("ignore %s", msg->topic);
            }
        }
    } else if ((s[0] == 'g') && (s[1] == '/')) {
        s += 2;
        if (0 == strcmp(s, "size")) {
            // todo resize
        // todo hold
        // todo mode
        } else {
            JSDRV_LOGW("buffer global unsupported: %s", s);
        }
    } else if (0 == strcmp(s, JSDRV_MSG_FINALIZE)) {
        self->do_exit = 1;
        rv = false;
    } else {
        JSDRV_LOGW("ignore %s", msg->topic);
    }
    jsdrvp_msg_free(self->context, msg);
    return rv;
}

bool handle_pending(struct buffer_s * self) {
    struct jsdrv_list_s * item = jsdrv_list_remove_head(&self->pending);
    if (NULL == item) {
        return false;
    }
    // todo
    return false;
}

static THREAD_RETURN_TYPE buffer_thread(THREAD_ARG_TYPE lpParam) {
    struct buffer_s * self = (struct buffer_s *) lpParam;
    JSDRV_LOGI("buffer thread started: %s", self->topic);

#if _WIN32
    HANDLE handles[1];
    DWORD handle_count;
    handles[0] = msg_queue_handle_get(self->cmd_q);
    handle_count = 1;
#else
    struct pollfd fds[1];
    fds[0].fd = msg_queue_handle_get(self->cmd_q);
    fds[0].events = POLLIN;
#endif

    while (!self->do_exit) {
#if _WIN32
        WaitForMultipleObjects(handle_count, handles, false, 100);
#else
        poll(fds, 1, 100);
#endif
        JSDRV_LOGD2("buffer thread tick");
        do {
            while (handle_cmd_q(self)) { ;
            }
            handle_pending(self);
        } while (!self->do_exit && !jsdrv_list_is_empty(&self->pending));
    }

    JSDRV_LOGI("buffer thread done: %s", self->topic);
    THREAD_RETURN();
}

static void _send_buffer_list(struct buffer_mgr_s * self) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_LIST, &jsdrv_union_cbin_r(NULL, 0));
    for (uint8_t i = 0; i < JSDRV_BUFFER_COUNT_MAX; ++i) {
        if (NULL != self->buffers[i].cmd_q) {
            m->payload.bin[m->value.size++] = i + 1;
        }
    }
    m->payload.bin[m->value.size++] = 0;
    jsdrvp_backend_send(self->context, m);
}

static uint8_t _buffer_recv(void * user_data, struct jsdrvp_msg_s * msg) {
    // topic should be "m/xxx/..."
    struct buffer_s * b = (struct buffer_s *) user_data;
    if (!jsdrv_cstr_starts_with(msg->topic, b->topic)) {
        JSDRV_LOGE("unexpected topic %s to %s", msg->topic, b->topic);
        return 1;
    }
    struct jsdrvp_msg_s * m = jsdrvp_msg_clone(b->context, msg);
    jsdrv_cstr_copy(m->topic, msg->topic + 6, sizeof(m->topic));
    m->u32_a = 0;  // signal_id=0 (invalid), for main processing
    msg_queue_push(b->cmd_q, m);
    return 0;
}

static uint8_t _buffer_recv_data(void * user_data, struct jsdrvp_msg_s * msg) {
    // topic should be "m/xxx/..."
    struct bufsig_s * b = (struct bufsig_s *) user_data;
    struct jsdrvp_msg_s * m = jsdrvp_msg_clone(b->parent->context, msg);
    m->u32_a = b->idx;  // signal_id=0 (invalid), for main processing
    msg_queue_push(b->parent->cmd_q, m);
    return 0;
}

static uint8_t _buffer_add(void * user_data, struct jsdrvp_msg_s * msg) {
    struct buffer_mgr_s * self = (struct buffer_mgr_s *) user_data;
    struct jsdrv_union_s v = msg->value;
    jsdrv_union_widen(&v);
    uint64_t buffer_id_i64 = v.value.i64;
    if ((buffer_id_i64 < 1) || (buffer_id_i64 > JSDRV_BUFFER_COUNT_MAX)) {
        JSDRV_LOGE("buffer_id %llu invalid", buffer_id_i64);
        return 1;
    }

    uint8_t buffer_id = (uint8_t) buffer_id_i64;
    struct buffer_s * b = &self->buffers[buffer_id - 1];
    if (NULL != b->cmd_q) {
        JSDRV_LOGE("buffer_id %u already exists", buffer_id);
        return 1;
    }
    JSDRV_LOGI("buffer_id %u add", buffer_id);
    memset(b, 0, sizeof(*b));
    tfp_snprintf(b->topic, sizeof(b->topic), "m/%03u", buffer_id);
    b->context = self->context;
    b->cmd_q = msg_queue_init();
    subscribe(b->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv, b);
    jsdrv_list_initialize(&b->pending);
    for (uint32_t i = 0; i < JSDRV_BUFSIG_COUNT_MAX; i++) {
        struct bufsig_s * s = &b->signals[i];
        s->idx = i;
        s->parent = b;
        s->active = false;
        s->topic[0] = 0;
    }

    if (jsdrv_thread_create(&b->thread, buffer_thread, b)) {
        JSDRV_LOGE("buffer_id %u thread create failed", buffer_id);
        return 1;
    }

    _send_buffer_list(self);
    return 0;
}

static void _buffer_remove_inner(struct buffer_mgr_s * self, uint8_t buffer_id) {
    struct buffer_s * b = &self->buffers[buffer_id - 1];
    if (NULL == b->cmd_q) {
        JSDRV_LOGE("buffer_id %u does not exist", buffer_id);
        return;
    }
    JSDRV_LOGI("buffer_id %u remove", buffer_id);

    msg_queue_push(b->cmd_q, jsdrvp_msg_alloc_value(self->context, JSDRV_MSG_FINALIZE, &jsdrv_union_u8(0)));
    jsdrv_thread_join(&b->thread, 1000);
    msg_queue_finalize(b->cmd_q);
    b->cmd_q = NULL;
    _send_buffer_list(self);
}

static uint8_t _buffer_remove(void * user_data, struct jsdrvp_msg_s * msg) {
    struct buffer_mgr_s * self = (struct buffer_mgr_s *) user_data;
    struct jsdrv_union_s v = msg->value;
    jsdrv_union_widen(&v);
    uint64_t buffer_id_i64 = v.value.i64;
    if ((buffer_id_i64 < 1) || (buffer_id_i64 > JSDRV_BUFFER_COUNT_MAX)) {
        JSDRV_LOGE("invalid buffer_id: %llu", buffer_id_i64);
        return 1;
    }
    _buffer_remove_inner(self, (uint8_t) buffer_id_i64);
    return 0;
}

int32_t jsdrv_buffer_initialize(struct jsdrv_context_s * context) {
    JSDRV_DBC_NOT_NULL(context);
    struct buffer_mgr_s * self = &instance_;
    memset(self, 0, sizeof(*self));
    self->context = context;

    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_ADD "$", &jsdrv_union_cjson_r(action_add_meta));
    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE "$", &jsdrv_union_cjson_r(action_remove_meta));
    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_LIST "$", &jsdrv_union_cjson_r(action_list_meta));

    subscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_SFLAG_PUB, _buffer_add, self);
    subscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, JSDRV_SFLAG_PUB, _buffer_remove, self);
    _send_buffer_list(self);

    return 0;
}

void jsdrv_buffer_finalize() {
    struct buffer_mgr_s * self = &instance_;
    if (self->context) {
        // todo send thread finalize
        self->context = 0;
    }
}
