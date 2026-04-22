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
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "mb/stdmsg.h"
#include <inttypes.h>
#include <jsdrv/cstr.h>
#include <stdio.h>

#include "mb/comm/frame.h"
#include "mb/comm/link.h"
#include "mb/stdmsg.h"
#include "mb/timesync.h"
#include "jsdrv_prv/meta_binary.h"


#define FRAME_SIZE_U8           (512U)
#define FRAME_HEADER_SIZE_U8    (8U)
#define FRAME_FOOTER_SIZE_U8    (4U)
#define FRAME_OVERHEAD_U8       (FRAME_HEADER_SIZE_U8 + FRAME_FOOTER_SIZE_U8)
#define FRAME_OVERHEAD_U32      (FRAME_OVERHEAD_U8 >> 2)
#define PAYLOAD_SIZE_MAX_U8     (FRAME_SIZE_U8 - FRAME_OVERHEAD_U8)
#define PAYLOAD_SIZE_MAX_U32    (PAYLOAD_SIZE_MAX_U8 >> 2)
#define MB_TOPIC_SIZE_MAX       (32U)
#define PUBSUB_DISCONNECT_STR   "h|disconnect"

// One time_map slot per possible pubsub-instance prefix (top-level
// topic first character).  Devices with any combination of ctrl /
// sensor / future task prefixes share this table; unused slots are
// harmless.
#define MB_DEV_TIME_MAP_COUNT   (256U)

// Hard-coded pubsub prefixes for state fetch (until distributed discovery)
// Default iteration prefixes when the upper driver does not provide
// its own via drv->state_fetch_prefixes().  This preserves legacy
// behavior for drivers that haven't opted into the callback yet and
// for devices whose firmware exposes both a `c` (ctrl) and `s`
// (sensor) task.  Drop to "" once null-target GET_INIT lands firmware
// side (see doc/plans/open_state_management.md).
static const char STATE_FETCH_PREFIXES_DEFAULT[] = "cs";

#define STATE_FETCH_PHASE_IDLE      0
#define STATE_FETCH_PHASE_GET_INIT  1
#define STATE_FETCH_PHASE_GET_NEXT  2
#define STATE_FETCH_PHASE_META_HDR  3
#define STATE_FETCH_PHASE_META_DATA 4

#define STATE_FETCH_GET_INIT_RETRY_MAX      20
#define STATE_FETCH_GET_INIT_RETRY_TIMEOUT  (JSDRV_TIME_SECOND / 20)  // 50 ms
#define STATE_FETCH_META_BLOBS_MAX  8
#define STATE_FETCH_META_PAGE_SIZE  256

#ifndef MB_STDMSG_STATE
#define MB_STDMSG_STATE 0x08
#endif
#ifndef MB_STDMSG_PUBSUB_INFO
#define MB_STDMSG_PUBSUB_INFO 0x09
#endif

struct state_fetch_blob_s {
    uint8_t  blob_type;
    uint8_t  target;
    uint16_t page_size;
    uint32_t offset;
    char     topic[32];
};

struct state_fetch_s {
    uint8_t  prefix_idx;
    uint8_t  phase;
    uint8_t  retry_count;
    uint32_t transaction_id;
    // Null-terminated list of top-level prefixes to iterate.  Set by
    // state_fetch_start from drv->state_fetch_prefixes(), else from
    // d->identity.pubsub_prefix (single char built into prefix_buf),
    // else STATE_FETCH_PREFIXES_DEFAULT.  Outlives the fetch.
    const char * prefixes;
    char     prefix_buf[2];  // backing storage for identity-derived prefix
    // metadata blob info captured from ././info entries
    struct state_fetch_blob_s blobs[STATE_FETCH_META_BLOBS_MAX];
    uint8_t  blob_count;
    uint8_t  blob_idx;
    // metadata blob read buffer
    uint8_t * meta_buf;
    uint32_t meta_buf_size;
    uint32_t meta_buf_offset;
    uint32_t meta_total_size;
};

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
    // Set when the LL side has emitted JSDRV_MSG_TYPE_LL_TERMINATED into
    // ll.rsp_q, meaning the backend has stopped producing for this device
    // and the UL thread is free to exit and publish DEVICE_REMOVE.
    volatile bool ll_terminated;

    FILE * file_in;

    struct mb_link_identity_s identity;  // received device identity
    // One time_map per top-level pubsub instance prefix, indexed by
    // the first character of the instance topic (e.g. time_maps['s']
    // for the sensor task's timesync, time_maps['c'] for ctrl-side).
    // counter_rate > 0.0 means the slot has been populated by an
    // incoming !map; a zero slot returns NULL from the accessor.  All
    // slots are cleared in on_ll_open() at session start.
    struct jsdrv_time_map_s time_maps[MB_DEV_TIME_MAP_COUNT];
    struct state_fetch_s state_fetch;
};

char mb_pubsub_prefix_get(void) {
    return 'h';
}

