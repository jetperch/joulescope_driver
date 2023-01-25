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

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "js220_api.h"
#include "jsdrv_prv/js220_stats.h"
#include "jsdrv_prv/downsample.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv_prv/sample_buffer_f32.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv/version.h"
#include "tinyprintf.h"
#include <inttypes.h>

/*
 * Streaming data handling
 *
 * All streaming signals start with a u32 sample id followed by the
 * channel data.  The sample_id is always reported at 2 Msps, regardless
 * of the actual output sample rate.  This value rolls over every
 * ~2147 seconds = ~35 minutes.  The JS220 counts the underlying sample id
 * as u56 which should never roll over in practice (~1142 years).
 *
 * Current, voltage, & power are f32 that arrive at a specified sampling
 * frequency, common to all signals, from 1 ksps to 1 Msps.  At 1 Msps,
 * only current and voltage are allowed.  This module must compute power.
 * At lower sampling frequencies, the instrument provides all three as
 * requested.
 *
 * In the present implementation, the f32 data is uncompressed.
 *
 * The remaining signals are all fixed at 2 Msps.
 *
 * ADC u16 data is actual data, never compressed.
 *
 * Current range u4 is compressed in 16-bit chunks:
 *      zzzzzzzz_zzzzxxxx   value=x, length=z => 1 to 4096 (z + 1)
 *
 * Binary u1 signals are represented as:
 *      0xxxxxxx:           Actual: 7 bits in 8 bits
 *      10xzzzzz:           value=x, length=z => 8 to 39    (z + 8)
 *      110xzzzz_zzzzzzzz   value=x, length=z => 40 to 4135 (z + 40)
 *
 * Voltage range u1 is compressed like other binary signals.
 *
 * UART u8 data is uncompressed.
 *
 * One goal of this module is to provide an uncompressed stream for each
 * signal.  Due to compression, channels may provide updates at different
 * times.  The target update rate  * is ~100 Hz at 1 Msps and ~10 Hz at 1 ksps.
 *
 * Synchronizing channels is an optional operation that is not included
 * in this driver.  See stream_buffer.h which connects easily to device
 * and produces {p}/s/stream/!data with the stream buffer instance as
 * the associated data.
 */


#define TIMEOUT_MS  (1000U)
#define SENSOR_COMMAND_TIMEOUT_MS  (3000U)
#define FRAME_SIZE_BYTES           (512U)
#define FRAME_SIZE_U32             (FRAME_SIZE_BYTES / 4)
#define MEM_SIZE_MAX               (512U * 1024U)
#define SAMPLING_FREQUENCY         (2000000U)
#define STREAM_PAYLOAD_FULL        (JSDRV_STREAM_DATA_SIZE - JSDRV_STREAM_HEADER_SIZE - JS220_USB_FRAME_LENGTH)

extern const struct jsdrvp_param_s js220_params[];

static const char * fw_ver_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"The controller firmware version.\","
    "\"detail\": \"The version is formatted as major8.minor8.patch16.\","
    "\"format\": \"version\","
    "\"flags\": [\"ro\"]"
"}";

static const char * hw_ver_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"The hardware firmware version.\","
    "\"detail\": \"The version is formatted as major8.minor8.patch16.\","
    "\"format\": \"version\","
    "\"flags\": [\"ro\"]"
"}";

static const char * reset_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"Reset to a boot target.\","
     "\"options\": ["
         "[0, \"app\"],"
         "[1, \"update1\"],"
         "[2, \"update2\"]"
     "]"
"}";

static const char * sampling_frequency_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"The sampling frequency.\","
    "\"default\": 2000000,"
    "\"options\": ["
        "[2000000, \"2 MHz\"],"  // limited to 1000000 for i, v, p, all others fixed at 2000000
        "[1000000, \"1 MHz\"],"
        "[500000, \"500 kHz\"],"
        "[200000, \"200 kHz\"],"
        "[100000, \"100 kHz\"],"
        "[50000, \"50 kHz\"],"
        "[20000, \"20 kHz\"],"
        "[10000, \"10 kHz\"],"
        "[5000, \"5 kHz\"],"
        "[2000, \"2 kHz\"],"
        "[1000, \"1 kHz\"],"
        "[500, \"500 Hz\"],"
        "[200, \"200 Hz\"],"
        "[100, \"100 Hz\"],"
        "[50, \"50 Hz\"],"
        "[20, \"20 Hz\"],"
        "[10, \"10 Hz\"],"
        "[5, \"5 Hz\"],"
        "[2, \"2 Hz\"],"
        "[1, \"1 Hz\"]"
    "]"
"}";

enum state_e {
    ST_NOT_PRESENT = 0,  //
    ST_CLOSED = 1,
    ST_OPENING = 2,
    ST_OPEN = 3,
};


struct field_def_s {
    const char * ctrl_topic;
    const char * data_topic;
    uint8_t field_id;
    uint8_t index;
    uint8_t element_type;               // jsdrv_data_type_e
    uint8_t element_size_bits;
    uint8_t downsample_min;
};

#define FIELD(ctrl_topic_, data_topic_, field_, index_, type_, size_, downsample_) {    \
    .ctrl_topic = (ctrl_topic_),                                                        \
    .data_topic = (data_topic_),                                                        \
    .field_id=JSDRV_FIELD_##field_,                                                     \
    .index=(index_),                                                                    \
    .element_type=JSDRV_DATA_TYPE_##type_,                                              \
    .element_size_bits=(size_),                                                         \
    .downsample_min=(downsample_),                                                      \
}

struct field_def_s PORT_MAP[] = {
        //   (ctrl field,       data field, jsdrv_field_e,  index, type, el_size_bits, downsample_min)
        FIELD("s/adc/0/ctrl",   "s/adc/0/!data",   RAW,         0, INT,   16, 1),  // 0 + 16
        FIELD("s/adc/1/ctrl",   "s/adc/1/!data",   RAW,         1, INT,   16, 1),  // 1
        FIELD("s/adc/2/ctrl",   "s/adc/2/!data",   RAW,         2, INT,   16, 1),  // 2
        FIELD("s/adc/3/ctrl",   "s/adc/3/!data",   RAW,         3, INT,   16, 1),  // 3
        FIELD("s/i/range/ctrl", "s/i/range/!data", RANGE,       0, UINT,  4, 1),   // 4
        FIELD("s/i/ctrl",       "s/i/!data",       CURRENT,     0, FLOAT, 32, 2),  // 5
        FIELD("s/v/ctrl",       "s/v/!data",       VOLTAGE,     0, FLOAT, 32, 2),  // 6
        FIELD("s/p/ctrl",       "s/p/!data",       POWER,       0, FLOAT, 32, 2),  // 7
        FIELD("s/gpi/0/ctrl",   "s/gpi/0/!data",   GPI,         0, UINT,  1, 1),  // 8
        FIELD("s/gpi/1/ctrl",   "s/gpi/1/!data",   GPI,         1, UINT,  1, 1),  // 9
        FIELD("s/gpi/2/ctrl",   "s/gpi/2/!data",   GPI,         2, UINT,  1, 1),  // 10
        FIELD("s/gpi/3/ctrl",   "s/gpi/3/!data",   GPI,         3, UINT,  1, 1),  // 11
        FIELD("s/gpi/7/ctrl",   "s/gpi/7/!data",   GPI,         7, UINT,  1, 1),  // 12 trigger
        FIELD("s/uart/0/ctrl",  "s/uart/0/!data",  UART,        0, UINT,  8, 1),  // 13 8-bit only
        FIELD("s/stats/ctrl",   "s/stats/value",   UNDEFINED,   0, UNDEFINED,   0, 0),  // 14 js220_statistics_raw_s
        FIELD(NULL, NULL, UNDEFINED,   0, UINT,   8, 0),  // 15 reserved and unavailable
};
#define PORT_ID_CURRENT (5 + 16)
#define PORT_ID_VOLTAGE (6 + 16)
#define PORT_ID_POWER   (7 + 16)
#define COMPUTE_POWER_MASK ((1 << PORT_ID_CURRENT) | (1 << PORT_ID_VOLTAGE) | (1 << PORT_ID_POWER))
#define PORTS_LENGTH (16)  // but last one is reserved

