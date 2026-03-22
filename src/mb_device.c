/*
* Copyright 2022-2025 Jetperch LLC
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

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/mb_drv.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "mb/stdmsg.h"
#include <inttypes.h>
#include <jsdrv/cstr.h>
#include <stdio.h>

#include "mb/comm/frame.h"
#include "mb/comm/link.h"
#include "mb/stdmsg.h"


#define FRAME_SIZE_U8           (512U)
#define FRAME_HEADER_SIZE_U8    (8U)
#define FRAME_FOOTER_SIZE_U8    (4U)
#define FRAME_OVERHEAD_U8       (FRAME_HEADER_SIZE_U8 + FRAME_FOOTER_SIZE_U8)
#define FRAME_OVERHEAD_U32      (FRAME_OVERHEAD_U8 >> 2)
#define PAYLOAD_SIZE_MAX_U8     (FRAME_SIZE_U8 - FRAME_OVERHEAD_U8)
#define PAYLOAD_SIZE_MAX_U32    (PAYLOAD_SIZE_MAX_U8 >> 2)
#define MB_TOPIC_SIZE_MAX       (32U)
#define PUBSUB_DISCONNECT_STR   "h|disconnect"

// todo move to an appropriate place
#define MB_USB_EP_BULK_IN  0x82
#define MB_USB_EP_BULK_OUT 0x01

enum events_e {
    EV_INVALID,
    EV_STATE_ENTER,
    EV_STATE_EXIT,

    EV_RESET,
    EV_ADVANCE,
    EV_TIMEOUT,

    EV_PUBSUB_FLUSH,

    EV_LINK_CONNECT_REQ,
    EV_LINK_CONNECT_ACK,
    EV_LINK_DISCONNECT_REQ,
    EV_LINK_DISCONNECT_ACK,
    EV_LINK_IDENTITY_RECEIVED,

    EV_BACKEND_OPEN_ACK,
    EV_BACKEND_OPEN_NACK,
    EV_BACKEND_OPEN_BULK_ACK,
    EV_BACKEND_OPEN_BULK_NACK,
    EV_BACKEND_CLOSE_ACK,

    EV_API_OPEN_REQUEST,
    EV_API_CLOSE_REQUEST,
};

enum state_e {
    ST_INVALID,         // initial state, requires reset
    ST_CLOSED,
    ST_LL_OPEN,
    ST_LL_BULK_OPEN,
    ST_LINK_REQUEST,
    ST_LINK_IDENTITY,
    ST_OPEN,

    // graceful disconnect
    ST_PUBSUB_FLUSH,
    ST_LINK_DISCONNECT,
    ST_LL_CLOSE_PEND,
    ST_LL_CLOSE,
};

struct jsdrvp_mb_dev_s {
    struct jsdrvp_ul_device_s ul; // MUST BE FIRST!
    struct jsdrvp_ll_device_s ll;
    struct jsdrv_context_s * context;
    struct jsdrvp_mb_drv_s * drv;  // optional device-specific upper driver
    uint16_t out_frame_id;
    uint16_t in_frame_id;
    uint16_t tracking_id_next;  // for confirmed delivery, wraps but skips 0
    uint64_t in_frame_count;
    int32_t open_mode;          // jsdrv_device_open_mode_e
    int64_t timeout_utc;      // <= 0 to disable timeout
    int64_t drv_timeout_utc;  // upper driver timeout, <= 0 to disable

    jsdrv_thread_t thread;
    volatile uint8_t state;  // state_e
    volatile bool finalize_pending;

    FILE * file_in;

    struct mb_link_identity_s identity;  // received device identity
    struct jsdrv_time_map_s time_map;
};

char mb_pubsub_prefix_get(void) {
    return 'h';
}

static void timeout_set(struct jsdrvp_mb_dev_s * self) {
    self->timeout_utc = jsdrv_time_utc() + JSDRV_TIME_SECOND;
}

static void timeout_clear(struct jsdrvp_mb_dev_s * self) {
    self->timeout_utc = 0;
}

typedef bool (*state_machine_fn)(struct jsdrvp_mb_dev_s * self, uint8_t event);
struct state_machine_transition_s {
    uint8_t event;
    uint8_t state_next;
    state_machine_fn guard;
};
struct state_machine_state_s {
    uint8_t state;  // for error checking
    const char * name;
    state_machine_fn on_enter;
    state_machine_fn on_exit;
    struct state_machine_transition_s const * const transitions;
};

static void send_to_frontend(struct jsdrvp_mb_dev_s * d, const char * subtopic, const struct jsdrv_union_s * value);
static void publish_to_device(struct jsdrvp_mb_dev_s * d, const char * topic, const struct jsdrv_union_s * value);
static void send_to_device(struct jsdrvp_mb_dev_s * d, enum mb_frame_service_type_e service_type, uint16_t metadata,
                           const uint32_t * data, uint32_t length_u32);

static void send_return_code_to_frontend(struct jsdrvp_mb_dev_s * d, const char * subtopic, int32_t rc) {
    struct jsdrv_topic_s full;
    jsdrv_topic_set(&full, d->ll.prefix);
    jsdrv_topic_append(&full, subtopic);
    jsdrv_topic_suffix_add(&full, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    JSDRV_LOGI("send_return_code %s = %d", full.topic, (int) rc);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, full.topic, &jsdrv_union_i32(rc));
    jsdrvp_backend_send(d->context, m);
}

static void send_frame_ctrl_to_device(struct jsdrvp_mb_dev_s * d, uint8_t ctrl) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_OUT_DATA, &jsdrv_union_i32(0));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->extra.bkusb_stream.endpoint = MB_USB_EP_BULK_OUT;
    m->value.size = 8;
    uint8_t * data_u8 = m->payload.bin;
    uint16_t * data_u16 =  (uint16_t *) data_u8;
    uint32_t * data_u32 =  (uint32_t *) data_u8;

    data_u8[0] = MB_FRAME_SOF1;
    data_u8[1] = MB_FRAME_SOF2;
    data_u8[2] = ctrl;
    data_u8[3] = MB_FRAME_FT_CONTROL << 3;
    data_u32[1] = mb_frame_link_check(data_u16[1]);
    msg_queue_push(d->ll.cmd_q, m);
}

static bool on_ll_open(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    self->out_frame_id = 0;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, JSDRV_MSG_OPEN, &jsdrv_union_i32(0));
    msg_queue_push(self->ll.cmd_q, m);
    return true;
}

static bool on_ll_bulk_open(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(self->context, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, &jsdrv_union_i32(0));
    m->extra.bkusb_stream.endpoint = MB_USB_EP_BULK_IN;
    msg_queue_push(self->ll.cmd_q, m);
    return true;
}

static bool on_link_request(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    send_frame_ctrl_to_device(self, MB_FRAME_CTRL_CONNECT_REQ);
    timeout_set(self);
    return true;
}

static bool on_link_identify(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) self;
    (void) event;

    struct mb_link_identity_s identity = {
        .mb_version = 0,
        .app_version = 0,
        .vendor_id = 0,
        .product_id = 0,
        .subtype = 0,
        .pubsub_prefix = 'h',
        .rsv1_u16 = 0,
    };

    send_to_device(self, MB_FRAME_ST_LINK, MB_LINK_MSG_IDENTITY,
        (uint32_t *) &identity, sizeof(identity) >> 2);
    timeout_set(self);
    return true;
}

static bool on_open_enter(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    timeout_clear(self);

    // Notify upper driver (publishes metadata)
    if (self->drv && self->drv->on_open) {
        self->drv->on_open(self->drv, self, &self->identity);
    }

    // State restore based on open mode.
    // Subscribe/unsubscribe on subtopics (not the device prefix itself)
    // to replay retained values without removing the original device subscriber.
    if (1 == self->open_mode) {  // JSDRV_DEVICE_OPEN_MODE_RESUME
        static const char * const subtrees[] = {"h", "c", "s", NULL};
        for (const char * const * st = subtrees; *st; ++st) {
            struct jsdrv_topic_s topic;
            jsdrv_topic_set(&topic, self->ll.prefix);
            jsdrv_topic_append(&topic, *st);
            jsdrvp_device_subscribe(self->context, self->ll.prefix, topic.topic,
                JSDRV_SFLAG_RETAIN | JSDRV_SFLAG_PUB);
            jsdrvp_device_unsubscribe(self->context, self->ll.prefix, topic.topic,
                JSDRV_SFLAG_RETAIN | JSDRV_SFLAG_PUB);
        }
    } else if (0 == self->open_mode) {  // JSDRV_DEVICE_OPEN_MODE_DEFAULTS
        if (self->drv && self->drv->publish_defaults) {
            self->drv->publish_defaults(self->drv, self);
        }
    }

    send_to_frontend(self, JSDRV_MSG_OPEN "#", &jsdrv_union_i32(0));
    return true;
}

static bool on_pubsub_flush(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    if (self->drv && self->drv->on_close) {
        self->drv->on_close(self->drv, self);
    }
    // ok to use "." prefix since directly sending to first PubSub instance.
    publish_to_device(self, "././!ping", &jsdrv_union_str(PUBSUB_DISCONNECT_STR));
    return true;
}

static bool on_link_disconnect(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    send_frame_ctrl_to_device(self, MB_FRAME_CTRL_DISCONNECT_REQ);
    return true;
}

static bool on_ll_close(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0));
    msg_queue_push(self->ll.cmd_q, m);
    return true;
}

static bool on_ll_close_exit(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    if (!self->finalize_pending) {
        send_to_frontend(self, JSDRV_MSG_CLOSE "#", &jsdrv_union_i32(0));
    }
    return true;
}

#define TRANSITION_END {0, 0, NULL}

const struct state_machine_transition_s state_machine_global[] = {
    {EV_RESET, ST_CLOSED, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_closed[] = {
    {EV_API_OPEN_REQUEST, ST_LL_OPEN, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_open[] = {
    {EV_BACKEND_OPEN_ACK, ST_LL_BULK_OPEN, NULL},
    {EV_BACKEND_OPEN_NACK, ST_LL_CLOSE, NULL},
    {EV_API_CLOSE_REQUEST, ST_LL_CLOSE, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_bulk_open[] = {
    {EV_BACKEND_OPEN_BULK_ACK, ST_LINK_REQUEST, NULL},
    {EV_BACKEND_OPEN_BULK_NACK, ST_LL_CLOSE, NULL},
    {EV_API_CLOSE_REQUEST, ST_LL_CLOSE, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_link_request[] = {
    {EV_LINK_CONNECT_ACK, ST_LINK_IDENTITY, NULL},
    {EV_TIMEOUT, ST_LINK_REQUEST, NULL},
    {EV_API_CLOSE_REQUEST, ST_LL_CLOSE, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_link_identity[] = {
    {EV_LINK_IDENTITY_RECEIVED, ST_OPEN, NULL},
    {EV_TIMEOUT, ST_LINK_REQUEST, NULL},
    {EV_API_CLOSE_REQUEST, ST_LL_CLOSE, NULL},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_open[] = {
    {EV_API_CLOSE_REQUEST, ST_PUBSUB_FLUSH, NULL},
    // todo handle detect link lost
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_pubsub_flush[] = {
    {EV_PUBSUB_FLUSH, ST_LINK_DISCONNECT, NULL},
    // todo timeout
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_link_disconnect[] = {
    {EV_LINK_DISCONNECT_ACK, ST_LL_CLOSE_PEND, NULL},
    // todo timeout
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_close_pend[] = {
    {EV_ADVANCE, ST_LL_CLOSE, NULL},
    // todo timeout
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_close[] = {
    {EV_BACKEND_CLOSE_ACK, ST_CLOSED, NULL},
    // todo timeout
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_end[] = {
    TRANSITION_END
};

const struct state_machine_state_s state_machine_states[] = {
    {ST_INVALID, "invalid", NULL, NULL, state_machine_global},
    {ST_CLOSED, "closed", NULL, NULL, state_machine_closed},
    {ST_LL_OPEN, "ll_open", on_ll_open, NULL, state_machine_ll_open},
    {ST_LL_BULK_OPEN, "ll_bulk_open", on_ll_bulk_open, NULL, state_machine_ll_bulk_open},
    {ST_LINK_REQUEST, "link_request", on_link_request, NULL, state_machine_link_request},
    {ST_LINK_IDENTITY, "link_identify", on_link_identify, NULL, state_machine_link_identity},
    {ST_OPEN, "open", on_open_enter, NULL, state_machine_open},
    {ST_PUBSUB_FLUSH, "pubsub_flush", on_pubsub_flush, NULL, state_machine_pubsub_flush},
    {ST_LINK_DISCONNECT, "link_disconnect", on_link_disconnect, NULL, state_machine_link_disconnect},
    {ST_LL_CLOSE_PEND, "ll_close_pend", NULL, NULL, state_machine_ll_close_pend},
    {ST_LL_CLOSE, "ll_close", on_ll_close, on_ll_close_exit, state_machine_ll_close},
    {0, NULL, NULL, NULL, NULL},
};

static inline void state_transition(struct jsdrvp_mb_dev_s * self, uint8_t next_state) {
    struct state_machine_state_s const * state;

    // exit
    state = &state_machine_states[self->state];
    if (NULL != state->on_exit) {
        state->on_exit(self, EV_STATE_EXIT);
    }

    // enter
    self->state = next_state;
    state = &state_machine_states[next_state];
    JSDRV_LOGI("state enter %d: %s", next_state, state->name);
    if (NULL != state->on_enter) {
        state->on_enter(self, EV_STATE_ENTER);
    }
}

static bool transitions_evaluate(struct jsdrvp_mb_dev_s * self, uint8_t state, uint8_t event) {
    struct state_machine_transition_s const * t = state_machine_states[state].transitions;
    while (t->event) {
        if (t->event == event) {
            if ((NULL == t->guard) || t->guard(self, event)) {
                state_transition(self, t->state_next);
                return true;
            }
        }
        ++t;
    }
    return false;
}

static void state_machine_process(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    if (!transitions_evaluate(self, 0, event)) {
        transitions_evaluate(self, self->state, event);
    }
}

static const char * prefix_match_and_strip(const char * prefix, const char * topic) {
    while (*prefix) {
        if (*prefix++ != *topic++) {
            return NULL;
        }
    }
    if (*topic++ != '/') {
        return NULL;
    }
    return topic;
}

/**
 * @brief Allocate a Minibitty frame message with specified payload size.
 *
 * @param d The target device.
 * @param service_type The mb_frame_service_type_e
 * @param length_u32 The payload length in u32 words.
 * @param metadata The 12-bit metadata value.
 * @return The allocated message or NULL on failure.
 */
