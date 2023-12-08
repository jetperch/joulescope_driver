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
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv.h"
#include "tinyprintf.h"
#include <math.h>
#include <inttypes.h>


#define BUFFER_THREAD_WAIT_TIMEOUT_MS  (50)
JSDRV_STATIC_ASSERT(16 == sizeof(struct jsdrv_summary_entry_s), entry_size_one);
JSDRV_STATIC_ASSERT(32 == sizeof(struct jsdrv_summary_entry_s[2]), entry_size_two);
JSDRV_STATIC_ASSERT(JSDRV_BUFSIG_COUNT_MAX <= 256, bufsig_fits_in_u8); // assumed for add/remove/list operations


static const char * action_add_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"Add a memory buffer.\""  // any u8 value between 1 and 16, inclusive
"}";

static const char * action_remove_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"Remove a memory buffer.\""
"}";

static const char * action_list_meta = "{"
    "\"brief\": \"The list of available buffers, 0 terminated.\""
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

enum buffer_state_s {
    ST_IDLE = 0,
    ST_AWAIT,
    ST_ACTIVE,
};

struct req_s {
    uint32_t signal_id;
    struct jsdrv_buffer_request_s req;
    struct jsdrv_list_s item;
};

struct buffer_s {
    uint8_t idx;
    uint8_t hold;
    enum buffer_state_s state;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_context_s * context;
    uint64_t size;
    struct msg_queue_s * cmd_q;
    struct jsdrv_list_s req_pending;
    struct jsdrv_list_s req_free;
    jsdrv_thread_t thread;
    volatile uint8_t do_exit;
    struct bufsig_s signals[JSDRV_BUFSIG_COUNT_MAX];  // 0 is reserved
};

struct buffer_mgr_s {
    struct jsdrv_context_s * context;
    struct buffer_s buffers[JSDRV_BUFFER_COUNT_MAX];
};


static struct buffer_mgr_s instance_;

static uint8_t _buffer_recv(void * user_data, struct jsdrvp_msg_s * msg);
static uint8_t _buffer_recv_data(void * user_data, struct jsdrvp_msg_s * msg);

static bool is_buffer_idx_valid(uint64_t buffer_idx) {
    return ((buffer_idx >= 1) && (buffer_idx <= JSDRV_BUFFER_COUNT_MAX));
}

static bool is_signal_idx_valid(uint64_t signal_idx) {
    return ((signal_idx >= 1) && (signal_idx <= JSDRV_BUFSIG_COUNT_MAX));
}

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
        intptr_t v = ((((intptr_t) (b->idx)) & 0xffff) << 16) | (b->parent->idx & 0xffff);
        unsubscribe(b->parent->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv_data, (void *) v);
        b->topic[0] = 0;
    }
}