JSDRV_STATIC_ASSERT(PORTS_LENGTH == JSDRV_ARRAY_SIZE(PORT_MAP), ports_length);

enum break_e {
    BREAK_NONE = 0,
    BREAK_CONNECT = 1,
    BREAK_PUBSUB_TOPIC = 2,
};

struct port_s {
    struct jsdrv_downsample_s * downsample;
    uint32_t decimate_factor;
    uint64_t sample_id_next;
    struct jsdrvp_msg_s * msg_in;  // one for each port
};

struct dev_s {
    struct jsdrvp_ul_device_s ul; // MUST BE FIRST!
    struct jsdrvp_ll_device_s ll;
    struct jsdrv_context_s * context;
    uint16_t out_frame_id;
    uint16_t in_frame_id;
    uint32_t stream_in_port_enable;

    struct port_s ports[PORTS_LENGTH]; // one for each port
    enum break_e ll_await_break_on;
    bool ll_await_break;
    char ll_await_break_topic[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_union_s ll_await_break_value;
    volatile bool do_exit;
    jsdrv_thread_t thread;
    uint8_t state;  // state_e

    struct sbuf_f32_s i_buf;
    struct sbuf_f32_s v_buf;
    struct sbuf_f32_s p_buf;

    // memory operations
    struct js220_port3_header_s mem_hdr;
    uint32_t mem_offset_valid;  // offset for completed mem_data.
    uint32_t mem_offset_sent;   // offset for write sent mem_data.
    uint8_t * mem_data;         // read/write data
    struct jsdrv_topic_s mem_topic;
};

const char * MEM_C[] = {"app", "upd1", "upd2", "storage", "log", "acfg", "bcfg", "pers", NULL};
const uint8_t MEM_C_U8[] = {
        JS220_PORT3_REGION_CTRL_APP,
        JS220_PORT3_REGION_CTRL_UPDATER1,
        JS220_PORT3_REGION_CTRL_UPDATER2,
        JS220_PORT3_REGION_CTRL_STORAGE,
        JS220_PORT3_REGION_CTRL_LOGGING,
        JS220_PORT3_REGION_CTRL_APP_CONFIG,
        JS220_PORT3_REGION_CTRL_BOOTLOADER_CONFIG,
        JS220_PORT3_REGION_CTRL_PERSONALITY,
        JS220_PORT3_REGION_CTRL_UNKNOWN,
};
JSDRV_STATIC_ASSERT(JSDRV_ARRAY_SIZE(MEM_C) == JSDRV_ARRAY_SIZE(MEM_C_U8), mem_c_arrays);

const char * MEM_S[] = {"app1", "app2", "cal_t", "cal_a", "cal_f", "pers", "id", NULL};
const uint8_t MEM_S_U8[] = {
        JS220_PORT3_REGION_SENSOR_APP1,
        JS220_PORT3_REGION_SENSOR_APP2,
        JS220_PORT3_REGION_SENSOR_CAL_TRIM,
        JS220_PORT3_REGION_SENSOR_CAL_ACTIVE,
        JS220_PORT3_REGION_SENSOR_CAL_FACTORY,
        JS220_PORT3_REGION_SENSOR_PERSONALITY,
        JS220_PORT3_REGION_SENSOR_ID,
        JS220_PORT3_REGION_SENSOR_UNKNOWN,
};
JSDRV_STATIC_ASSERT(JSDRV_ARRAY_SIZE(MEM_S) == JSDRV_ARRAY_SIZE(MEM_S_U8), mem_s_arrays);

static bool handle_rsp(struct dev_s * d, struct jsdrvp_msg_s * msg);

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

typedef bool (*msg_filter_fn)(void * user_data, struct dev_s * d, struct jsdrvp_msg_s * msg);

static bool msg_filter_none(void * user_data, struct dev_s * d, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    (void) d;
    (void) msg;
    return false;
}

static bool msg_filter_by_topic(void * user_data, struct dev_s * d, struct jsdrvp_msg_s * msg) {
    (void) d;
    const char * topic = (const char *) user_data;
    return (0 == strcmp(msg->topic, topic));
}

static struct jsdrvp_msg_s * ll_await(struct dev_s * d, msg_filter_fn filter_fn, void * filter_user_data, uint32_t timeout_ms) {
    uint32_t t_now = jsdrv_time_ms_u32();
    uint32_t t_end = t_now + timeout_ms;
    d->ll_await_break = false;

    while (!d->ll_await_break && !d->do_exit) {
#if _WIN32
        HANDLE h = msg_queue_handle_get(d->ll.rsp_q);
        WaitForSingleObject(h, timeout_ms);
#else
        struct pollfd fds = {
            .fd = msg_queue_handle_get(d->ll.rsp_q),
            .events = POLLIN,
            .revents = 0,
        };
        poll(&fds, 1, 2);
#endif
        struct jsdrvp_msg_s * m = msg_queue_pop_immediate(d->ll.rsp_q);
        if (m) {
            JSDRV_LOGD1("ll_await, process %s", m->topic);
            if (filter_fn(filter_user_data, d, m)) {
                return m;
            } else {
                handle_rsp(d, m);
            }
        }
        t_now = jsdrv_time_ms_u32();
        timeout_ms = t_end - t_now;
        if ((timeout_ms > (1U << 31U)) || (timeout_ms == 0)) {
            JSDRV_LOGW("ll_await timed out");
            return NULL;
        }
    }
    return NULL;
}

static struct jsdrvp_msg_s * ll_await_topic(struct dev_s * d, const char * topic, uint32_t timeout_ms) {
    return ll_await(d, msg_filter_by_topic, (void *) topic, timeout_ms);
}

#if 0
static int32_t jsdrvb_ctrl_out(struct dev_s * d, usb_setup_t setup, const void * buffer) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_CTRL_OUT, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_USB_CTRL;
    m->extra.bkusb_ctrl.setup = setup;
    if (setup.s.wLength > sizeof(m->payload.bin)) {
        JSDRV_LOGE("ctrl_out too big: %d", (int) setup.s.wLength);
        jsdrvp_msg_free(d->context, m);
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    memcpy(m->payload.bin, buffer, setup.s.wLength);
    m->value.size = setup.s.wLength;

    msg_queue_push(d->ll.cmd_q, m);
    m = ll_await_topic(d, JSDRV_USBBK_MSG_CTRL_OUT, TIMEOUT_MS);
    if (!m) {
        JSDRV_LOGW("ctrl_out timed out");
        return JSDRV_ERROR_TIMED_OUT;
    }
    jsdrvp_msg_free(d->context, m);
    return 0;
}
#endif

static int32_t jsdrvb_ctrl_in(struct dev_s * d, usb_setup_t setup, void * buffer, uint32_t * size) {
    int32_t rv = 0;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_CTRL_IN, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_USB_CTRL;
    m->extra.bkusb_ctrl.setup = setup;

    msg_queue_push(d->ll.cmd_q, m);
    m = ll_await_topic(d, JSDRV_USBBK_MSG_CTRL_IN, TIMEOUT_MS);
    if (!m) {
        JSDRV_LOGW("ctrl_in timed out");
        return JSDRV_ERROR_TIMED_OUT;
    }
    if (m->value.size > setup.s.wLength) {
        JSDRV_LOGW("ctrl_in returned too much data");
        rv = JSDRV_ERROR_TOO_BIG;
    } else {
        memcpy(buffer, m->payload.bin, m->value.size);
        if (size) {
            *size = m->value.size;
        }
    }
    jsdrvp_msg_free(d->context, m);
    return rv;
}

static int32_t jsdrvb_bulk_in_stream_open(struct dev_s * d) {
    int32_t rv = 0;
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, &jsdrv_union_i32(0));
    m->extra.bkusb_stream.endpoint = JS220_USB_EP_BULK_IN;
    msg_queue_push(d->ll.cmd_q, m);
    m = ll_await_topic(d, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, TIMEOUT_MS);
    if (!m) {
        JSDRV_LOGW("jsdrvb_bulk_in_stream_open timed out");
        return JSDRV_ERROR_TIMED_OUT;
    } else if (m->value.value.i32) {
        JSDRV_LOGW("jsdrvb_bulk_in_stream_open failed %d", m->value.value.i32);
        rv = m->value.value.i32;
    }
    jsdrvp_msg_free(d->context, m);
    return rv;
}