static struct jsdrvp_msg_s * msg_alloc_send_to_device(struct jsdrvp_mb_dev_s * d, enum mb_frame_service_type_e service_type, uint8_t length_u32, uint16_t metadata) {
    if ((length_u32 == 0) || (length_u32 > PAYLOAD_SIZE_MAX_U32)) {
        JSDRV_LOGE("send_to_device: invalid length %ul", length_u32);
        return NULL;
    }

    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_OUT_DATA, &jsdrv_union_i32(0));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->extra.bkusb_stream.endpoint = MB_USB_EP_BULK_OUT;
    m->value.size = (length_u32 + FRAME_OVERHEAD_U32) << 2;
    uint8_t * data_u8 = m->payload.bin;
    uint16_t * data_u16 = (uint16_t *) data_u8;
    uint32_t * data_u32 =  (uint32_t *) data_u8;

    data_u8[0] = MB_FRAME_SOF1;
    data_u8[1] = MB_FRAME_SOF2;
    data_u16[1] = (MB_FRAME_FT_DATA << 11) | (d->out_frame_id & MB_FRAME_FRAME_ID_MAX);
    d->out_frame_id = (d->out_frame_id + 1) & MB_FRAME_FRAME_ID_MAX;
    data_u8[4] = length_u32 - 1;
    data_u8[5] = mb_frame_length_check(length_u32 - 1);
    data_u16[3] = (metadata & 0x0fff) | (((uint16_t) service_type) << 12);
    data_u32[length_u32 + 2] = 0;  // no frame_check on USB
    return m;
}