static void timeout_set(struct jsdrvp_mb_dev_s * self) {
    self->timeout_utc = jsdrv_time_utc() + (JSDRV_TIME_SECOND / 4);
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
    // Drop any stale timesync maps from a previous open so accessors
    // return NULL until the current session's !map publishes arrive.
    memset(self->time_maps, 0, sizeof(self->time_maps));
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

// --- State fetch: !state GET to restore device retained values ---

static void state_fetch_start(struct jsdrvp_mb_dev_s * self);
static void state_fetch_send_get_init(struct jsdrvp_mb_dev_s * self);
static void state_fetch_send_get_next(struct jsdrvp_mb_dev_s * self);
static void state_fetch_advance_prefix(struct jsdrvp_mb_dev_s * self);
static void state_fetch_start_meta(struct jsdrvp_mb_dev_s * self);
static void state_fetch_send_meta_read(struct jsdrvp_mb_dev_s * self);
static void state_fetch_complete(struct jsdrvp_mb_dev_s * self);

static void state_fetch_publish_state(struct jsdrvp_mb_dev_s * self,
                                       const char * topic, uint8_t value_type,
                                       const void * value, uint32_t size) {
    // Build message with fully-qualified device topic, same as handle_in_publish
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(self->context);
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->ll.prefix);
    jsdrv_topic_append(&t, topic);
    jsdrv_cstr_copy(m->topic, t.topic, sizeof(m->topic));
    m->value.type = value_type;
    m->value.size = size;
    m->value.flags = JSDRV_UNION_FLAG_RETAIN;
    m->value.app = 0;
    if (    0
            || (value_type == JSDRV_UNION_STR)
            || (value_type == JSDRV_UNION_JSON)
            || (value_type == JSDRV_UNION_BIN)
            || (value_type == JSDRV_UNION_STDMSG)
            || (value_type == JSDRV_UNION_FRAME)) {
        m->value.value.bin = m->payload.bin;
        memcpy(m->payload.bin, value, size);
    } else if (size <= 8) {
        m->value.value.u64 = 0;
        memcpy(&m->value.value.u64, value, size);
    }
    jsdrvp_backend_send(self->context, m);
}

static void state_fetch_publish_stdmsg_to_device(struct jsdrvp_mb_dev_s * self,
                                                   const char * topic,
                                                   uint8_t stdmsg_type,
                                                   const void * payload,
                                                   uint16_t payload_size) {
    uint8_t buf[128];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = stdmsg_type;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    if (payload && payload_size > 0) {
        memcpy(buf + sizeof(*hdr), payload, payload_size);
    }
    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = sizeof(*hdr) + payload_size;
    v.value.bin = buf;
    v.flags = 0;
    v.app = 0;
    publish_to_device(self, topic, &v);
}

static void state_fetch_start(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;
    memset(sf, 0, sizeof(*sf));
    sf->prefix_idx = 0;
    sf->transaction_id = 0x5F00;
    sf->prefixes = NULL;
    if (self->drv && self->drv->state_fetch_prefixes) {
        sf->prefixes = self->drv->state_fetch_prefixes(self->drv);
    }
    if (!sf->prefixes || !sf->prefixes[0]) {
        if (self->identity.pubsub_prefix) {
            sf->prefix_buf[0] = self->identity.pubsub_prefix;
            sf->prefix_buf[1] = '\0';
            sf->prefixes = sf->prefix_buf;
        } else {
            sf->prefixes = STATE_FETCH_PREFIXES_DEFAULT;
        }
    }
    state_fetch_send_get_init(self);
}

void jsdrvp_mb_dev_state_fetch_start(struct jsdrvp_mb_dev_s * dev) {
    if (!dev) {
        return;
    }
    if (dev->state_fetch.phase != STATE_FETCH_PHASE_IDLE) {
        // fetch already running or completed; ignore duplicate kicks
        return;
    }
    state_fetch_start(dev);
}

static void state_fetch_send_get_init(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;
    char prefix = sf->prefixes[sf->prefix_idx];
    if (!prefix) {
        state_fetch_start_meta(self);
        return;
    }
    sf->phase = STATE_FETCH_PHASE_GET_INIT;
    sf->transaction_id++;

    // Build state header: transaction_id(u32), type(u8)=GET_INIT, flags, status, rsv
    uint8_t payload[8];
    memset(payload, 0, sizeof(payload));
    uint32_t txn = sf->transaction_id;
    memcpy(payload, &txn, 4);
    payload[4] = 1;  // MB_STDMSG_STATE_TYPE_GET_INIT

    char topic[16];
    topic[0] = prefix; topic[1] = '/'; topic[2] = '.'; topic[3] = '/';
    topic[4] = '!'; topic[5] = 's'; topic[6] = 't'; topic[7] = 'a';
    topic[8] = 't'; topic[9] = 'e'; topic[10] = '\0';

    JSDRV_LOGI("state_fetch: GET_INIT for '%c'", prefix);
    state_fetch_publish_stdmsg_to_device(self, topic, MB_STDMSG_STATE, payload, 8);
    timeout_set(self);
}

static void state_fetch_send_get_next(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;
    char prefix = sf->prefixes[sf->prefix_idx];
    sf->phase = STATE_FETCH_PHASE_GET_NEXT;

    uint8_t payload[8];
    memset(payload, 0, sizeof(payload));
    uint32_t txn = sf->transaction_id;
    memcpy(payload, &txn, 4);
    payload[4] = 3;  // MB_STDMSG_STATE_TYPE_GET_NEXT

    char topic[16];
    topic[0] = prefix; topic[1] = '/'; topic[2] = '.'; topic[3] = '/';
    topic[4] = '!'; topic[5] = 's'; topic[6] = 't'; topic[7] = 'a';
    topic[8] = 't'; topic[9] = 'e'; topic[10] = '\0';

    state_fetch_publish_stdmsg_to_device(self, topic, MB_STDMSG_STATE, payload, 8);
    timeout_set(self);
}