static struct jsdrvp_msg_s * bulk_out_factory(struct dev_s * d, uint8_t port_id, uint32_t payload_size) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_OUT_DATA, &jsdrv_union_i32(0));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->extra.bkusb_stream.endpoint = JS220_USB_EP_BULK_OUT;
    m->value.size = sizeof(uint32_t) + payload_size;
    uint32_t * p_u32 = (uint32_t *) m->payload.bin;
    *p_u32 = js220_frame_hdr_pack(d->out_frame_id++, payload_size, port_id);
    return m;
}

static int32_t bulk_out_publish(struct dev_s * d, const char * topic, const struct jsdrv_union_s * value) {
    uint16_t length = sizeof(struct js220_publish_s);
    struct jsdrvp_msg_s * m = bulk_out_factory(d, 1, 0);
    struct js220_publish_s * p = (struct js220_publish_s *) &m->payload.bin[4];
    char buf[32];
    jsdrv_union_value_to_str(value, buf, (uint32_t) sizeof(buf), 1);
    JSDRV_LOGI("publish to dev %s %s", topic, buf);
    memset(p, 0, sizeof(*p) + sizeof(union jsdrv_union_inner_u));
    jsdrv_cstr_copy(p->topic, topic, sizeof(p->topic));
    p->type = value->type;
    p->flags = value->flags;
    p->op = value->op;
    p->app = value->app;
    if ((p->type == JSDRV_UNION_JSON) || (p->type == JSDRV_UNION_STR)) {
        if (jsdrv_cstr_copy((char *) p->data, value->value.str, JS220_PUBSUB_DATA_LENGTH_MAX)) {
            JSDRV_LOGW("bulk_out_publish(%s) string truncated", topic);
        }
        length += (uint16_t) strlen((char *) p->data);
    } else if (p->type == JSDRV_UNION_BIN) {
        size_t sz = value->size;
        if (value->size > JS220_PUBSUB_DATA_LENGTH_MAX) {
            JSDRV_LOGW("bulk_out_publish(%s) bin truncated", topic);
            sz = JS220_PUBSUB_DATA_LENGTH_MAX;
        }
        memcpy(p->data, value->value.bin, sz);
        length += (uint16_t) sz;
    } else {
        memcpy(p->data, &value->value.u64, sizeof(uint64_t));
        length += (uint16_t) sizeof(uint64_t);
    }
    m->value.size += length;
    struct js220_frame_hdr_s * hdr = (struct js220_frame_hdr_s *) &m->payload.bin[0];
    hdr->length += length;
    msg_queue_push(d->ll.cmd_q, m);
    return 0;
}

static void send_to_frontend(struct dev_s * d, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(d->context, "", value);
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s", d->ll.prefix, subtopic);
    jsdrvp_backend_send(d->context, m);
}

static int32_t send_return_code_to_frontend(struct dev_s * d, const char * subtopic, int32_t rc) {
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(d->context, "", &jsdrv_union_i32(rc));
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s%c", d->ll.prefix, subtopic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    jsdrvp_backend_send(d->context, m);
    return rc;
}

static void update_state(struct dev_s * d, enum state_e state) {
    d->state = state;
    send_to_frontend(d, "h/state", &jsdrv_union_u32_r(d->state));
}

static int32_t d_ctrl_req(struct dev_s * d, uint8_t op) {
    uint8_t buf_in[1];
    usb_setup_t setup = { .s = {
            .bmRequestType = USB_REQUEST_TYPE(IN, VENDOR, DEVICE),
            .bRequest = op,
            .wValue = 0,
            .wIndex = 0,
            .wLength = sizeof(buf_in),
    }};
    uint32_t sz = 0;
    int32_t rv = jsdrvb_ctrl_in(d, setup, buf_in, &sz);
    if (rv) {
        goto exit;
    }
    if (sz != sizeof(buf_in)) {
        rv = JSDRV_ERROR_INVALID_MESSAGE_LENGTH;
        goto exit;
    }
    rv = buf_in[0];
exit:
    if (rv) {
        JSDRV_LOGW("d_ctrl_req(%d) returned %" PRId32, (int) op, rv);
    }
    return rv;
}

static int32_t ll_await_pubsub_topic(struct dev_s * d, const char * topic, uint32_t timeout_ms) {
    jsdrv_cstr_copy(d->ll_await_break_topic, topic, sizeof(d->ll_await_break_topic));
    d->ll_await_break_on = BREAK_PUBSUB_TOPIC;
    ll_await(d, msg_filter_none, NULL, timeout_ms);
    if (!d->ll_await_break) {
        JSDRV_LOGE("ll_await_pubsub_topic(%s) timed out", topic);
        return JSDRV_ERROR_TIMED_OUT;
    }
    return 0;
}

static int32_t ping_wait(struct dev_s * d, uint32_t value) {
    JSDRV_LOGD1("ping_wait(%" PRIu32 ")", value);
    bulk_out_publish(d, JS220_TOPIC_PING, &jsdrv_union_u32(value));

    if (!ll_await_pubsub_topic(d, JS220_TOPIC_PONG, 1000)) {
        if ((d->ll_await_break_value.type != JSDRV_UNION_U32) || (d->ll_await_break_value.value.u32 != value)) {
            JSDRV_LOGW("ping_wait value mismatch: send=%" PRIu32 ", recv=%" PRIu32,
                     value, d->ll_await_break_value.value.u32);
        } else {
            JSDRV_LOGD1("ping_wait(%" PRIu32 ") done", value);
        }
        return 0;
    } else {
        JSDRV_LOGW("ping_wait(%" PRIu32 ") timed out", value);
        return JSDRV_ERROR_TIMED_OUT;
    }
}

static int32_t wait_for_connect(struct dev_s * d) {
    // only allowed response at this time is BULK IN, port 0, JS220_PORT0_OP_CONNECT
    // however, process all to be robust
    d->ll_await_break_on = BREAK_CONNECT;
    ll_await(d, msg_filter_none, NULL, 1000);
    if (!d->ll_await_break) {
        JSDRV_LOGE("OP_CONNECT timed out");
        return JSDRV_ERROR_TIMED_OUT;
    }
    return 0;
}

static int32_t d_open_ll(struct dev_s * d, int32_t opt) {
    JSDRV_LOGI("open_ll");
    int32_t rc;
    d->ll_await_break_on = BREAK_NONE;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_MSG_OPEN, &jsdrv_union_i32(opt & 1));

    if (d->state == ST_OPEN) {
        return JSDRV_ERROR_IN_USE;
    }
    msg_queue_push(d->ll.cmd_q, m);
    m = ll_await_topic(d, JSDRV_MSG_OPEN, TIMEOUT_MS);
    if (!m) {
        return JSDRV_ERROR_TIMED_OUT;
    }
    update_state(d, ST_OPENING);
    rc = m->value.value.i32;
    jsdrvp_msg_free(d->context, m);
    if (rc) {
        JSDRV_LOGE("open_ll failed");
        update_state(d, ST_CLOSED);
        return rc;
    }
    return 0;
}