static void send_to_device(struct jsdrvp_mb_dev_s * d, enum mb_frame_service_type_e service_type, uint16_t metadata,
                           const uint32_t * data, uint32_t length_u32) {
    JSDRV_DBC_NOT_NULL(data);
    struct jsdrvp_msg_s * m = msg_alloc_send_to_device(d, service_type, length_u32, metadata);
    if (!m) {
        return;
    }
    uint8_t * data_u8 = &m->payload.bin[FRAME_HEADER_SIZE_U8];
    memcpy(data_u8, data, length_u32 << 2);
    msg_queue_push(d->ll.cmd_q, m);
}

static uint16_t tracking_id_alloc(struct jsdrvp_mb_dev_s * d) {
    uint16_t id = d->tracking_id_next;
    if (0 == id) {
        id = 1;
    }
    d->tracking_id_next = id + 1;
    if (0 == d->tracking_id_next) {
        d->tracking_id_next = 1;
    }
    return id;
}

static void publish_to_device_confirmed(struct jsdrvp_mb_dev_s * d, const char * topic,
                                        const struct jsdrv_union_s * value, bool confirmed) {
    uint32_t value_size = (value->size < 8) ? 8 : value->size;  // keep things simple
    uint32_t length_u8 = sizeof(struct mb_stdmsg_header_s) + MB_TOPIC_SIZE_MAX + value_size;  // topic and value
    uint32_t length_u32 = (length_u8 + 3) >> 2;  // round up
    struct jsdrvp_msg_s * m = msg_alloc_send_to_device(d, MB_FRAME_ST_STDMSG, length_u32, 0);
    if (!m) {
        return;
    }

    uint8_t * data_u8 = &m->payload.bin[FRAME_HEADER_SIZE_U8];

    uint16_t tracking_id = confirmed ? tracking_id_alloc(d) : 0;

    // Populate stdmsg header
    struct mb_stdmsg_header_s hdr;
    hdr.version = 0;
    hdr.type = MB_STDMSG_PUBLISH;
    hdr.origin_prefix = 'h';
    hdr.metadata = 0
        | (((uint32_t) tracking_id) << 16)
        | ((length_u8 & 0x0003U) << 6)  // size LSB
        | (value->type & 0x000fU);
    jsdrv_memcpy(data_u8, &hdr, sizeof(hdr));
    data_u8 += sizeof(hdr);

    // populate topic
    jsdrv_cstr_copy((char *) data_u8, topic, MB_TOPIC_SIZE_MAX);
    data_u8 += MB_TOPIC_SIZE_MAX;

    // populate value
    if ((value->type == JSDRV_UNION_JSON) || (value->type == JSDRV_UNION_STR)) {
        if (jsdrv_cstr_copy((char *) data_u8, value->value.str, (PAYLOAD_SIZE_MAX_U8 - MB_TOPIC_SIZE_MAX))) {
            JSDRV_LOGW("bulk_out_publish(%s) string truncated", topic);
        }
    } else if (value->type == JSDRV_UNION_BIN || value->type == JSDRV_UNION_STDMSG) {
        memcpy(data_u8, value->value.bin, value->size);
    } else {
        memcpy(data_u8, &value->value.u64, sizeof(uint64_t));
    }
    msg_queue_push(d->ll.cmd_q, m);
}