static void bufsig_sub(struct bufsig_s * b, const char * topic) {
    bufsig_unsub(b);
    jsdrv_cstr_copy(b->topic, topic, sizeof(b->topic));
    intptr_t v = ((((intptr_t) (b->idx)) & 0xffff) << 16) | (b->parent->idx & 0xffff);
    subscribe(b->parent->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv_data, (void *) v);
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

static bool await_check(struct buffer_s * self) {
    for (uint32_t idx = 1; idx < JSDRV_BUFSIG_COUNT_MAX; ++idx) {
        struct bufsig_s *b = &self->signals[idx];
        if (!b->active) {
            continue;
        } else if (0 == b->hdr.sample_rate) {
            return false;
        }
    }
    return true;
}

static void bufsig_publish_info(struct bufsig_s * self) {
    struct jsdrv_context_s * context = self->parent->context;
    struct jsdrv_buffer_info_s info;
    if (jsdrv_bufsig_info(self, &info)) {
        struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(context, "",
                &jsdrv_union_cbin_r((uint8_t *) &info, sizeof(info)));
        tfp_snprintf(m->topic, sizeof(m->topic), "m/%03d/s/%03d/info", self->parent->idx, self->idx);
        m->value.app = JSDRV_PAYLOAD_TYPE_BUFFER_INFO;
        jsdrvp_backend_send(context, m);
    }
}

static void buffer_alloc(struct buffer_s * self) {
    double coef_f32 = sizeof(float);
    double coef_u = 0.0;
    JSDRV_LOGI("buffer_alloc %" PRIu64, self->size);
    size_t summary_entry_sz = sizeof(struct jsdrv_summary_entry_s);
    for (uint32_t lvl = 1; lvl < 8; ++lvl) {
        double pow_lvl = pow(32.0, lvl - 1);
        coef_f32 += summary_entry_sz / (128 * pow_lvl);
        coef_u += summary_entry_sz / (1024 * pow_lvl);
    }

    // determine size in bytes per second
    double sz_per_s = 0.0;
    for (uint32_t idx = 1; idx < JSDRV_BUFSIG_COUNT_MAX; ++idx) {
        struct bufsig_s *b = &self->signals[idx];
        if (!b->active) {
            continue;
        }
        uint32_t sample_rate = b->hdr.sample_rate / b->hdr.decimate_factor;
        if ((b->hdr.element_type == JSDRV_DATA_TYPE_FLOAT) && (b->hdr.element_size_bits == 32)) {
            sz_per_s +=sample_rate * coef_f32;
        } else {
            sz_per_s += sample_rate * ((b->hdr.element_size_bits / 8.0) + coef_u);
        }
    }
    // determine sample count for each signal, allocate, and publish duration
    double duration = self->size / sz_per_s;
    JSDRV_LOGI("%d B/s -> %d seconds", (int) sz_per_s, (int) duration);
    for (uint32_t idx = 1; idx < JSDRV_BUFSIG_COUNT_MAX; ++idx) {
        struct bufsig_s *b = &self->signals[idx];
        if (!b->active) {
            continue;
        }
        uint32_t sample_rate = b->hdr.sample_rate / b->hdr.decimate_factor;
        double N = duration * sample_rate;
        uint64_t r0;        // power of 2
        uint64_t rN = 32;   // power of 2
        if ((b->hdr.element_type == JSDRV_DATA_TYPE_FLOAT) && (b->hdr.element_size_bits == 32)) {
            r0 = 128;
        } else {
            r0 = 1024;
        }
        int64_t level = (int64_t) (ceil(log2(N / (double) (r0 * (rN * rN - 1))) / log2((double) rN) + 1.0));
        if (level < 1) {
            level = 1;
        }
        uint64_t rZ = r0;
        for (int i = 1; i <= level; ++i) {
            rZ *= rN;
        }
        uint64_t k = (uint64_t) (round(N / rZ));
        if (k == 0) {
            k = 1;
        }
        uint64_t Np = k * rZ;
        jsdrv_bufsig_alloc(b, Np, r0, rN);
        bufsig_publish_info(b);
    }
}

static void buffer_free(struct buffer_s * self) {
    if (self->state == ST_ACTIVE) {
        self->state = ST_AWAIT;
    }
    for (uint32_t idx = 0; idx < JSDRV_BUFSIG_COUNT_MAX; ++idx) {
        struct bufsig_s *b = &self->signals[idx];
        jsdrv_bufsig_clear(b);
        bufsig_publish_info(b);
        jsdrv_bufsig_free(b);
    }
}

static void req_post(struct buffer_s * self, uint32_t bufsig_idx, struct jsdrv_buffer_request_s * req) {
    struct jsdrv_list_s * item;
    struct req_s * r;

    // Search for existing request
    jsdrv_list_foreach(&self->req_pending, item) {
        r = JSDRV_CONTAINER_OF(item, struct req_s, item);
        if ((r->signal_id == bufsig_idx) && (r->req.rsp_id == req->rsp_id) && (0 == strcmp(r->req.rsp_topic, req->rsp_topic))) {
            JSDRV_LOGD1("dedup rsp_id %lld", req->rsp_id);
            // found existing request still pending; update request.
            r->req = *req;
            return;
        }
    }

    // No existing request found; create new request.
    item = jsdrv_list_remove_head(&self->req_free);
    if (NULL != item) {
        r = JSDRV_CONTAINER_OF(item, struct req_s, item);
    } else {
        JSDRV_LOGD1("create request");
        r = jsdrv_alloc_clr(sizeof(struct req_s));
        jsdrv_list_initialize(&r->item);
    }
    r->signal_id = bufsig_idx;
    r->req = *req;
    jsdrv_list_add_tail(&self->req_pending, &r->item);
}

static bool req_handle_one(struct buffer_s * self) {
    struct jsdrv_list_s * item = jsdrv_list_remove_head(&self->req_pending);
    if (NULL == item) {
        return false;
    }
    struct req_s * req = JSDRV_CONTAINER_OF(item, struct req_s, item);
    struct bufsig_s * b = &self->signals[req->signal_id];
    if (!b->active) {
        return false;
    }
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_data(self->context, req->req.rsp_topic);
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) msg->value.value.bin;
    if (jsdrv_bufsig_process_request(b, &req->req, rsp)) {
        jsdrvp_msg_free(self->context, msg);
    } else {
        msg->value.app = JSDRV_PAYLOAD_TYPE_BUFFER_RSP;
        jsdrvp_backend_send(self->context, msg);
        jsdrv_list_add_tail(&self->req_free, item);
    }
    return true;
}