static void state_fetch_process_info_entry(struct jsdrvp_mb_dev_s * self,
                                            const uint8_t * value_data,
                                            uint16_t value_size) {
    // value_data is the STDMSG value: [inner_stdmsg_hdr(8)][mb_pubsub_info_s + blobs]
    struct state_fetch_s * sf = &self->state_fetch;
    if (value_size < 8 + 4) return;  // need at least stdmsg_hdr + info header

    const uint8_t * info_data = value_data + 8;  // skip inner stdmsg header
    uint8_t blob_count = info_data[1];  // info_s.blob_count

    const uint8_t * blob_data = info_data + 4;  // skip info header
    for (uint8_t i = 0; i < blob_count && sf->blob_count < STATE_FETCH_META_BLOBS_MAX; ++i) {
        if ((uint32_t)(blob_data - value_data) + 40 > value_size) break;
        struct state_fetch_blob_s * b = &sf->blobs[sf->blob_count];
        b->blob_type = blob_data[0];
        b->target = blob_data[1];
        memcpy(&b->page_size, blob_data + 2, 2);
        memcpy(&b->offset, blob_data + 4, 4);
        memcpy(b->topic, blob_data + 8, 32);
        sf->blob_count++;
        blob_data += 40;
    }
    JSDRV_LOGI("state_fetch: captured %u blobs from ././info", sf->blob_count);
}

static void state_fetch_on_rsp(struct jsdrvp_mb_dev_s * self,
                                const struct jsdrv_union_s * value) {
    struct state_fetch_s * sf = &self->state_fetch;
    if (value->type != JSDRV_UNION_STDMSG) return;
    if (value->size < 8 + 8) return;  // stdmsg_hdr + state_header

    const uint8_t * data = value->value.bin;
    // Skip stdmsg header to get state header
    const uint8_t * sh = data + 8;
    uint8_t state_type = sh[4];
    uint8_t state_flags = sh[5];
    uint8_t status = sh[6];

    timeout_clear(self);

    if (sf->phase == STATE_FETCH_PHASE_GET_INIT) {
        if (status == JSDRV_ERROR_BUSY) {
            if (sf->retry_count < STATE_FETCH_GET_INIT_RETRY_MAX) {
                sf->retry_count++;
                JSDRV_LOGI("state_fetch: GET_INIT BUSY for '%c', retry %u/%u",
                           sf->prefixes[sf->prefix_idx],
                           sf->retry_count, STATE_FETCH_GET_INIT_RETRY_MAX);
                self->timeout_utc = jsdrv_time_utc()
                        + STATE_FETCH_GET_INIT_RETRY_TIMEOUT;
                return;
            }
        }
        if (status != 0) {
            JSDRV_LOGW("state_fetch: GET_INIT failed status=%u", status);
            state_fetch_advance_prefix(self);
            return;
        }
        state_fetch_send_get_next(self);
        return;
    }

    if (sf->phase == STATE_FETCH_PHASE_GET_NEXT) {
        if (state_type != 2) {  // not GET_RSP
            JSDRV_LOGW("state_fetch: GET_NEXT unexpected state_type=%u, advancing", state_type);
            state_fetch_advance_prefix(self);
            return;
        }
        if (status != 0) {
            JSDRV_LOGW("state_fetch: GET_NEXT failed status=%u", status);
            state_fetch_advance_prefix(self);
            return;
        }

        // Parse state entries
        uint32_t offset = 8 + 8;  // stdmsg_hdr + state_header
        while (offset + 48 <= value->size) {  // 48 = min entry size (32 topic + 8 header + 8 value)
            const uint8_t * raw_topic = data + offset;
            uint8_t vtype = data[offset + 32];
            uint16_t vsize;
            memcpy(&vsize, data + offset + 34, 2);
            const uint8_t * vdata = data + offset + 40;
            uint16_t padded = (vsize + 7) & ~7;
            if (padded < 8) padded = 8;
            uint32_t entry_size = 40 + padded;

            if (offset + entry_size > value->size) break;
            if (raw_topic[0] == '\0') break;

            // Copy topic with guaranteed null termination
            char topic[MB_TOPIC_SIZE_MAX];
            memcpy(topic, raw_topic, MB_TOPIC_SIZE_MAX);
            topic[MB_TOPIC_SIZE_MAX - 1] = '\0';

            // Publish the retained value to the host pubsub
            state_fetch_publish_state(self, topic, vtype, vdata, vsize);

            // Check if this is the ././info topic
            if (0 == strcmp(topic + 1, "/./info") && vtype == JSDRV_UNION_STDMSG) {
                state_fetch_process_info_entry(self, vdata, vsize);
            }

            offset += entry_size;
        }

        if (state_flags & 0x02) {  // MB_STDMSG_STATE_FLAG_END
            state_fetch_advance_prefix(self);
        } else {
            state_fetch_send_get_next(self);
        }
        return;
    }
}

static void state_fetch_advance_prefix(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;
    sf->prefix_idx++;
    sf->retry_count = 0;
    state_fetch_send_get_init(self);
}

// --- Metadata fetch: read binary blobs via memory protocol ---