static void publish_to_device(struct jsdrvp_mb_dev_s * d, const char * topic, const struct jsdrv_union_s * value) {
    publish_to_device_confirmed(d, topic, value, false);
}

static bool handle_cmd(struct jsdrvp_mb_dev_s * d, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    JSDRV_LOGI("topic %s", msg->topic);

    const char * topic = prefix_match_and_strip(d->ll.prefix, msg->topic);
    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            d->finalize_pending = true;
            state_machine_process(d, EV_API_CLOSE_REQUEST);
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if (!topic) {
        JSDRV_LOGE("handle_cmd mismatch %s, %s", msg->topic, d->ll.prefix);
    } else if (topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, topic)) {
            if ((msg->value.type == JSDRV_UNION_U32) || (msg->value.type == JSDRV_UNION_I32)) {
                d->open_mode = msg->value.value.i32;
            } else {
                d->open_mode = 0;  // JSDRV_DEVICE_OPEN_MODE_DEFAULTS
            }
            state_machine_process(d, EV_API_OPEN_REQUEST);
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, topic)) {
            state_machine_process(d, EV_API_CLOSE_REQUEST);
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, topic)) {
            // just finalize this upper-level driver (keep lower-level running)
            d->finalize_pending = true;
            state_machine_process(d, EV_API_CLOSE_REQUEST);
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    //} else if (d->state != ST_OPEN) {
    //    // todo error code.
    } else if (d->drv && d->drv->handle_cmd && d->drv->handle_cmd(d->drv, d, topic, &msg->value)) {
        // upper driver handled it
    } else if (((topic[0] == 'h') || (topic[0] == '.')) && (topic[1] == '/')) {
        if (0 == strcmp("h/link/!ping", topic)) {
            send_to_device(d, MB_FRAME_ST_LINK, MB_LINK_MSG_PING,
                (uint32_t *) msg->value.value.bin, (msg->value.size + 3) >> 2);
            send_return_code_to_frontend(d, topic, 0);
        } else if (0 == strcmp("h/in/frecord", topic)) {
            if (NULL != d->file_in) {
                fclose(d->file_in);
            }
            if (msg->value.size) {
                d->file_in = fopen(msg->value.value.str, "wb");
            }
            send_return_code_to_frontend(d, topic, 0);
        } else {
            JSDRV_LOGW("handle_cmd unsupported h/ topic: %s", topic);
            send_return_code_to_frontend(d, topic, JSDRV_ERROR_NOT_SUPPORTED);
        }
    } else {
        publish_to_device_confirmed(d, topic, &msg->value, msg->source != 0);
    }
    jsdrvp_msg_free(d->context, msg);
    return rv;
}