static int32_t d_open(struct dev_s * d, int32_t opt) {
    JSDRV_LOGI("open");
    int32_t rc;
    rc = d_ctrl_req(d, JS220_CTRL_OP_DISCONNECT);
    if (rc) {
        JSDRV_LOGI("jsdrvb_bulk_in_stream_open disconnect: %d", rc);
        // ok, just continue on.
    }

    d->out_frame_id = 0;
    d->stream_in_port_enable = 0x000f;  // always enable ports 0, 1, 2, 3
    rc = jsdrvb_bulk_in_stream_open(d);
    if (rc) {
        d->stream_in_port_enable = 0;
        JSDRV_LOGE("jsdrvb_bulk_in_stream_open failed: %d", rc);
        return rc;
    }

    JSDRV_RETURN_ON_ERROR(d_ctrl_req(d, JS220_CTRL_OP_CONNECT));

    if (JSDRV_DEVICE_OPEN_MODE_RAW != opt) {  // normal operation
        JSDRV_RETURN_ON_ERROR(wait_for_connect(d));
        JSDRV_RETURN_ON_ERROR(bulk_out_publish(d, "$", &jsdrv_union_null()));
        JSDRV_RETURN_ON_ERROR(ping_wait(d, 1));
        JSDRV_RETURN_ON_ERROR(bulk_out_publish(d, "?", &jsdrv_union_null()));
        JSDRV_RETURN_ON_ERROR(ping_wait(d, 2));
    } else {
        send_to_frontend(d, "h/!reset$", &jsdrv_union_cjson_r(reset_meta));
    }

    JSDRV_LOGI("open complete");
    update_state(d, ST_OPEN);
    return 0;
}

static int32_t d_close(struct dev_s * d) {
    int32_t rv = 0;
    JSDRV_LOGI("close");
    if ((d->state == ST_OPENING) || (d->state == ST_OPEN)) {
        d->stream_in_port_enable = 0;  // disable all ports
        d_ctrl_req(d, JS220_CTRL_OP_DISCONNECT);  // ignore errors
        struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0));
        msg_queue_push(d->ll.cmd_q, m);
        m = ll_await_topic(d, JSDRV_MSG_CLOSE, 1000);
        if (!m) {
            rv = JSDRV_ERROR_TIMED_OUT;
        } else {
            rv = m->value.value.i32;
            jsdrvp_msg_free(d->context, m);
        }
        for (uint32_t idx = 0; idx < JSDRV_ARRAY_SIZE(d->ports); ++idx) {
            struct port_s * p = &d->ports[idx];
            if (NULL != p->msg_in) {
                jsdrvp_msg_free(d->context, p->msg_in);
                p->msg_in = NULL;
            }
        }
        update_state(d, ST_CLOSED);
    }
    return rv;
}

static bool stream_in_port_enable(struct dev_s * d, const char * topic, bool enable) {
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(PORT_MAP); ++i) {
        if (PORT_MAP[i].ctrl_topic && (0 == strcmp(PORT_MAP[i].ctrl_topic, topic))) {
            uint32_t mask = (0x00010000 << i);
            struct port_s * p = &d->ports[i];
            if (NULL != p->msg_in) {
                jsdrvp_msg_free(d->context, p->msg_in);
                p->msg_in = NULL;
            }
            if (NULL != p->downsample) {
                jsdrv_downsample_clear(p->downsample);
            }
            if (enable) {
                p->sample_id_next = 0;
                d->stream_in_port_enable |= mask;
            } else {
                d->stream_in_port_enable &= ~mask;
            }
            JSDRV_LOGD1("stream_in_port_enable port %s => 0x%08lx", topic, d->stream_in_port_enable);
            if (PORT_MAP[i].field_id == JSDRV_FIELD_CURRENT) {
                sbuf_f32_clear(&d->i_buf);
            } else if (PORT_MAP[i].field_id == JSDRV_FIELD_VOLTAGE) {
                sbuf_f32_clear(&d->v_buf);
            } else if (PORT_MAP[i].field_id == JSDRV_FIELD_POWER) {
                sbuf_f32_clear(&d->p_buf);
            }
            return (PORT_MAP[i].field_id != JSDRV_FIELD_POWER);  // todo when add decimate, always true when decimated
        }
    }
    JSDRV_LOGW("stream_in_port_enable port not found %s", topic);
    return false;
}