static void meta_fetch_on_topic(void * user_data, const char * topic, const char * json_meta) {
    struct jsdrvp_mb_dev_s * self = (struct jsdrvp_mb_dev_s *) user_data;
    // Replace leading "./" with the blob's instance prefix
    char resolved[64];
    struct state_fetch_s * sf = &self->state_fetch;
    if (topic[0] == '.' && topic[1] == '/' && sf->blob_idx < sf->blob_count) {
        resolved[0] = sf->blobs[sf->blob_idx].topic[0];
        jsdrv_cstr_copy(resolved + 1, topic + 1, sizeof(resolved) - 1);
    } else {
        jsdrv_cstr_copy(resolved, topic, sizeof(resolved));
    }
    // Publish as {device}/{resolved_topic}$ with JSON value
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(self->context);
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, self->ll.prefix);
    jsdrv_topic_append(&t, resolved);
    size_t tlen = strlen(t.topic);
    t.topic[tlen] = '$';
    t.topic[tlen + 1] = '\0';
    jsdrv_cstr_copy(m->topic, t.topic, sizeof(m->topic));
    m->value.type = JSDRV_UNION_JSON;
    m->value.size = (uint32_t)(strlen(json_meta) + 1);
    m->value.flags = JSDRV_UNION_FLAG_RETAIN;
    m->value.app = 0;
    if (m->value.size <= sizeof(m->payload.bin)) {
        m->value.value.str = m->payload.str;
        memcpy(m->payload.str, json_meta, m->value.size);
    } else {
        char * ptr = jsdrv_alloc(m->value.size);
        memcpy(ptr, json_meta, m->value.size);
        m->value.value.str = ptr;
        m->value.flags |= JSDRV_UNION_FLAG_HEAP_MEMORY;
    }
    jsdrvp_backend_send(self->context, m);
}

static void state_fetch_start_meta(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;

    // Find the next pubsub_meta blob from current blob_idx
    while (sf->blob_idx < sf->blob_count) {
        if (sf->blobs[sf->blob_idx].blob_type == 1) {  // pubsub_meta
            break;
        }
        sf->blob_idx++;
    }
    if (sf->blob_idx >= sf->blob_count) {
        state_fetch_complete(self);
        return;
    }

    // Read first page to get header
    sf->phase = STATE_FETCH_PHASE_META_HDR;
    sf->meta_buf_offset = 0;
    sf->meta_total_size = 0;
    if (sf->meta_buf) { jsdrv_free(sf->meta_buf); sf->meta_buf = NULL; }
    state_fetch_send_meta_read(self);
}

static void state_fetch_send_meta_read(struct jsdrvp_mb_dev_s * self) {
    struct state_fetch_s * sf = &self->state_fetch;
    struct state_fetch_blob_s * blob = &sf->blobs[sf->blob_idx];

    uint32_t read_offset = blob->offset + sf->meta_buf_offset;
    uint32_t page_size = blob->page_size ? blob->page_size : STATE_FETCH_META_PAGE_SIZE;

    // Build mb_stdmsg_mem_s READ command
    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));
    sf->transaction_id++;
    uint32_t txn = sf->transaction_id;
    memcpy(payload + 0, &txn, 4);     // transaction_id
    payload[4] = blob->target;         // target
    payload[5] = 1;                    // operation = READ
    uint16_t timeout_ms = 5000;
    memcpy(payload + 8, &timeout_ms, 2);  // timeout_ms
    memcpy(payload + 12, &read_offset, 4);  // offset
    memcpy(payload + 16, &page_size, 4);    // length

    state_fetch_publish_stdmsg_to_device(self, blob->topic, 0x07, payload, 24);
    timeout_set(self);
}