static void req_list_free(struct jsdrv_list_s * list) {
    while (1) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(list);
        if (NULL == item) {
            break;
        }
        struct req_s * req = JSDRV_CONTAINER_OF(item, struct req_s, item);
        jsdrv_free(req);
    }
}

static int32_t send_return_code_to_frontend(struct jsdrv_context_s * context, const char * topic, int32_t rc,
        jsdrv_pubsub_subscribe_fn cbk_fn, void * cbk_user_data) {
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_i32(rc));
    tfp_snprintf(m->topic, sizeof(m->topic), "%s%c", topic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    m->extra.frontend.subscriber.internal_fn = cbk_fn;
    m->extra.frontend.subscriber.user_data = cbk_user_data;
    m->extra.frontend.subscriber.is_internal = 1;
    jsdrvp_backend_send(context, m);
    return rc;
}

static int32_t buffer_recv_complete(struct buffer_s * self, const char * subtopic, int32_t rc) {
    if (rc >= 0) {
        struct jsdrvp_msg_s *m;
        m = jsdrvp_msg_alloc_value(self->context, "", &jsdrv_union_i32(rc));
        tfp_snprintf(m->topic, sizeof(m->topic), "m/%03d/%s%c", self->idx,
                     subtopic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
        m->extra.frontend.subscriber.internal_fn = _buffer_recv;
        m->extra.frontend.subscriber.user_data = NULL;
        m->extra.frontend.subscriber.is_internal = 1;
        jsdrvp_backend_send(self->context, m);
    }
    return rc;
}

static bool handle_cmd_q(struct buffer_s * self) {
    bool rv = true;
    int32_t rc = -1;  // ignored
    struct jsdrvp_msg_s * msg = msg_queue_pop_immediate(self->cmd_q);
    char idx_str[JSDRV_TOPIC_LENGTH_MAX];
    // note: ResetEvent handled automatically by msg_queue_pop_immediate
    if (NULL == msg) {
        return false;
    }

    const char * s = msg->topic;
    if ((msg->u32_a > 0) && (msg->u32_a < JSDRV_BUFSIG_COUNT_MAX)) {
        if ((self->state == ST_ACTIVE) || (self->state == ST_AWAIT)) {
            struct bufsig_s *b = &self->signals[msg->u32_a];
            struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) msg->value.value.bin;
            jsdrv_bufsig_recv_data(b, signal);
            if (self->state == ST_AWAIT) {
                if (await_check(self)) {
                    buffer_alloc(self);
                    self->state = ST_ACTIVE;
                }
            } else {
                bufsig_publish_info(b);
            }
        }
    } else if (msg->u32_a != 0) {
        JSDRV_LOGW("Invalid buffer index: %s", msg->u32_a);
        rc = JSDRV_ERROR_NOT_FOUND;
    } else if ((s[0] == 'a') && (s[1] == '/')) {
        s += 2;
        struct jsdrv_union_s v = msg->value;
        jsdrv_union_widen(&v);
        uint32_t idx = v.value.u32;
        struct bufsig_s * b = &self->signals[idx];

        if ((idx <= 0) || (idx >= JSDRV_BUFSIG_COUNT_MAX)) {
            JSDRV_LOGW("Invalid signal index: %s", msg->topic);
            rc = JSDRV_ERROR_NOT_FOUND;
        } else if (0 == strcmp(s, "!add")) {
            if (b->active) {
                JSDRV_LOGW("signal already active: %u", idx);
                rc = JSDRV_ERROR_BUSY;
            } else {
                JSDRV_LOGI("signal add %d", (int) msg->u32_a);
                buffer_free(self);
                b->active = true;
                buf_publish_signal_list(self);
                rc = 0;
            }
        } else if (0 == strcmp(s, "!remove")) {
            JSDRV_LOGI("signal remove %d", (int) msg->u32_a);
            bufsig_unsub(b);
            b->active = false;
            buffer_free(self);
            buf_publish_signal_list(self);
            rc = 0;
        } else {
            JSDRV_LOGW("signal unsupported: %s", s);
            rc = JSDRV_ERROR_NOT_FOUND;
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
            rc = JSDRV_ERROR_PARAMETER_INVALID;
        } else if ((idx <= 0) || (idx >= JSDRV_BUFSIG_COUNT_MAX)) {
            JSDRV_LOGW("Invalid signal index: %s", msg->topic);
            // todo validate idx
            rc = JSDRV_ERROR_NOT_FOUND;
        } else {
            struct bufsig_s * b = &self->signals[idx];
            if (0 == strcmp(s, "!req")) {
                if (msg->value.app != JSDRV_PAYLOAD_TYPE_BUFFER_REQ) {
                    JSDRV_LOGI("buffer request but app field is %d", (int) msg->value.app);
                }
                req_post(self, idx, (struct jsdrv_buffer_request_s *) msg->value.value.bin);
                rc = 0;
            } else if (0 == strcmp(s, "topic")) {
                JSDRV_LOGI("buffer %d set topic %s", idx, msg->value.value.str);
                bufsig_sub(b, msg->value.value.str);
                rc = 0;
            } else if (0 == strcmp(s, "info")) {
                // published by us, ignore
            } else {
                JSDRV_LOGW("ignore %s", msg->topic);
                rc = JSDRV_ERROR_PARAMETER_INVALID;
            }
        }
    } else if ((s[0] == 'g') && (s[1] == '/')) {
        s += 2;
        if (0 == strcmp(s, "size")) {
            struct jsdrv_union_s v = msg->value;
            jsdrv_union_widen(&v);
            uint64_t sz = v.value.u64;
            JSDRV_LOGI("buffer set size start: %" PRIu64, sz);
            buffer_free(self);
            self->size = sz;
            self->state = (0 == self->size) ? ST_IDLE : ST_AWAIT;
            JSDRV_LOGI("buffer set size done %d: %" PRIu64, self->state, sz);
            rc = 0;
        } else if (0 == strcmp(s, "list")) {
            // published by us, ignore
        } else if (0 == strcmp(s, "hold")) {
            bool bool_v = false;
            jsdrv_union_to_bool(&msg->value, &bool_v);
            self->hold = bool_v ? 1 : 0;
            JSDRV_LOGI("hold %s", self->hold ? "on" : "off");
            rc = 0;
        } else if (0 == strcmp(s, "!clear")) {
            JSDRV_LOGI("clear");
            buffer_free(self);
            rc = 0;
        } else {
            // todo mode circular or single capture
            JSDRV_LOGW("buffer global unsupported: %s", s);
            rc = JSDRV_ERROR_PARAMETER_INVALID;
        }
    } else if (0 == strcmp(s, JSDRV_MSG_FINALIZE)) {
        self->do_exit = 1;
        rv = false;
        rc = 0;
    } else {
        JSDRV_LOGW("ignore %s", msg->topic);
        rc = JSDRV_ERROR_PARAMETER_INVALID;
    }
    buffer_recv_complete(self, msg->topic, rc);
    jsdrvp_msg_free(self->context, msg);
    return rv;
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
        WaitForMultipleObjects(handle_count, handles, false, BUFFER_THREAD_WAIT_TIMEOUT_MS);
#else
        poll(fds, 1, BUFFER_THREAD_WAIT_TIMEOUT_MS);
#endif
        JSDRV_LOGD2("buffer thread tick");
        do {
            while (handle_cmd_q(self)) { ;
            }
            req_handle_one(self);
        } while (!self->do_exit && !jsdrv_list_is_empty(&self->req_pending));
    }

    // Clear all signals.
    for (uint32_t idx = 0; idx < JSDRV_BUFSIG_COUNT_MAX; ++idx) {
        struct bufsig_s * s = &self->signals[idx];
        if (s->topic[0]) {
            bufsig_unsub(s);
            jsdrv_bufsig_clear(s);
            jsdrv_bufsig_free(s);
        }
    }

    req_list_free(&self->req_pending);
    req_list_free(&self->req_free);
    JSDRV_LOGI("buffer thread done: %s", self->topic);
    THREAD_RETURN();
}