static void send_to_frontend(struct jsdrvp_mb_dev_s * d, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, d->ll.prefix);
    jsdrv_topic_append(&topic, subtopic);

    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, topic.topic, value);
    jsdrvp_backend_send(d->context, m);
}

static void handle_in_link(struct jsdrvp_mb_dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    JSDRV_LOGD3("handle link frame: length=%u", length);
    uint8_t msg_type = (uint8_t) (metadata & 0xff);
    switch (msg_type) {
        case MB_LINK_MSG_INVALID:
            JSDRV_LOGW("link msg: invalid");
            break;
        case MB_LINK_MSG_PING:
            // todo respond with pong
            break;
        case MB_LINK_MSG_PONG:
            send_to_frontend(d, "h/link/!pong", &jsdrv_union_bin((uint8_t *) data, length * 4));
            break;
        case MB_LINK_MSG_IDENTITY:
            if (length >= (sizeof(struct mb_link_identity_s) >> 2)) {
                d->identity = *((const struct mb_link_identity_s *) data);
            }
            state_machine_process(d, EV_LINK_IDENTITY_RECEIVED);
            break;
        default:
            JSDRV_LOGW("link msg: unknown %d", msg_type);
            break;
    }
}

static void handle_in_trace(struct jsdrvp_mb_dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    (void) metadata;
    send_to_frontend(d, "h/!trace", &jsdrv_union_bin((uint8_t *) data, length * 4));
}

static void handle_in_publish(struct jsdrvp_mb_dev_s * d, uint32_t metadata, struct mb_stdmsg_publish_s * publish, uint8_t length) {
    uint8_t value_type = (uint8_t) (metadata & 0x0000000fU);
    uint8_t size_lsb = (uint8_t) ((metadata & 0x000000C0U) >> 6);
    uint32_t length_u32 = ((uint32_t) length) << 2;
    if (size_lsb) {
        length_u32 = length_u32 - 4 + size_lsb;
    }
    uint32_t value_size = length_u32 - MB_TOPIC_SIZE_MAX;

    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);

    // process topic
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, d->ll.prefix);
    jsdrv_topic_append(&topic, publish->topic);
    jsdrv_cstr_copy(m->topic, topic.topic, sizeof(m->topic));

    // process value
    m->value.size = value_size;
    m->value.type = value_type;
    if (    0
            || (value_type == JSDRV_UNION_STR)
            || (value_type == JSDRV_UNION_JSON)
            || (value_type == JSDRV_UNION_BIN)
            || (value_type == JSDRV_UNION_STDMSG)
            || (value_type == JSDRV_UNION_FRAME)) {
        JSDRV_ASSERT(value_size <= JSDRV_PAYLOAD_LENGTH_MAX);  // always true
        m->value.value.bin = m->payload.bin;
        memcpy(m->payload.bin, publish->value.bin, m->value.size);
    } else {
        m->value.value.u64 = publish->value.u64;
    }

    if (jsdrv_cstr_ends_with(topic.topic, "/./!pong")
            && (value_type == JSDRV_UNION_STR)
            && jsdrv_cstr_casecmp(PUBSUB_DISCONNECT_STR, m->payload.str)) {
        state_machine_process(d, EV_PUBSUB_FLUSH);
        jsdrvp_msg_free(d->context, m);
    } else if (jsdrv_cstr_ends_with(publish->topic, "/!rsp")
               && (value_type == JSDRV_UNION_STDMSG)
               && value_size >= (sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_pubsub_response_s))) {
        // Confirmed delivery response: device publishes to {origin_prefix}/!rsp
        // with a STDMSG value containing mb_stdmsg_pubsub_response_s.
        struct mb_stdmsg_header_s * rsp_hdr = (struct mb_stdmsg_header_s *) &publish->value;
        struct mb_stdmsg_pubsub_response_s * rsp =
            (struct mb_stdmsg_pubsub_response_s *) (rsp_hdr + 1);
        JSDRV_LOGI("confirmed delivery rsp topic=%s rc=%d", rsp->topic, (int) rsp->return_code);
        send_return_code_to_frontend(d, rsp->topic, (int32_t) rsp->return_code);
        jsdrvp_msg_free(d->context, m);
    } else if (d->drv && d->drv->handle_publish
               && d->drv->handle_publish(d->drv, d, publish->topic, &m->value)) {
        jsdrvp_msg_free(d->context, m);
    } else {
        jsdrvp_backend_send(d->context, m);
    }
}