static void state_fetch_on_meta_rsp(struct jsdrvp_mb_dev_s * self,
                                     const struct jsdrv_union_s * value) {
    struct state_fetch_s * sf = &self->state_fetch;
    timeout_clear(self);

    if (value->type != JSDRV_UNION_STDMSG) return;
    uint32_t hdr_off = 8;  // skip stdmsg header
    if (value->size < hdr_off + 24) return;

    const uint8_t * mem_rsp = value->value.bin + hdr_off;
    uint8_t status = mem_rsp[7];
    uint32_t length;
    memcpy(&length, mem_rsp + 16, 4);

    if (status != 0) {
        JSDRV_LOGW("state_fetch: meta READ failed status=%u", status);
        // Skip this blob, try next
        goto next_blob;
    }

    const uint8_t * data = mem_rsp + 24;
    uint32_t data_size = value->size - hdr_off - 24;
    if (length > data_size) length = data_size;

    if (sf->phase == STATE_FETCH_PHASE_META_HDR) {
        // Parse header to get total_size
        if (length < 32 || memcmp(data, "MBtm_1.0", 8) != 0) {
            JSDRV_LOGW("state_fetch: meta blob bad magic");
            goto next_blob;
        }
        memcpy(&sf->meta_total_size, data + 12, 4);
        if (sf->meta_total_size == 0 || sf->meta_total_size > 64 * 1024) {
            JSDRV_LOGW("state_fetch: meta total_size=%u invalid", sf->meta_total_size);
            goto next_blob;
        }
        sf->meta_buf = jsdrv_alloc(sf->meta_total_size);
        if (!sf->meta_buf) {
            JSDRV_LOGW("state_fetch: meta alloc failed size=%u", sf->meta_total_size);
            goto next_blob;
        }
        sf->meta_buf_size = sf->meta_total_size;
        // Copy first page
        uint32_t copy = (length > sf->meta_total_size) ? sf->meta_total_size : length;
        memcpy(sf->meta_buf, data, copy);
        sf->meta_buf_offset = copy;
        sf->phase = STATE_FETCH_PHASE_META_DATA;

        if (sf->meta_buf_offset >= sf->meta_total_size) {
            goto parse_blob;
        }
        state_fetch_send_meta_read(self);
        return;
    }

    if (sf->phase == STATE_FETCH_PHASE_META_DATA) {
        uint32_t remaining = sf->meta_total_size - sf->meta_buf_offset;
        uint32_t copy = (length > remaining) ? remaining : length;
        memcpy(sf->meta_buf + sf->meta_buf_offset, data, copy);
        sf->meta_buf_offset += copy;

        if (sf->meta_buf_offset >= sf->meta_total_size) {
            goto parse_blob;
        }
        state_fetch_send_meta_read(self);
        return;
    }
    return;

parse_blob:
    JSDRV_LOGI("state_fetch: parsing %u byte metadata blob", sf->meta_total_size);
    meta_binary_parse(sf->meta_buf, sf->meta_total_size, meta_fetch_on_topic, self);
    // fall through to next_blob

next_blob:
    if (sf->meta_buf) { jsdrv_free(sf->meta_buf); sf->meta_buf = NULL; }
    sf->blob_idx++;
    while (sf->blob_idx < sf->blob_count) {
        if (sf->blobs[sf->blob_idx].blob_type == 1) break;
        sf->blob_idx++;
    }
    if (sf->blob_idx >= sf->blob_count) {
        state_fetch_complete(self);
    } else {
        JSDRV_LOGI("state_fetch: reading meta blob %u (type=%u topic=%s offset=0x%x)",
                    sf->blob_idx, sf->blobs[sf->blob_idx].blob_type,
                    sf->blobs[sf->blob_idx].topic, sf->blobs[sf->blob_idx].offset);
        sf->phase = STATE_FETCH_PHASE_META_HDR;
        sf->meta_buf_offset = 0;
        sf->meta_total_size = 0;
        state_fetch_send_meta_read(self);
    }
}

static void state_fetch_complete(struct jsdrvp_mb_dev_s * self) {
    self->state_fetch.phase = STATE_FETCH_PHASE_IDLE;
    JSDRV_LOGI("state_fetch: complete, signaling OPEN");
    send_to_frontend(self, JSDRV_MSG_OPEN "#", &jsdrv_union_i32(0));
}

// --- End state fetch ---

static bool on_open_enter(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    timeout_clear(self);

    // Notify upper driver
    if (self->drv && self->drv->on_open) {
        self->drv->on_open(self->drv, self, &self->identity);
    }

    // State restore: replay host-cached values
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

    // Fetch device state + metadata, then signal OPEN when complete.
    // A driver may defer by implementing open_ready (returning false)
    // and invoking jsdrvp_mb_dev_state_fetch_start() when ready.
    bool ready = true;
    if (self->drv && self->drv->open_ready) {
        ready = self->drv->open_ready(self->drv, self);
    }
    if (ready) {
        state_fetch_start(self);
    }
    return true;
}

// Close-path on_enter helpers arm a 250 ms timeout so EV_TIMEOUT can
// force progress if the device never responds with the expected ack.
// Without these timeouts, a misbehaving or disconnected device wedges
// the UL thread forever and downstream finalize/join times out with
// UAF risk.  The close path is idempotent device-side, so forcing a
// transition is safe: a late ack in the new state is simply dropped.
static bool on_pubsub_flush(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    if (self->drv && self->drv->on_close) {
        self->drv->on_close(self->drv, self);
    }
    // ok to use "." prefix since directly sending to first PubSub instance.
    publish_to_device(self, "././!ping", &jsdrv_union_str(PUBSUB_DISCONNECT_STR));
    timeout_set(self);
    return true;
}

static bool on_pubsub_flush_timeout(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) self; (void) event;
    JSDRV_LOGW("close timeout: no PUBSUB_FLUSH ack, advancing");
    return true;
}

static bool on_link_disconnect(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    send_frame_ctrl_to_device(self, MB_FRAME_CTRL_DISCONNECT_REQ);
    timeout_set(self);
    return true;
}

static bool on_link_disconnect_timeout(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) self; (void) event;
    JSDRV_LOGW("close timeout: no LINK_DISCONNECT ack, advancing");
    return true;
}

static bool on_ll_close(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(self->context, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0));
    msg_queue_push(self->ll.cmd_q, m);
    timeout_set(self);
    return true;
}

static bool on_ll_close_timeout(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) self; (void) event;
    JSDRV_LOGW("close timeout: no BACKEND_CLOSE ack, advancing");
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