static void _send_buffer_list(struct buffer_mgr_s * self) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_LIST, &jsdrv_union_cbin_r(NULL, 0));
    for (uint8_t buffer_idx = 1; buffer_idx <= JSDRV_BUFFER_COUNT_MAX; ++buffer_idx) {
        if (NULL != self->buffers[buffer_idx - 1].cmd_q) {
            m->payload.bin[m->value.size++] = buffer_idx;
        }
    }
    m->payload.bin[m->value.size++] = 0;
    jsdrvp_backend_send(self->context, m);
}

static uint8_t _buffer_recv(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;

    // topic should be "m/xxx/..."
    if (!jsdrv_cstr_starts_with(msg->topic, "m/")) {
        JSDRV_LOGE("unexpected topic %s", msg->topic);
        return 1;
    }
    uint32_t buffer_idx = 0;
    for (char * p = &msg->topic[2]; (*p && (*p != '/')); ++p) {
        if ((*p >= '0') && (*p <= '9')) {
            buffer_idx = buffer_idx * 10 + (*p - '0');
        } else {
            JSDRV_LOGE("buffer_idx parse failed: %s", msg->topic);
            return 1;
        }
    }

    if (!is_buffer_idx_valid(buffer_idx)) {
        JSDRV_LOGE("buffer_idx not valid: %s", msg->topic);
        return 1;
    }

    struct buffer_s * b = &instance_.buffers[buffer_idx - 1];
    if (jsdrv_cstr_ends_with(msg->topic, "!rsp")) {
        // allow external to register to our !rsp topic.
        return 0;
    }
    if (!jsdrv_cstr_starts_with(msg->topic, b->topic)) {
        JSDRV_LOGE("unexpected topic %s to %s", msg->topic, b->topic);
        return 1;
    }

    if (NULL != b->cmd_q) {
        struct jsdrvp_msg_s * m = jsdrvp_msg_clone(b->context, msg);
        jsdrv_cstr_copy(m->topic, msg->topic + 6, sizeof(m->topic));
        m->u32_a = 0;  // signal_id=0 (invalid), for main processing
        msg_queue_push(b->cmd_q, m);
    }
    return 0;
}