static void handle_in_stdmsg(struct jsdrvp_mb_dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    (void) metadata;  // do not use frame metadata
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) data;
    data += sizeof(struct mb_stdmsg_header_s) / sizeof(uint32_t);

    switch (hdr->type) {
        case MB_STDMSG_PUBLISH:
            handle_in_publish(d, hdr->metadata, (struct mb_stdmsg_publish_s *) data, length - 2);
            break;
        case MB_STDMSG_PUBSUB_RESPONSE: {
            struct mb_stdmsg_pubsub_response_s * rsp =
                (struct mb_stdmsg_pubsub_response_s *) data;
            send_return_code_to_frontend(d, rsp->topic,
                (int32_t) rsp->return_code);
            break;
        }
        case MB_STDMSG_TIMESYNC_SYNC:
            // todo
            break;
        case MB_STDMSG_TIMESYNC_MAP:
            // todo
            break;
        case MB_STDMSG_COMM_STATS:
            // todo
            break;
        case MB_STDMSG_THROUGHPUT:
            // todo recreate throughput test handle_in_throughput(d, metadata, p_u32 + 2, length + 1);
            break;
        default:
            break;
    }
}

static void handle_stream_in_link_frame(struct jsdrvp_mb_dev_s * d, uint32_t * p_u32) {
    uint8_t * p_u8 = (uint8_t *) p_u32;
    uint16_t * p_u16 = (uint16_t *) p_u32;
    uint32_t link_check = mb_frame_link_check(p_u16[1]);
    if (link_check != p_u32[1]) {
        JSDRV_LOGW("link frame check mismatch");
        return;
    }

    uint8_t frame_type = p_u8[3] >> 3;
    if (frame_type != MB_FRAME_FT_CONTROL) {
        JSDRV_LOGW("unsupported link frame: 0x%02x", frame_type);
        return;
    }

    uint8_t ctrl = p_u8[2];
    uint8_t event = 0;
    switch (ctrl) {
        case MB_FRAME_CTRL_CONNECT_REQ: event = EV_LINK_CONNECT_REQ; break;
        case MB_FRAME_CTRL_CONNECT_ACK: event = EV_LINK_CONNECT_ACK; break;
        case MB_FRAME_CTRL_DISCONNECT_REQ: event = EV_LINK_DISCONNECT_REQ; break;
        case MB_FRAME_CTRL_DISCONNECT_ACK: event = EV_LINK_DISCONNECT_ACK; break;
        default:
            JSDRV_LOGW("unsupported link control: %d", ctrl);
            return;
    }
    JSDRV_LOGI("link frame: ctrl=%d -> event=%d", ctrl, event);
    state_machine_process(d, event);
}