static bool on_open_timeout(struct jsdrvp_mb_dev_s * self, uint8_t event) {
    (void) event;
    struct state_fetch_s * sf = &self->state_fetch;
    if (sf->phase == STATE_FETCH_PHASE_IDLE) {
        return false;
    }
    if (sf->phase == STATE_FETCH_PHASE_GET_INIT
            && sf->retry_count < STATE_FETCH_GET_INIT_RETRY_MAX) {
        char prefix = sf->prefixes[sf->prefix_idx];
        sf->retry_count++;
        JSDRV_LOGI("state_fetch: retry GET_INIT for '%c' (%u/%u)",
                   prefix, sf->retry_count,
                   STATE_FETCH_GET_INIT_RETRY_MAX);
        state_fetch_send_get_init(self);
        self->timeout_utc = jsdrv_time_utc()
                + STATE_FETCH_GET_INIT_RETRY_TIMEOUT;
    } else if (sf->phase == STATE_FETCH_PHASE_GET_INIT
            || sf->phase == STATE_FETCH_PHASE_GET_NEXT) {
        char prefix = sf->prefixes[sf->prefix_idx];
        JSDRV_LOGW("state_fetch: timeout waiting for !state response from '%c'", prefix);
        state_fetch_advance_prefix(self);
    } else {
        struct state_fetch_blob_s * blob = &sf->blobs[sf->blob_idx];
        JSDRV_LOGW("state_fetch: timeout reading metadata blob at %s offset=0x%x",
                    blob->topic, blob->offset);
        if (sf->meta_buf) {
            jsdrv_free(sf->meta_buf);
            sf->meta_buf = NULL;
        }
        sf->blob_idx++;
        state_fetch_start_meta(self);
    }
    return false;  // stay in ST_OPEN
}