static uint8_t _buffer_recv_data(void * user_data, struct jsdrvp_msg_s * msg) {
    intptr_t v = (intptr_t) user_data;
    uint32_t buffer_idx = v & 0xffff;
    uint32_t signal_idx = (v >> 16) & 0xffff;

    if (!is_buffer_idx_valid(buffer_idx)) {
        JSDRV_LOGE("buffer_idx not valid: %s", msg->topic);
        return 1;
    }
    if (!is_signal_idx_valid(signal_idx)) {
        JSDRV_LOGE("signal_idx not valid: %s", msg->topic);
        return 1;
    }

    struct bufsig_s * b = &instance_.buffers[buffer_idx - 1].signals[signal_idx];
    struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) msg->value.value.bin;
    if (signal->element_count == 0) {
        JSDRV_LOGW("empty stream signal message");
    } else if (NULL == b->parent->cmd_q) {
        // discard
    } else if (0 == b->parent->hold) {
        struct jsdrvp_msg_s * m = jsdrvp_msg_clone(b->parent->context, msg);
        m->u32_a = b->idx;  // signal_id=0 (invalid), for main processing
        msg_queue_push(b->parent->cmd_q, m);
    }
    return 0;
}

static uint8_t _buffer_add(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    struct buffer_mgr_s * self = &instance_;
    struct jsdrv_union_s v = msg->value;
    jsdrv_union_widen(&v);
    uint64_t buffer_id_u64 = v.value.u64;
    if (!is_buffer_idx_valid(buffer_id_u64)) {
        JSDRV_LOGE("buffer_id %llu invalid", buffer_id_u64);
        return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_ERROR_PARAMETER_INVALID, _buffer_add, NULL);
    }

    uint8_t buffer_id = (uint8_t) buffer_id_u64;
    struct buffer_s * b = &self->buffers[buffer_id - 1];
    if (NULL != b->cmd_q) {
        JSDRV_LOGE("buffer_id %u already exists", buffer_id);
        return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_ERROR_ALREADY_EXISTS, _buffer_add, NULL);
    }
    JSDRV_LOGI("buffer_id %u add", buffer_id);
    memset(b, 0, sizeof(*b));
    b->idx = buffer_id;
    b->hold = 0;
    b->state = ST_IDLE;
    tfp_snprintf(b->topic, sizeof(b->topic), "m/%03u", buffer_id);
    b->context = self->context;
    b->cmd_q = msg_queue_init();
    subscribe(b->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv, b);
    jsdrv_list_initialize(&b->req_pending);
    jsdrv_list_initialize(&b->req_free);
    for (uint32_t i = 0; i < JSDRV_BUFSIG_COUNT_MAX; i++) {  // initialize 0 (invalid) just in case
        struct bufsig_s * s = &b->signals[i];
        s->idx = i;
        s->parent = b;
        s->active = false;
        s->topic[0] = 0;
    }

    if (jsdrv_thread_create(&b->thread, buffer_thread, b, -1)) {
        JSDRV_LOGE("buffer_id %u thread create failed", buffer_id);
        return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_ERROR_UNSPECIFIED, _buffer_add, NULL);
    }

    _send_buffer_list(self);
    return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, 0, _buffer_add, NULL);
}