static void handle_stream_in_frame(struct jsdrvp_mb_dev_s * d, uint32_t * p_u32) {
    uint8_t * p_u8 = (uint8_t *) p_u32;
    uint16_t * p_u16 = (uint16_t *) p_u32;
    if (p_u8[0] != MB_FRAME_SOF1) {
        JSDRV_LOGW("frame SOF1 mismatch: 0x%02x", p_u8[0]);
        return;
    }
    if (p_u8[1] != MB_FRAME_SOF2) {
        JSDRV_LOGW("frame SOF2 mismatch: 0x%02x", p_u8[1]);
        return;
    }
    uint16_t frame_id = p_u16[1] & MB_FRAME_FRAME_ID_MAX;
    uint8_t frame_type = p_u8[3] >> 3;

    switch (frame_type) {
        case MB_FRAME_FT_DATA:
            break;
        case MB_FRAME_FT_ACK_ALL:           /* intentional fall-through */
        case MB_FRAME_FT_ACK_ONE:           /* intentional fall-through */
        case MB_FRAME_FT_NACK:              /* intentional fall-through */
        case MB_FRAME_FT_RESERVED:          /* intentional fall-through */
        case MB_FRAME_FT_CONTROL:           /* intentional fall-through */
            handle_stream_in_link_frame(d, p_u32);
            return;
        default:
            JSDRV_LOGW("unexpected frame type: 0x%02x", frame_type);
            break;
    };

    if (d->in_frame_id != frame_id) {
        JSDRV_LOGW("in frame_id mismatch %d != %d", (int) d->in_frame_id, (int) frame_id);
        // todo keep statistics
    }
    d->in_frame_id = (frame_id + 1) & MB_FRAME_FRAME_ID_MAX;
    ++d->in_frame_count;

    uint8_t length = p_u8[4];
    uint8_t length_check_expect = mb_frame_length_check(length);
    uint8_t length_check_actual = p_u8[5];
    if (length_check_expect != length_check_actual) {
        JSDRV_LOGW("frame length check mismatch: 0x%02x != 0x%02x", length_check_expect, length_check_actual);
    }
    uint16_t metadata = p_u16[3] & 0x0fffU;
    uint8_t service_type = p_u8[7] >> 4;

    switch (service_type) {
        case MB_FRAME_ST_INVALID:
            JSDRV_LOGW("invalid service type");
            break;
        case MB_FRAME_ST_LINK:
            handle_in_link(d, metadata, p_u32 + 2, length + 1);
            break;
        case MB_FRAME_ST_TRACE:
            handle_in_trace(d, metadata, p_u32 + 2, length + 1);
            break;
        case MB_FRAME_ST_STDMSG:
            handle_in_stdmsg(d, metadata, p_u32 + 2, length + 1);
            break;
        case MB_FRAME_ST_APP:
            if (d->drv && d->drv->handle_app) {
                d->drv->handle_app(d->drv, d, metadata, p_u32 + 2, length + 1);
            }
            break;
        default:
            JSDRV_LOGW("unsupported service type %d", (int) service_type);
            break;
    }
}

static void handle_stream_in(struct jsdrvp_mb_dev_s * d, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    uint32_t frame_count = (msg->value.size + FRAME_SIZE_U8 - 1) / FRAME_SIZE_U8;
    for (uint32_t i = 0; i < frame_count; ++i) {
        uint32_t * p_u32 = (uint32_t *) &msg->value.value.bin[i * FRAME_SIZE_U8];
        handle_stream_in_frame(d, p_u32);
    }
    if (NULL != d->file_in) {
        fwrite(msg->value.value.bin, 1, msg->value.size, d->file_in);
    }
}

static bool handle_rsp(struct jsdrvp_mb_dev_s * d, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    uint8_t event = 0;
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_USBBK_MSG_STREAM_IN_DATA, msg->topic)) {
        JSDRV_LOGD3("stream_in_data sz=%d", (int) msg->value.size);
        handle_stream_in(d, msg);
        msg_queue_push(d->ll.cmd_q, msg);  // return
        return true;
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic)) {
        JSDRV_LOGD2("stream_out_data done");
        // no action necessary
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, msg->topic)) {
        event = (0 == msg->value.value.u32) ? EV_BACKEND_OPEN_BULK_ACK : EV_BACKEND_OPEN_BULK_NACK;
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_CLOSE, msg->topic)) {
        // ignore, close will clean up
    } else if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, msg->topic)) {
            event = (0 == msg->value.value.u32) ? EV_BACKEND_OPEN_ACK : EV_BACKEND_OPEN_NACK;
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, msg->topic)) {
            event = EV_BACKEND_CLOSE_ACK;
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            d->finalize_pending = true;
            event = EV_API_CLOSE_REQUEST;
        } else {
            JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
        }
    } else {
        JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
    }
    jsdrvp_msg_free(d->context, msg);
    if (event) {
        state_machine_process(d, event);
    }
    return rv;
}

static uint32_t thread_timeout_duration_ms(struct jsdrvp_mb_dev_s * d) {
    int64_t now = jsdrv_time_utc();
    int64_t earliest = 0;
    if (d->timeout_utc > 0) {
        earliest = d->timeout_utc;
    }
    if ((d->drv_timeout_utc > 0) && ((earliest <= 0) || (d->drv_timeout_utc < earliest))) {
        earliest = d->drv_timeout_utc;
    }
    if (earliest > 0) {
        int64_t duration = earliest - now;
        if (duration < 0) {
            return 0;
        }
        if (duration < (5 * JSDRV_TIME_SECOND)) {
            return (uint32_t) JSDRV_TIME_TO_MILLISECONDS(duration);
        }
    }
    return 5000;
}