const struct state_machine_transition_s state_machine_open[] = {
    {EV_TIMEOUT, ST_OPEN, on_open_timeout},
    {EV_API_CLOSE_REQUEST, ST_PUBSUB_FLUSH, NULL},
    // todo handle detect link lost
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_pubsub_flush[] = {
    {EV_PUBSUB_FLUSH, ST_LINK_DISCONNECT, NULL},
    {EV_TIMEOUT, ST_LINK_DISCONNECT, on_pubsub_flush_timeout},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_link_disconnect[] = {
    {EV_LINK_DISCONNECT_ACK, ST_LL_CLOSE_PEND, NULL},
    {EV_TIMEOUT, ST_LL_CLOSE_PEND, on_link_disconnect_timeout},
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_close_pend[] = {
    {EV_ADVANCE, ST_LL_CLOSE, NULL},
    // ST_LL_CLOSE_PEND is internally-advanced from the UL thread loop
    // (EV_ADVANCE dispatched every iteration) so no timeout is needed.
    TRANSITION_END
};

const struct state_machine_transition_s state_machine_ll_close[] = {
    {EV_BACKEND_CLOSE_ACK, ST_CLOSED, NULL},
    {EV_TIMEOUT, ST_CLOSED, on_ll_close_timeout},
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
        if (!transitions_evaluate(self, self->state, event)) {
            // API-level events (OPEN/CLOSE) should never be silently
            // dropped — a dropped OPEN leaves the caller's jsdrv_open
            // waiting for a completion signal that will never arrive.
            // Log so races are visible; the caller will still time out,
            // but at least the root cause is traceable in the log.
            if (event == EV_API_OPEN_REQUEST || event == EV_API_CLOSE_REQUEST) {
                JSDRV_LOGW("state_machine: dropped API event %u in state %u (%s)",
                           (unsigned) event, (unsigned) self->state,
                           state_machine_states[self->state].name);
            }
        }
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
    if ((msg->value.type >= JSDRV_UNION_U8) && (msg->value.type <= JSDRV_UNION_U64)) {
        JSDRV_LOGI("topic %s = %" PRIu64, msg->topic, msg->value.value.u64);
    } else if ((msg->value.type >= JSDRV_UNION_I8) && (msg->value.type <= JSDRV_UNION_I64)) {
        JSDRV_LOGI("topic %s = %" PRIi64, msg->topic, msg->value.value.i64);
    } else if ((msg->value.type == JSDRV_UNION_STR) || (msg->value.type == JSDRV_UNION_JSON)) {
        JSDRV_LOGI("topic %s = %s", msg->topic, msg->value.value.str);
    } else {
        JSDRV_LOGI("topic %s = <bin>", msg->topic);
    }

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

static void handle_in_timesync_sync(struct jsdrvp_mb_dev_s * d,
        struct mb_stdmsg_publish_s * publish, uint32_t value_size) {
    // Only respond to sync REQUESTS from the device.  Ignore any
    // SYNC stdmsg arriving on a different topic (e.g., echo of our
    // own !rsp) so we don't reply twice or loop.
    if (!jsdrv_cstr_ends_with(publish->topic, "/!req")) {
        return;
    }
    int64_t utc_recv = jsdrv_time_utc();
    uint32_t body_size = value_size - sizeof(struct mb_stdmsg_header_s);
    if (body_size < sizeof(struct mb_timesync_sync_v1_s)) {
        JSDRV_LOGW("timesync_sync too short: %" PRIu32, value_size);
        return;
    }
    struct mb_stdmsg_header_s * inner = (struct mb_stdmsg_header_s *) &publish->value;
    const struct mb_timesync_sync_v1_s * src =
        (const struct mb_timesync_sync_v1_s *) (inner + 1);

    // Build response: inner TIMESYNC_SYNC header + body
    uint8_t resp_buf[sizeof(struct mb_stdmsg_header_s)
                     + sizeof(struct mb_timesync_sync_v1_s)];
    struct mb_stdmsg_header_s * resp_hdr = (struct mb_stdmsg_header_s *) resp_buf;
    resp_hdr->version = 0;
    resp_hdr->type = MB_STDMSG_TIMESYNC_SYNC;
    resp_hdr->origin_prefix = 'h';
    resp_hdr->metadata = 0;

    struct mb_timesync_sync_v1_s * body =
        (struct mb_timesync_sync_v1_s *) (resp_hdr + 1);
    body->count_start = src->count_start;       // echo
    body->utc_recv = utc_recv;
    body->utc_send = jsdrv_time_utc();
    body->count_end = 0;                        // device fills

    // Route the response back to the SAME pubsub instance that sent the
    // request.  publish->topic is something like "c/ts/!req" or
    // "s/ts/!req"; we send to "{prefix}/ts/!rsp" so that the response
    // reaches the timesync task in the matching instance.  Without this,
    // both ctrl and sensor responses would go to the ctrl's c/ts/!rsp,
    // and the ctrl's task would process the sensor's count_start as if
    // it were its own — corrupting the algorithm.
    char rsp_topic[12];
    rsp_topic[0] = publish->topic[0];   // 'c' or 's'
    rsp_topic[1] = '/';
    rsp_topic[2] = 't';
    rsp_topic[3] = 's';
    rsp_topic[4] = '/';
    rsp_topic[5] = '!';
    rsp_topic[6] = 'r';
    rsp_topic[7] = 's';
    rsp_topic[8] = 'p';
    rsp_topic[9] = '\0';

    struct jsdrv_union_s value = jsdrv_union_bin(resp_buf, sizeof(resp_buf));
    value.type = JSDRV_UNION_STDMSG;
    publish_to_device(d, rsp_topic, &value);
    JSDRV_LOGD2("timesync sync rsp: count=%" PRIu64 " utc_recv=%" PRIi64,
                src->count_start, utc_recv);
}

static void handle_in_timesync_map(struct jsdrvp_mb_dev_s * d,
        struct mb_stdmsg_publish_s * publish, uint32_t value_size) {
    uint32_t body_size = value_size - sizeof(struct mb_stdmsg_header_s);
    if (body_size < sizeof(struct mb_timesync_map_v1_s)) {
        JSDRV_LOGW("timesync_map too short: %" PRIu32, value_size);
        return;
    }
    struct mb_stdmsg_header_s * inner = (struct mb_stdmsg_header_s *) &publish->value;
    const struct mb_timesync_map_v1_s * body =
        (const struct mb_timesync_map_v1_s *) (inner + 1);

    // Cache the map keyed by the publishing task's prefix char.  Which
    // prefix is "authoritative" for a given signal is a device-driver
    // policy question answered via jsdrvp_mb_dev_time_map(dev, prefix).
    uint8_t idx = (uint8_t) publish->topic[0];
    struct jsdrv_time_map_s * map = &d->time_maps[idx];
    map->offset_time = body->utc;
    map->offset_counter = body->counter;
    map->counter_rate = ((double) body->counter_rate) / ((double) (1ULL << 32));
    JSDRV_LOGD1("timesync map %s: utc=%" PRIi64 " counter=%" PRIu64 " rate=%f",
                publish->topic, body->utc, body->counter, map->counter_rate);
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

    // Check for timesync STDMSG values.  These are consumed for their
    // side effects (host-side response, device cache update) and ALSO
    // forwarded to frontend subscribers below so tools can observe them.
    if ((value_type == JSDRV_UNION_STDMSG)
            && (value_size >= sizeof(struct mb_stdmsg_header_s))) {
        struct mb_stdmsg_header_s * inner_hdr = (struct mb_stdmsg_header_s *) &publish->value;
        if (inner_hdr->type == MB_STDMSG_TIMESYNC_SYNC) {
            handle_in_timesync_sync(d, publish, value_size);
        } else if (inner_hdr->type == MB_STDMSG_TIMESYNC_MAP) {
            handle_in_timesync_map(d, publish, value_size);
        }
    }

    if (jsdrv_cstr_ends_with(topic.topic, "/./!pong")
            && (value_type == JSDRV_UNION_STR)
            && jsdrv_cstr_casecmp(PUBSUB_DISCONNECT_STR, m->payload.str)) {
        state_machine_process(d, EV_PUBSUB_FLUSH);
        jsdrvp_msg_free(d->context, m);
    } else if (jsdrv_cstr_ends_with(publish->topic, "/!rsp")
               && (value_type == JSDRV_UNION_STDMSG)
               && value_size >= sizeof(struct mb_stdmsg_header_s)) {
        // Route to state fetch if active
        struct mb_stdmsg_header_s * rsp_hdr = (struct mb_stdmsg_header_s *) &publish->value;
        if (d->state_fetch.phase != STATE_FETCH_PHASE_IDLE
                && value_size >= sizeof(struct mb_stdmsg_header_s) + 4) {
            // Match on transaction_id (first 4 bytes after inner stdmsg header)
            uint32_t rsp_txn;
            memcpy(&rsp_txn, (const uint8_t *)&publish->value + sizeof(struct mb_stdmsg_header_s), 4);
            if (rsp_txn == d->state_fetch.transaction_id) {
                if (rsp_hdr->type == MB_STDMSG_STATE) {
                    state_fetch_on_rsp(d, &m->value);
                    jsdrvp_msg_free(d->context, m);
                    return;
                } else if (rsp_hdr->type == 0x07) {  // MB_STDMSG_MEM
                    state_fetch_on_meta_rsp(d, &m->value);
                    jsdrvp_msg_free(d->context, m);
                    return;
                }
            }
        }
        // Check inner type: only handle confirmed delivery (PUBSUB_RESPONSE).
        // All other types (e.g. MEM) fall through to the upper driver.
        if (rsp_hdr->type == MB_STDMSG_PUBSUB_RESPONSE
                && value_size >= (sizeof(struct mb_stdmsg_header_s)
                    + sizeof(struct mb_stdmsg_pubsub_response_s))) {
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
    if (msg->inner_msg_type == JSDRV_MSG_TYPE_LL_TERMINATED) {
        // Barrier B1: LL has stopped producing.  Any downstream cmds
        // we issue would be dropped, so force the state machine to
        // CLOSED and mark for graceful UL exit.
        JSDRV_LOGI("handle_rsp LL_TERMINATED %s", d->ll.prefix);
        d->ll_terminated = true;
        d->state = ST_CLOSED;
        // Restore NORMAL type so the message returns to the free pool
        // cleanly via jsdrvp_msg_free.
        msg->inner_msg_type = JSDRV_MSG_TYPE_NORMAL;
        jsdrvp_msg_free(d->context, msg);
        return true;
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


static JSDRV_THREAD_RETURN_TYPE driver_thread(JSDRV_THREAD_ARG_TYPE lpParam) {
    struct jsdrvp_mb_dev_s *d = (struct jsdrvp_mb_dev_s *) lpParam;
    JSDRV_LOGI("MB USB upper-level thread started for %s d=%p", d->ll.prefix, (void *) d);
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
        if (((d->finalize_pending) || (d->ll_terminated)) && (d->state == ST_CLOSED)) {
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

        if ((d->timeout_utc > 0) && (jsdrv_time_utc() >= d->timeout_utc)) {
            d->timeout_utc = 0;
            state_machine_process(d, EV_TIMEOUT);
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

    // Barrier B2: if we are exiting because the LL side terminated
    // (forced removal), emit DEVICE_REMOVE into msg_backend as our
    // very last act.  Because this thread is the sole producer of
    // device-scoped messages for this device in msg_backend, the
    // frontend sees every prior message before this one and can
    // safely free the frontend_dev_s when it processes DEVICE_REMOVE.
    if (d->ll_terminated && !d->finalize_pending) {
        struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc(d->context);
        jsdrv_cstr_copy(msg->topic, JSDRV_MSG_DEVICE_REMOVE, sizeof(msg->topic));
        msg->value.type = JSDRV_UNION_STR;
        msg->value.value.str = msg->payload.str;
        jsdrv_cstr_copy(msg->payload.str, d->ll.prefix, sizeof(msg->payload.str));
        jsdrvp_backend_send(d->context, msg);
    }

    JSDRV_LOGI("MB USB upper-level thread done %s d=%p", d->ll.prefix, (void *) d);
    JSDRV_THREAD_RETURN();
}

static void join(struct jsdrvp_ul_device_s * device) {
    struct jsdrvp_mb_dev_s * d = (struct jsdrvp_mb_dev_s *) device;
    // If the thread is still running (requested-close path), send
    // FINALIZE; on forced-remove the thread has already exited via
    // the LL_TERMINATED barrier and this message is a harmless no-op.
    JSDRV_LOGI("ul join(%s d=%p): send FINALIZE", d->ll.prefix, (void *) d);
    jsdrvp_send_finalize_msg(d->context, d->ul.cmd_q, JSDRV_MSG_FINALIZE);
    // 10 s is a diagnostic cap, not an expected wait: the LL_TERMINATED
    // barrier (forced removal) or the FINALIZE command (requested
    // close) guarantees the thread exits in a bounded time.  Anything
    // approaching this cap indicates a lost barrier or a USB stall and
    // is a real bug to investigate.
    int32_t jrc = jsdrv_thread_join(&d->thread, 10000);
    JSDRV_LOGI("ul join(%s d=%p): joined rc=%d", d->ll.prefix, (void *) d, (int) jrc);
    if (jrc) {
        // Thread join timed out: the UL thread is still running and
        // will UAF if we proceed to free d.  Leak d (and its ul.cmd_q)
        // rather than crashing.  The LEAK log arms attention — the
        // root cause (stuck barrier / hung USB op) must be fixed.
        JSDRV_LOGE("ul join(%s d=%p): LEAK - thread still running, refusing to free", d->ll.prefix, (void *) d);
        return;
    }
    if (d->drv && d->drv->finalize) {
        d->drv->finalize(d->drv);
    }
    if (d->ul.cmd_q) {
        msg_queue_finalize(d->ul.cmd_q, d->context);
        d->ul.cmd_q = NULL;
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

const struct jsdrv_time_map_s * jsdrvp_mb_dev_time_map(
        struct jsdrvp_mb_dev_s * dev, char prefix) {
    if (!dev) {
        return NULL;
    }
    const struct jsdrv_time_map_s * map = &dev->time_maps[(uint8_t) prefix];
    if (map->counter_rate <= 0.0) {
        return NULL;  // not yet populated in this session
    }
    return map;
}