static bool handle_cmd_ctrl(struct dev_s * d, const char * topic, const struct jsdrv_union_s * value) {
    bool v = false;
    if (jsdrv_cstr_ends_with(topic, "/ctrl")) {
        jsdrv_union_to_bool(value, &v);
        if (stream_in_port_enable(d, topic, value)) {
            bulk_out_publish(d, topic, value);
        } else {
            struct jsdrv_topic_s t;
            jsdrv_topic_set(&t, topic);
            jsdrv_topic_suffix_add(&t, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
            send_to_frontend(d, t.topic, &jsdrv_union_i32(0));
        }
        return true;
    } else {
        return false;
    }
}

static void handle_rsp_ctrl(struct dev_s * d, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrv_topic_s t;
    bool v = false;
    if (jsdrv_cstr_ends_with(topic, "/ctrl?")) {
        jsdrv_topic_set(&t, topic);
        jsdrv_topic_suffix_remove(&t);
        jsdrv_union_to_bool(value, &v);
        stream_in_port_enable(d, t.topic, v);
    }
}

static int32_t mem_complete(struct dev_s * d, int32_t status) {
    JSDRV_LOGI("mem_complete(%d)", status);
    if (JS220_PORT3_OP_NONE == d->mem_hdr.op) {
        return status;
    }

    if ((0 == status) && (JS220_PORT3_OP_READ_REQ == d->mem_hdr.op)) {
        struct jsdrv_topic_s topic;
        jsdrv_topic_set(&topic, d->mem_topic.topic);
        jsdrv_topic_remove(&topic);
        jsdrv_topic_append(&topic, "!rdata");
        JSDRV_LOGD1("%s with %d bytes", topic.topic, d->mem_hdr.length);
        struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, topic.topic, &jsdrv_union_bin(d->mem_data, d->mem_hdr.length));
        jsdrvp_backend_send(d->context, m);
    }

    jsdrv_topic_suffix_add(&d->mem_topic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    m->value = jsdrv_union_i32(status);
    memcpy(m->topic, d->mem_topic.topic, d->mem_topic.length + 1);
    jsdrvp_backend_send(d->context, m);

    jsdrv_topic_clear(&d->mem_topic);
    memset(&d->mem_hdr, 0, sizeof(d->mem_topic));
    d->mem_offset_valid = 0;
    d->mem_offset_sent = 0;
    if (NULL != d->mem_data) {
        jsdrv_free(d->mem_data);
        d->mem_data = NULL;
    }
    return status;
}

static int32_t handle_cmd_mem(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    struct jsdrv_topic_s topic_holder;
    const char * topic = prefix_match_and_strip(d->ll.prefix, msg->topic);
    const char * const * table;
    const uint8_t * table_u8;

    if (d->mem_hdr.op) {
        JSDRV_LOGW("aborting ongoing memory operation");
        mem_complete(d, JSDRV_ERROR_ABORTED);
    }

    if (jsdrv_cstr_starts_with(topic, "h/mem/c/")) {
        table = MEM_C;
        table_u8 = MEM_C_U8;
    } else if (jsdrv_cstr_starts_with(topic, "h/mem/s/")) {
        table = MEM_S;
        table_u8 = MEM_S_U8;
    } else {
        JSDRV_LOGW("invalid mem region chk1: %s", topic);
        return send_return_code_to_frontend(d, topic, JSDRV_ERROR_PARAMETER_INVALID);
    }

    // parse topic into region/command
    jsdrv_topic_set(&topic_holder, topic + 8);
    char * region_str = topic_holder.topic;
    char * t = region_str;
    while (*t && (*t != '/')) {
        ++t;
    }
    if (*t == '/') {
        *t++ = 0;
    } else {
        JSDRV_LOGW("invalid mem region chk2: %s", topic);
        return send_return_code_to_frontend(d, topic, JSDRV_ERROR_PARAMETER_INVALID);
    }
    char * mem_cmd_str = t;

    int idx = 0;
    if (jsdrv_cstr_to_index(region_str, table, &idx)) {
        JSDRV_LOGW("Invalid mem region chk3: %s", msg->topic);
        return send_return_code_to_frontend(d, topic, JSDRV_ERROR_PARAMETER_INVALID);
    }

    jsdrv_topic_set(&d->mem_topic, msg->topic);
    struct jsdrvp_msg_s * msg_bk = bulk_out_factory(d, 3, sizeof(struct js220_port3_header_s));
    struct js220_port3_msg_s * m = (struct js220_port3_msg_s *) msg_bk->value.value.bin;
    memset(&m->hdr, 0, sizeof(m->hdr));
    m->hdr.region = table_u8[idx];
    if (0 == strcmp("!erase", mem_cmd_str)) {
        m->hdr.op = JS220_PORT3_OP_ERASE;
    } else if (0 == strcmp("!write", mem_cmd_str)) {
        if (msg->value.size > MEM_SIZE_MAX) {
            JSDRV_LOGW("write size too big: %d > %d", (int) msg->value.size, (int) MEM_SIZE_MAX);
            --d->out_frame_id;
            jsdrvp_msg_free(d->context, msg_bk);
            return mem_complete(d, JSDRV_ERROR_PARAMETER_INVALID);
        }
        m->hdr.op = JS220_PORT3_OP_WRITE_START;
        m->hdr.length = msg->value.size;
        d->mem_data = jsdrv_alloc(msg->value.size);
        memcpy(d->mem_data, msg->value.value.bin, msg->value.size);
    } else if (0 == strcmp("!read", mem_cmd_str)) {
        int32_t sz = MEM_SIZE_MAX;
        jsdrv_union_as_type(&msg->value, JSDRV_UNION_U32);
        if (msg->value.value.u32) {
            sz = msg->value.value.u32;
        }
        d->mem_data = jsdrv_alloc(sz);
        m->hdr.op = JS220_PORT3_OP_READ_REQ;
        m->hdr.length = sz;
    } else {
        JSDRV_LOGW("invalid mem op: %s", mem_cmd_str);
        --d->out_frame_id;
        jsdrvp_msg_free(d->context, msg_bk);
        jsdrv_topic_clear(&d->mem_topic);
        return send_return_code_to_frontend(d, topic, JSDRV_ERROR_PARAMETER_INVALID);
    }
    d->mem_hdr = m->hdr;
    JSDRV_LOGD1("mem cmd: region=%s, op=%s, length=%d", region_str, mem_cmd_str, (int) d->mem_hdr.length);
    msg_queue_push(d->ll.cmd_q, msg_bk);

    return 0;
}

static int32_t handle_reset(struct dev_s * d, int32_t target) {
    // target: boot_target_e in js220_ctrl boot.h: 0=app, 1=update1, 2=update2
    if ((target < 0) || (target > 2)) {
        JSDRV_LOGE("reset to invalid target %d", target);
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    JSDRV_LOGI("reset to %d", target);
    struct jsdrvp_msg_s *msg_bk = bulk_out_factory(d, 3, sizeof(struct js220_port3_header_s));
    struct js220_port3_msg_s *m = (struct js220_port3_msg_s *) msg_bk->value.value.bin;
    memset(&m->hdr, 0, sizeof(m->hdr));
    m->hdr.op = JS220_PORT3_OP_BOOT;
    m->hdr.arg = target;
    msg_queue_push(d->ll.cmd_q, msg_bk);
    return 0;
}

static int32_t on_sampling_frequency(struct dev_s * d,  const struct jsdrv_union_s * value) {
    struct jsdrv_union_s v = *value;
    uint32_t fs;
    uint32_t fs_in;
    if (jsdrv_union_as_type(&v, JSDRV_UNION_U32)) {
        JSDRV_LOGW("Could not process sampling frequency");
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    fs = v.value.u32;
    JSDRV_LOGI("on_sampling_frequency(%lu)", fs);
    for (uint32_t idx = 0; idx < (PORTS_LENGTH - 2); ++idx) {
        struct port_s *p = &d->ports[idx];
        if (p->downsample) {
            jsdrv_downsample_free(p->downsample);
            p->downsample = NULL;
        }
        if (p->msg_in) {
            jsdrvp_msg_free(d->context, p->msg_in);
            p->msg_in = NULL;
        }
        if (PORT_MAP[idx].element_type != JSDRV_DATA_TYPE_FLOAT) {
            p->decimate_factor = 1;
            continue;
        }
        fs_in = SAMPLING_FREQUENCY / PORT_MAP[idx].downsample_min;
        if (fs > fs_in) {
            fs = fs_in;
        }
        p->downsample = jsdrv_downsample_alloc(fs_in, fs);
        if (NULL == p->downsample) {
            JSDRV_LOGW("jsdrv_downsample_alloc failed");
        }
        p->decimate_factor = SAMPLING_FREQUENCY / fs;
    }
    return 0;
}

static bool handle_cmd(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    int32_t rc = 0;
    bool rv = true;
    if (!msg) {
        return false;
    }
    const char * topic = prefix_match_and_strip(d->ll.prefix, msg->topic);
    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            // full driver shutdown
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if (!topic) {
        JSDRV_LOGE("handle_cmd mismatch %s, %s", msg->topic, d->ll.prefix);
    } else if (topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, topic)) {
            int32_t opt = 0;
            if ((msg->value.type == JSDRV_UNION_U32) || (msg->value.type == JSDRV_UNION_I32)) {
                opt = msg->value.value.i32;
            }
            rc = d_open_ll(d, opt);
            if (0 == rc) {
                rc = d_open(d, opt);
                if (rc) {
                    d_close(d);
                }
            }
            send_to_frontend(d, JSDRV_MSG_OPEN "#", &jsdrv_union_i32(rc));
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, topic)) {
            rc = d_close(d);
            send_to_frontend(d, JSDRV_MSG_CLOSE "#", &jsdrv_union_i32(rc));
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, topic)) {
            // just finalize this upper-level driver (keep lower-level running)
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if (d->state != ST_OPEN) {
        send_return_code_to_frontend(d, topic, JSDRV_ERROR_CLOSED);
    } else if ((topic[0] == 'h') && (topic[1] == '/')) {
        JSDRV_LOGD1("handle_cmd local %s", topic);
        // handle any host-side parameters here.
        if (jsdrv_cstr_starts_with(topic, "h/mem/")) {
            handle_cmd_mem(d, msg);
        } else if (0 == strcmp("h/!reset", topic)) {   // value=target
            rc = handle_reset(d, msg->value.value.i32);
            send_return_code_to_frontend(d, topic, rc);
        } else if (0 == strcmp("h/timeout", topic)) {
            jsdrv_thread_sleep_ms(msg->value.value.u32);
            send_return_code_to_frontend(d, topic, 0);
        } else if (0 == strcmp("h/fs", topic)) {
            rc = on_sampling_frequency(d, &msg->value);
            send_return_code_to_frontend(d, topic, rc);
        } else {
            JSDRV_LOGE("topic invalid: %s", msg->topic);
            send_return_code_to_frontend(d, topic, JSDRV_ERROR_PARAMETER_INVALID);
        }
    } else {
        JSDRV_LOGD1("handle_cmd to device %s", topic);
        if (!handle_cmd_ctrl(d, topic, &msg->value)) {
            bulk_out_publish(d, topic, &msg->value);
        }
    }
    return rv;
}

static void handle_stream_in_port(struct dev_s * d, uint8_t port_id, uint32_t * p_u32, uint16_t size) {
    struct field_def_s * field_def = &PORT_MAP[port_id & 0x0f];
    struct port_s * port = &d->ports[port_id & 0x0f];
    struct jsdrv_stream_signal_s * s;
    struct jsdrvp_msg_s * m = port->msg_in;
    if (!field_def->data_topic || !field_def->data_topic[0]) {
        return;
    }

    // header is u32 sample_id, consume and skip to payload
    // sample_id is always for 2 Msps, regardless of this port's sample rate
    uint32_t sample_id_u32 = *p_u32++;
    uint64_t sample_id_u64 = (port->sample_id_next & 0xffffffff00000000LLU) | sample_id_u32;
    size -= sizeof(uint32_t);

    if (port->sample_id_next == 0) {
        port->sample_id_next = sample_id_u64;  // todo correctly initialize u64
    }

    switch (port_id) {
        case PORT_ID_CURRENT:
            sbuf_f32_add(&d->i_buf, port->sample_id_next, (float *) p_u32, size / sizeof(float));
            break;
        case PORT_ID_VOLTAGE:
            sbuf_f32_add(&d->v_buf, port->sample_id_next, (float *) p_u32, size / sizeof(float));
            break;
        default:
            break;
    }

    // todo improve robustness around rollover events.
    uint32_t sample_count = (size << 3) / field_def->element_size_bits;
    if (sample_id_u64 == port->sample_id_next) {
        // normal behavior
    } else if ((sample_id_u64 + sample_count) <= port->sample_id_next) {
        JSDRV_LOGI("stream_in_port %d sample_id dup: received=%" PRIu64 " expected=%" PRIu64,
                   port_id, sample_id_u64, port->sample_id_next);
    } else {
        if (m) {
            JSDRV_LOGI("stream_in_port %d sample_id skip: received=%" PRIu64 " expected=%" PRIu64,
                       port_id, sample_id_u64, port->sample_id_next);
            jsdrvp_backend_send(d->context, m);
            m = NULL;
        }
        port->sample_id_next = sample_id_u64;
    }

    if (m && ((m->value.size + size) >= sizeof(struct jsdrv_stream_signal_s))) {
        JSDRV_LOGD1("stream_in_port: port_id=%d", (int) port_id);
        port->msg_in = NULL;
        jsdrvp_backend_send(d->context, m);
        m = NULL;
    }

    if (m) {
        s = (struct jsdrv_stream_signal_s *) m->value.value.bin;
    } else {
        m = jsdrvp_msg_alloc_data(d->context, "");
        tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s", d->ll.prefix, field_def->data_topic);
        s = (struct jsdrv_stream_signal_s *) m->value.value.bin;
        s->sample_id = port->sample_id_next;
        s->sample_rate = SAMPLING_FREQUENCY;
        if (field_def->element_type == JSDRV_DATA_TYPE_FLOAT) {
            s->decimate_factor = port->decimate_factor;
        } else {
            s->decimate_factor = 1;
        }
        s->index = field_def->index;
        s->field_id = field_def->field_id;
        s->element_type = field_def->element_type;
        s->element_size_bits = field_def->element_size_bits;
        s->element_count = 0;
        m->value.app = JSDRV_PAYLOAD_TYPE_STREAM;
        m->value.size = JSDRV_STREAM_HEADER_SIZE;
        port->msg_in = m;
    }

    // Add decompression here as needed - compression not yet implemented on sensor

    uint8_t * p = (uint8_t *) &m->value.value.bin[m->value.size];
    JSDRV_ASSERT((m->value.size + size) <= sizeof(struct jsdrv_stream_signal_s));

    if ((port->downsample != NULL) && (s->element_type == JSDRV_DATA_TYPE_FLOAT)) {
        float * x = (float *) p_u32;
        float * y = (float *) p;
        for (uint32_t idx = 0; idx < sample_count; ++idx) {
            if (jsdrv_downsample_add_f32(port->downsample, sample_id_u64 >> 1, x[idx], y)) {
                ++y;
                if (s->element_count == 0) {
                    s->sample_id = sample_id_u64;
                }
                ++s->element_count;
                m->value.size += sizeof(float);
            }
            sample_id_u64 += 2;
        }
        port->sample_id_next = sample_id_u64;
    } else {
        m->value.size += size;
        memcpy(p, p_u32, size);
        s->element_count += sample_count;
        port->sample_id_next += sample_count;
    }

    // determine if need to send
    uint64_t sample_id_delta = port->sample_id_next - s->sample_id;
    uint32_t element_count_max = SAMPLING_FREQUENCY / (20 * port->decimate_factor);
    if ((((s->element_count * s->element_size_bits) / 8) >= STREAM_PAYLOAD_FULL)
            || (s->element_count >= element_count_max)) {
        JSDRV_LOGD1("stream_in_port: port_id=%d, sample_id_delta=%d, size=%d", (int) port_id, (int) sample_id_delta, (int) m->value.size);
        port->msg_in = NULL;
        jsdrvp_backend_send(d->context, m);
    }
}

static void compute_power(struct dev_s * d) {
    // for full-rate data, must compute power on the host
    // insufficient sensor-controller and USB bandwidth to stream everything.
    sbuf_f32_mult(&d->p_buf, &d->i_buf, &d->v_buf);
    uint32_t sz = sbuf_f32_length(&d->p_buf);
    if (sz) {
        handle_stream_in_port(d, PORT_ID_POWER, &d->p_buf.msg_sample_id, (1 + sz) * 4);
    }
}

static void handle_uart_in(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    (void) d;
    (void) p_u32;
    (void) size;
    // Not yet generated by the JS220 instrument.
    // When available, add UART message parsing support here.
}

static void handle_statistics_in(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    if (size != sizeof(struct js220_statistics_raw_s)) {
        JSDRV_LOGW("statistics size mismatch");
        return;
    }
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/s/stats/value", d->ll.prefix);
    struct jsdrv_statistics_s * dst = (struct jsdrv_statistics_s *) m->payload.bin;
    m->value = jsdrv_union_cbin_r((uint8_t *) dst, sizeof(*dst));
    m->value.app = JSDRV_PAYLOAD_TYPE_STATISTICS;
    struct js220_statistics_raw_s * src = (struct js220_statistics_raw_s *) p_u32;
    if (0 == js220_stats_convert(src, dst)) {
        jsdrvp_backend_send(d->context, m);
    }
}

static void handle_stream_in_port0(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    (void) d;
    (void) size;
    struct js220_port0_header_s * hdr = (struct js220_port0_header_s *) p_u32;
    union js220_port0_payload_u * p = (union js220_port0_payload_u *) &hdr[1];

    switch (hdr->op) {
        case JS220_PORT0_OP_CONNECT: {
            char prot_ver_str[JSDRV_VERSION_STR_LENGTH_MAX];
            char fw_ver_str[JSDRV_VERSION_STR_LENGTH_MAX];
            char hw_ver_str[JSDRV_VERSION_STR_LENGTH_MAX];
            char fpga_ver_str[JSDRV_VERSION_STR_LENGTH_MAX];

            struct js220_port0_connect_s * c = &p->connect;

            JSDRV_LOGI("port0 connect rsp");
            jsdrv_version_u32_to_str(c->protocol_version, prot_ver_str, sizeof(prot_ver_str));
            if (JSDRV_VERSION_DECODE_U32_MAJOR(c->protocol_version) != JS220_PROTOCOL_VERSION_MAJOR) {
                JSDRV_LOGE("Protocol version mismatch: local=%s, remote=%s",
                         JS220_PROTOCOL_VERSION_STR, prot_ver_str);
                break;
            }

            size_t sz_expect = sizeof(struct js220_port0_header_s) + sizeof(struct js220_port0_connect_s);
            if (size < sz_expect) {
                JSDRV_LOGW("connect message size mismatch: %zu < %zu", size, sz_expect);
                break;
            }

            jsdrv_version_u32_to_str(c->fw_version, fw_ver_str, sizeof(fw_ver_str));
            jsdrv_version_u32_to_str(c->hw_version, hw_ver_str, sizeof(hw_ver_str));
            jsdrv_version_u32_to_str(c->fpga_version, fpga_ver_str, sizeof(fpga_ver_str));
            JSDRV_LOGI("JS220 app_id=%d, FW=%s, HW=%s, FPGA=%s, protocol=%s",
                     c->app_id, fw_ver_str, hw_ver_str, fpga_ver_str, prot_ver_str);
            send_to_frontend(d, "c/fw/version$", &jsdrv_union_cjson_r(fw_ver_meta));
            send_to_frontend(d, "c/hw/version$", &jsdrv_union_cjson_r(hw_ver_meta));
            send_to_frontend(d, "h/fs$", &jsdrv_union_cjson_r(sampling_frequency_meta));
            send_to_frontend(d, "h/!reset$", &jsdrv_union_cjson_r(reset_meta));
            send_to_frontend(d, "c/fw/version", &jsdrv_union_u32_r(c->fw_version));
            send_to_frontend(d, "c/hw/version", &jsdrv_union_u32_r(c->hw_version));
            send_to_frontend(d, "s/fpga/version", &jsdrv_union_u32_r(c->fpga_version));

            if (d->ll_await_break_on == BREAK_CONNECT) {
                d->ll_await_break_on = BREAK_NONE;
                d->ll_await_break = true;
            }
            break;
        }

        case JS220_PORT0_OP_ECHO:
            JSDRV_LOGD3("port 0 echo rsp");
            break;

        case JS220_PORT0_OP_TIMESYNC: {
            JSDRV_LOGD3("port 0 timesync req");
            uint16_t length = sizeof(struct js220_port0_header_s) + sizeof(struct js220_port0_timesync_s);
            struct jsdrvp_msg_s * m = bulk_out_factory(d, 0, length);
            struct js220_port0_msg_s * p0 = (struct js220_port0_msg_s *) m->payload.bin;
            p0->port0_hdr.op = JS220_PORT0_OP_TIMESYNC;
            p0->port0_hdr.status = 0;
            p0->port0_hdr.arg = 0;
            p0->payload.timesync.rsv_i64 = p->timesync.rsv_i64;
            p0->payload.timesync.start_count = p->timesync.start_count;
            p0->payload.timesync.utc_recv = jsdrv_time_utc();
            p0->payload.timesync.utc_send = p0->payload.timesync.utc_recv;
            p0->payload.timesync.end_count = 0;
            msg_queue_push(d->ll.cmd_q, m);
            break;
        }

        default:
            JSDRV_LOGW("Unsupported port0 op: %d", (int) hdr->op);
            break;
    }
}

static void handle_stream_in_pubsub(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    struct js220_publish_s * p = (struct js220_publish_s *) p_u32;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(d->context);
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s", d->ll.prefix, p->topic);
    size_t sz = strlen(m->topic);
    if (m->topic[sz - 1] == '?') {
        m->topic[sz - 1] = 0;  // we asked for it, treat as normal.
    }
    m->value.type = p->type;
    m->value.flags = p->flags;
    m->value.op = p->op;
    m->value.app = p->app;
    m->value.size = size - sizeof(*p);

    if ((p->type == JSDRV_UNION_STR) || (p->type == JSDRV_UNION_JSON)) {
        if (m->value.size > sizeof(m->payload.bin)) {
            JSDRV_LOGE("pubsub from js220 %s STR, but size too big %u", m->topic, m->value.size);
            jsdrvp_msg_free(d->context, m);
            return;
        } else {
            jsdrv_memcpy(m->payload.str, p->data, m->value.size);
            m->payload.str[m->value.size - 1] = 0; // force null term
            m->value.value.str = m->payload.str;
        }
    } else if (p->type == JSDRV_UNION_BIN) {
        if (m->value.size > sizeof(m->payload.bin)) {
            JSDRV_LOGE("pubsub from js220 %s BIN, but size too big %u", m->topic, m->value.size);
            jsdrvp_msg_free(d->context, m);
            return;
        } else {
            jsdrv_memcpy(m->payload.bin, p->data, m->value.size);
            m->value.value.bin = m->payload.bin;
        }
    } else {
        jsdrv_memcpy(&m->value.value, p->data, sizeof(m->value.value));
    }
    char buf[32];
    jsdrv_union_value_to_str(&m->value, buf, (uint32_t) sizeof(buf), 1);
    JSDRV_LOGD1("publish from dev: %s %s", p->topic, buf);

    if ((d->ll_await_break_on == BREAK_PUBSUB_TOPIC) && (0 == strcmp(d->ll_await_break_topic, p->topic))) {
        d->ll_await_break_on = BREAK_NONE;
        d->ll_await_break = true;
        d->ll_await_break_value = m->value;
    }

    handle_rsp_ctrl(d, p->topic, &m->value);  // reconnect in streaming, may not be desirable
    jsdrvp_backend_send(d->context, m);
}

static void handle_stream_in_logging(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    (void) d;
    (void) p_u32;
    (void) size;
}

static void mem_write_end(struct dev_s * d) {
    struct jsdrvp_msg_s * msg_bk = bulk_out_factory(d, 3, sizeof(struct js220_port3_header_s));
    struct js220_port3_msg_s * m = (struct js220_port3_msg_s *) msg_bk->value.value.bin;
    memset(&m->hdr, 0, sizeof(m->hdr));
    m->hdr = d->mem_hdr;
    m->hdr.op = JS220_PORT3_OP_NONE;
    msg_queue_push(d->ll.cmd_q, msg_bk);
}

static void mem_write_next(struct dev_s * d) {
    for (int count = 0; ; ++count) {
        uint32_t remaining = d->mem_hdr.length - d->mem_offset_sent;
        if (0 == remaining) {
            if (count) {
                mem_write_end(d);
            }
            break;
        }
        if ((d->mem_offset_sent - d->mem_offset_valid) > (JS220_PORT3_BUFFER_SIZE + 512)) {
            // enough data outstanding
            break;
        }
        struct jsdrvp_msg_s * msg_bk = bulk_out_factory(d, 3, JS220_PAYLOAD_SIZE_MAX);
        struct js220_port3_msg_s * m = (struct js220_port3_msg_s *) msg_bk->value.value.bin;
        m->hdr = d->mem_hdr;
        m->hdr.op = JS220_PORT3_OP_WRITE_DATA;
        m->hdr.offset = d->mem_offset_sent;
        m->hdr.length = (remaining > JS220_PORT3_DATA_SIZE_MAX) ? JS220_PORT3_DATA_SIZE_MAX : remaining;
        memcpy(m->data, d->mem_data + m->hdr.offset, m->hdr.length);
        JSDRV_LOGD1("mem_write_data offset=%d, length=%d", (int) d->mem_offset_sent, (int) m->hdr.length);
        d->mem_offset_sent += m->hdr.length;
        msg_queue_push(d->ll.cmd_q, msg_bk);
    }
}

static void mem_write_finalize(struct dev_s * d) {
    JSDRV_LOGI("mem_write_finalize");
    struct jsdrvp_msg_s * msg_bk = bulk_out_factory(d, 3, sizeof(struct js220_port3_header_s));
    struct js220_port3_msg_s * m = (struct js220_port3_msg_s *) msg_bk->value.value.bin;
    d->mem_hdr.op = JS220_PORT3_OP_WRITE_FINALIZE;
    m->hdr = d->mem_hdr;
    msg_queue_push(d->ll.cmd_q, msg_bk);
}

static void mem_status(struct dev_s * d, uint8_t status) {
    if (0 == d->mem_hdr.status) {
        d->mem_hdr.status = status;
    }
}

static void mem_handle_read_data(struct dev_s * d, struct js220_port3_msg_s * msg) {
    if (msg->hdr.offset != d->mem_offset_valid) {
        JSDRV_LOGW("read_data expected offset %d, received %d", (int) d->mem_offset_valid, (int) msg->hdr.offset);
        mem_status(d, JSDRV_ERROR_SEQUENCE);
    } else if (msg->hdr.length > JS220_PORT3_DATA_SIZE_MAX) {
        JSDRV_LOGW("read_data length to long: %d", (int) msg->hdr.length);
        mem_status(d, JSDRV_ERROR_PARAMETER_INVALID);
    } else {
        JSDRV_LOGD1("mem_read_data offset=%d, sz=%d", (int) d->mem_offset_valid, (int) msg->hdr.length);
        uint32_t sz_remaining = d->mem_hdr.length - d->mem_offset_valid;
        uint32_t sz = msg->hdr.length;
        if (sz > sz_remaining) {
            sz = sz_remaining;
        }
        if (sz) {
            memcpy(d->mem_data + d->mem_offset_valid, msg->data, sz);
            d->mem_offset_valid += sz;
        } else {
            JSDRV_LOGW("mem_read_data ignore extra data: offset=%d, sz=%d", (int) d->mem_offset_valid, (int) msg->hdr.length);
        }
    }
}

static void handle_stream_in_mem(struct dev_s * d, uint32_t * p_u32, uint16_t size) {
    size += sizeof(union js220_frame_hdr_u);  // excluded over USB
    size_t hdr_size = sizeof(union js220_frame_hdr_u) + sizeof(struct js220_port3_header_s);
    if (size < hdr_size) {
        JSDRV_LOGE("invalid in mem frame, too small");
        return;
    }
    struct js220_port3_msg_s * msg = (struct js220_port3_msg_s *) p_u32;

    if ((msg->hdr.op == JS220_PORT3_OP_ACK) && (d->mem_hdr.op == msg->hdr.arg)) {
        JSDRV_LOGD1("in_mem ack=%d, op=%d, status=%d",
                    (int) msg->hdr.op, (int) msg->hdr.arg, (int) msg->hdr.status);
        uint8_t status = d->mem_hdr.status;
        if (0 == status) {
            status = msg->hdr.status;
        }

        switch (msg->hdr.arg) {
            case JS220_PORT3_OP_ERASE: mem_complete(d, status); break;
            case JS220_PORT3_OP_WRITE_START:
                if (status) {
                    mem_complete(d, status);
                } else {
                    d->mem_hdr.op = JS220_PORT3_OP_WRITE_DATA;
                    mem_write_next(d);
                }
                break;
            case JS220_PORT3_OP_WRITE_DATA:
                if (status) {
                    mem_complete(d, status);
                } else if (msg->hdr.offset <= d->mem_offset_valid) {
                    JSDRV_LOGW("ack write data already acked: %d %d", (int) msg->hdr.offset, (int) d->mem_offset_valid);
                    mem_complete(d, JSDRV_ERROR_SEQUENCE);
                } else if (msg->hdr.offset > d->mem_offset_sent) {
                    JSDRV_LOGW("ack write data not yet sent: %d %d", (int) msg->hdr.offset, (int) d->mem_offset_sent);
                    mem_complete(d, JSDRV_ERROR_SEQUENCE);
                } else {
                    JSDRV_LOGD1("ack write data %lu of %lu", msg->hdr.offset, d->mem_hdr.length);
                    d->mem_offset_valid = msg->hdr.offset;
                    if (d->mem_offset_valid == d->mem_hdr.length) {
                        mem_write_finalize(d);
                    } else {
                        mem_write_next(d);
                    }
                }
                break;
            case JS220_PORT3_OP_WRITE_FINALIZE: mem_complete(d, status); break;
            case JS220_PORT3_OP_READ_REQ:
                d->mem_hdr.length = d->mem_offset_valid;  // truncate as needed
                mem_complete(d, status);
                break;
            default:
                JSDRV_LOGW("unsupported ack: %d", (int) msg->hdr.arg);
                break;
        }
    } else if ((msg->hdr.op == JS220_PORT3_OP_READ_DATA) && (d->mem_hdr.op == JS220_PORT3_OP_READ_REQ)) {
        mem_handle_read_data(d, msg);
    } else {
        JSDRV_LOGW("mem in op %d, received %d, arg=%d", d->mem_hdr.op, msg->hdr.op, msg->hdr.arg);
        mem_complete(d, JSDRV_ERROR_ABORTED);
    }

}

static void handle_stream_in_frame(struct dev_s * d, uint32_t * p_u32) {
    union js220_frame_hdr_u hdr;
    hdr.u32 = p_u32[0];
    if (d->in_frame_id != (uint16_t) hdr.h.frame_id) {
        JSDRV_LOGW("in frame_id mismatch %d != %d", (int) d->in_frame_id, (int) hdr.h.frame_id);
        // todo keep statistics
        d->in_frame_id = hdr.h.frame_id;
    }
    if ((d->stream_in_port_enable & (1U << hdr.h.port_id)) == 0U) {
        JSDRV_LOGW("stream in ignore on inactive port %d", hdr.h.port_id);
    } else if (hdr.h.port_id >= 16U) {
        if (hdr.h.port_id == (16U + 13U)) {
            handle_uart_in(d, p_u32 + 1, hdr.h.length);
        } else if (hdr.h.port_id == (16U + 14U)) {
            handle_statistics_in(d, p_u32 + 1, hdr.h.length);
        } else {
            handle_stream_in_port(d, hdr.h.port_id, p_u32 + 1, hdr.h.length);
            if ((hdr.h.port_id == PORT_ID_VOLTAGE) &&
                       ((d->stream_in_port_enable & COMPUTE_POWER_MASK) == COMPUTE_POWER_MASK)) {
                compute_power(d);
            }
        }
    } else {
        JSDRV_LOGD1("stream in: port=%d, length=%d", hdr.h.port_id, hdr.h.length);
        switch ((uint8_t) hdr.h.port_id) {
            case 0: handle_stream_in_port0(d, p_u32 + 1, hdr.h.length); break;
            case 1: handle_stream_in_pubsub(d, p_u32 + 1, hdr.h.length); break;
            case 2: handle_stream_in_logging(d, p_u32 + 1, hdr.h.length); break;
            case 3: handle_stream_in_mem(d, p_u32, hdr.h.length); break;
            default: break; // unsupported, discard
        }
    }
    ++d->in_frame_id;
}

static void handle_stream_in(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    uint32_t frame_count = (msg->value.size + FRAME_SIZE_BYTES - 1) / FRAME_SIZE_BYTES;
    for (uint32_t i = 0; i < frame_count; ++i) {
        uint32_t * p_u32 = (uint32_t *) &msg->value.value.bin[i * FRAME_SIZE_BYTES];
        handle_stream_in_frame(d, p_u32);
    }
}

static bool handle_rsp(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    bool rv = true;
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
    } else if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
        }
    } else {
        JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
    }
    jsdrvp_msg_free(d->context, msg);
    return rv;
}