static void _buffer_remove_inner(struct buffer_mgr_s * self, uint8_t buffer_id) {
    struct buffer_s * b = &self->buffers[buffer_id - 1];
    if (NULL == b->cmd_q) {
        JSDRV_LOGE("buffer_id %u does not exist", buffer_id);
        return;
    }
    JSDRV_LOGI("buffer_id %u remove", buffer_id);

    unsubscribe(b->context, b->topic, JSDRV_SFLAG_PUB, _buffer_recv, b);
    msg_queue_push(b->cmd_q, jsdrvp_msg_alloc_value(self->context, JSDRV_MSG_FINALIZE, &jsdrv_union_u8(0)));
    jsdrv_thread_join(&b->thread, 1000);
    msg_queue_finalize(b->cmd_q);
    b->cmd_q = NULL;
    _send_buffer_list(self);
}

static uint8_t _buffer_remove(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    struct buffer_mgr_s * self = &instance_;
    struct jsdrv_union_s v = msg->value;
    jsdrv_union_widen(&v);
    uint64_t buffer_id_u64 = v.value.u64;
    if (!is_buffer_idx_valid(buffer_id_u64)) {
        JSDRV_LOGE("invalid buffer_id: %llu", buffer_id_u64);
        return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, JSDRV_ERROR_NOT_FOUND, _buffer_remove, NULL);
    }
    _buffer_remove_inner(self, (uint8_t) buffer_id_u64);
    return send_return_code_to_frontend(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, 0, _buffer_remove, NULL);
}

int32_t jsdrv_buffer_initialize(struct jsdrv_context_s * context) {
    JSDRV_DBC_NOT_NULL(context);
    struct buffer_mgr_s * self = &instance_;
    if (NULL != self->context) {
        return JSDRV_ERROR_IN_USE;
    }
    memset(self, 0, sizeof(*self));
    self->context = context;

    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_ADD "$", &jsdrv_union_cjson_r(action_add_meta));
    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE "$", &jsdrv_union_cjson_r(action_remove_meta));
    send_to_frontend(self, JSDRV_BUFFER_MGR_MSG_ACTION_LIST "$", &jsdrv_union_cjson_r(action_list_meta));

    subscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_SFLAG_PUB, _buffer_add, NULL);
    subscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, JSDRV_SFLAG_PUB, _buffer_remove, NULL);
    _send_buffer_list(self);

    return 0;
}

void jsdrv_buffer_finalize(void) {
    struct buffer_mgr_s * self = &instance_;
    if (self->context) {
        unsubscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, JSDRV_SFLAG_PUB, _buffer_add, NULL);
        unsubscribe(self->context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, JSDRV_SFLAG_PUB, _buffer_remove, NULL);

        // finalize all buffers
        for (uint32_t buffer_idx = 1; buffer_idx < JSDRV_BUFFER_COUNT_MAX; ++buffer_idx) {
            if (NULL != self->buffers[buffer_idx - 1].cmd_q) {
                _buffer_remove_inner(self, buffer_idx);
            }
        }
        self->context = NULL;
    }
}