static THREAD_RETURN_TYPE driver_thread(THREAD_ARG_TYPE lpParam) {
    struct jsdrvp_mb_dev_s *d = (struct jsdrvp_mb_dev_s *) lpParam;
    JSDRV_LOGI("MB USB upper-level thread started for %s", d->ll.prefix);
    state_machine_process(d, EV_RESET);

#if _WIN32
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count;
    handles[0] = msg_queue_handle_get(d->ul.cmd_q);
    handles[1] = msg_queue_handle_get(d->ll.rsp_q);
    handle_count = 2;
#else
    struct pollfd fds[2];
    fds[0].fd = msg_queue_handle_get(d->ul.cmd_q);
    fds[0].events = POLLIN;
    fds[1].fd = msg_queue_handle_get(d->ll.rsp_q);
    fds[1].events = POLLIN;
#endif

    while (true) {
        if ((d->finalize_pending) && (d->state == ST_CLOSED)) {
            break;
        }
#if _WIN32
        WaitForMultipleObjects(handle_count, handles, false, thread_timeout_duration_ms(d));
#else
        (void) thread_timeout_duration_ms; // todo support timeout_duration_ms
        poll(fds, 2, 2);
#endif
        JSDRV_LOGD2("ul thread tick");
        while (handle_cmd(d, msg_queue_pop_immediate(d->ul.cmd_q))) {
            ;
        }
        // note: ResetEvent handled automatically by msg_queue_pop_immediate
        while (handle_rsp(d, msg_queue_pop_immediate(d->ll.rsp_q))) {
            ;
        }

        if ((d->drv_timeout_utc > 0) && (jsdrv_time_utc() >= d->drv_timeout_utc)) {
            d->drv_timeout_utc = 0;
            if (d->drv && d->drv->on_timeout) {
                d->drv->on_timeout(d->drv, d);
            }
        }

        if (d->state == ST_LL_CLOSE_PEND) {
            state_machine_process(d, EV_ADVANCE);
        }
    }

    if (NULL != d->file_in) {
        fclose(d->file_in);
    }

    JSDRV_LOGI("MB USB upper-level thread done %s", d->ll.prefix);
    THREAD_RETURN();
}

static void join(struct jsdrvp_ul_device_s * device) {
    struct jsdrvp_mb_dev_s * d = (struct jsdrvp_mb_dev_s *) device;
    jsdrvp_send_finalize_msg(d->context, d->ul.cmd_q, JSDRV_MSG_FINALIZE);
    // and wait for thread to exit.
    jsdrv_thread_join(&d->thread, 1000);
    if (d->drv && d->drv->finalize) {
        d->drv->finalize(d->drv);
    }
    jsdrv_free(d);
}

int32_t jsdrvp_ul_mb_device_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context,
                                        struct jsdrvp_ll_device_s * ll, struct jsdrvp_mb_drv_s * drv) {
    JSDRV_DBC_NOT_NULL(device);
    JSDRV_DBC_NOT_NULL(context);
    JSDRV_DBC_NOT_NULL(ll);

    for (uint32_t state = 0; ; ++state) {
        const struct state_machine_state_s * s = &state_machine_states[state];
        if ((s->state == 0) && !s->name) {
            break;
        }
        if (s->state != state) {
            JSDRV_LOGE("state machine state mismatch %d != %d", (int) s->state, (int) state);
            return JSDRV_ERROR_UNSPECIFIED;
        }
    }

    *device = NULL;
    struct jsdrvp_mb_dev_s * d = jsdrv_alloc_clr(sizeof(struct jsdrvp_mb_dev_s));
    JSDRV_LOGI("jsdrvp_ul_mb_device_factory %p", d);
    d->context = context;
    d->ll = *ll;
    d->drv = drv;
    d->ul.cmd_q = msg_queue_init();
    d->ul.join = join;
    if (jsdrv_thread_create(&d->thread, driver_thread, d, 1)) {
        return JSDRV_ERROR_UNSPECIFIED;
    }
    *device = &d->ul;
    return 0;
}

int32_t jsdrvp_mb_device_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context,
                                 struct jsdrvp_ll_device_s * ll) {
    return jsdrvp_ul_mb_device_usb_factory(device, context, ll, NULL);
}

// --- Service functions for upper drivers ---

void jsdrvp_mb_dev_send_to_frontend(struct jsdrvp_mb_dev_s * dev, const char * subtopic, const struct jsdrv_union_s * value) {
    send_to_frontend(dev, subtopic, value);
}

void jsdrvp_mb_dev_publish_to_device(struct jsdrvp_mb_dev_s * dev, const char * topic, const struct jsdrv_union_s * value) {
    publish_to_device(dev, topic, value);
}

void jsdrvp_mb_dev_send_to_device(struct jsdrvp_mb_dev_s * dev, enum mb_frame_service_type_e service_type, uint16_t metadata,
                                   const uint32_t * data, uint32_t length_u32) {
    send_to_device(dev, service_type, metadata, data, length_u32);
}

struct jsdrv_context_s * jsdrvp_mb_dev_context(struct jsdrvp_mb_dev_s * dev) {
    return dev->context;
}

const char * jsdrvp_mb_dev_prefix(struct jsdrvp_mb_dev_s * dev) {
    return dev->ll.prefix;
}

void jsdrvp_mb_dev_backend_send(struct jsdrvp_mb_dev_s * dev, struct jsdrvp_msg_s * msg) {
    jsdrvp_backend_send(dev->context, msg);
}

void jsdrvp_mb_dev_set_timeout(struct jsdrvp_mb_dev_s * dev, int64_t timeout_utc) {
    dev->drv_timeout_utc = timeout_utc;
}

void jsdrvp_mb_dev_send_return_code(struct jsdrvp_mb_dev_s * dev, const char * subtopic, int32_t rc) {
    send_return_code_to_frontend(dev, subtopic, rc);
}

const char * jsdrvp_mb_dev_state_str(struct jsdrvp_mb_dev_s * dev) {
    return state_machine_states[dev->state].name;
}

int32_t jsdrvp_mb_dev_open_mode(struct jsdrvp_mb_dev_s * dev) {
    return dev->open_mode;
}