static THREAD_RETURN_TYPE driver_thread(THREAD_ARG_TYPE lpParam) {
    struct jsdrvp_msg_s * msg;
    struct dev_s *d = (struct dev_s *) lpParam;
    JSDRV_LOGI("JS220 USB upper-level thread started for %s", d->ll.prefix);

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

    // publish metadata for our host-side parameters
    for (const struct jsdrvp_param_s * p = js220_params; p->topic; ++p) {
        msg = jsdrvp_msg_alloc_value(d->context, "", &jsdrv_union_json(p->meta));
        tfp_snprintf(msg->topic, sizeof(msg->topic), "%s/%s$", d->ll.prefix, p->topic);
        jsdrvp_backend_send(d->context, msg);
    }

    update_state(d, ST_CLOSED);

    while (!d->do_exit) {
#if _WIN32
        WaitForMultipleObjects(handle_count, handles, false, 5000);
#else
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
    }

    JSDRV_LOGI("JS220 USB upper-level thread done %s", d->ll.prefix);
    THREAD_RETURN();
}

static void join(struct jsdrvp_ul_device_s * device) {
    struct dev_s * d = (struct dev_s *) device;
    jsdrvp_send_finalize_msg(d->context, d->ul.cmd_q, "");
    // and wait for thread to exit.
    jsdrv_thread_join(&d->thread, 1000);

    for (uint32_t idx = 0; idx < JSDRV_ARRAY_SIZE(d->ports); ++idx) {
        struct port_s *p = &d->ports[idx];
        if (NULL != p->msg_in) {
            jsdrvp_msg_free(d->context, p->msg_in);
            p->msg_in = NULL;
        }
        if (NULL != p->downsample) {
            jsdrv_downsample_free(p->downsample);
            p->downsample = NULL;
        }
    }
    jsdrv_free(d);
}

int32_t jsdrvp_ul_js220_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll) {
    JSDRV_DBC_NOT_NULL(device);
    JSDRV_DBC_NOT_NULL(context);
    JSDRV_DBC_NOT_NULL(ll);
    *device = NULL;
    struct dev_s * d = jsdrv_alloc_clr(sizeof(struct dev_s));
    JSDRV_LOGD3("jsdrvp_ul_js220_usb_factory %p", d);
    on_sampling_frequency(d, &jsdrv_union_u32(SAMPLING_FREQUENCY));
    d->context = context;
    d->ll = *ll;
    d->ul.cmd_q = msg_queue_init();
    d->ul.join = join;
    if (jsdrv_thread_create(&d->thread, driver_thread, d)) {
        return JSDRV_ERROR_UNSPECIFIED;
    }
#if _WIN32
    if (!SetThreadPriority(d->thread.thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }
#endif
    *device = &d->ul;
    return 0;
}
