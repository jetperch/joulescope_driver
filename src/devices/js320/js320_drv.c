/*
 * Copyright 2025 Jetperch LLC
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
#include "jsdrv/cstr.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/devices/js320/js320_fwup.h"
#include "jsdrv_prv/devices/js320/js320_jtag.h"
#include "jsdrv_prv/devices/js320/js320_stats.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/sample_buffer_f32.h"
#include "mb/comm/link.h"
#include <inttypes.h>
#include <string.h>


#define JS320_PUB_RATE_DEFAULT (20U)         // Hz
#define JS320_FS_DEFAULT       (1000000U)    // host-tracked rate, Hz
#define JS320_CH_COUNT         (16U)
#define JS320_NATIVE_RATE      (16000000U)   // device pre-decimation rate, Hz
#define JS320_DECIMATE         (16U)         // device decimation factor
// Headroom keeps room for one more incoming MB frame's worth of bytes.
#define JS320_STREAM_PAYLOAD_FULL (JSDRV_STREAM_DATA_SIZE - JSDRV_STREAM_HEADER_SIZE - 1024U)

// Default values for the device-side decimation knobs.  Must match the
// firmware/gateware defaults in js320/firmware/fpga_mcu/src/app.c and
// js320/gateware/source/regfile.v so that the host's runtime decimation
// estimate matches the actual device output rate immediately after
// open, before any user reconfiguration arrives.
#define JS320_DEFAULT_SIGNAL_DWN_N (0U)      // 0 = passthrough (no extra i/v/p decimation)
#define JS320_DEFAULT_GPI_DWN_MODE (2U)      // 0=off, 1=toggle, 2=first, 3=majority
#define JS320_DEFAULT_GPI_DWN_N    (16U)     // gateware default factor

// Post-JS320_DECIMATE signal rate (16 MHz / 16 = 1 MHz).  The s/dwnN/N
// register divides this further; h/fs unification computes N = this / fs.
#define JS320_SIGNAL_RATE_AFTER_DECIMATE (1000000U)

// Drop-until-ack timeout.  Host drops sample frames after a s/dwnN/N
// change until the firmware's s/dwnN/!ack arrives.  If the ack never
// arrives, after this timeout the host resumes passing samples and logs
// a warning.
#define JS320_DWNN_ACK_TIMEOUT (2 * JSDRV_TIME_SECOND)

enum js320_group_e {
    JS320_GROUP_NONE = 0,
    JS320_GROUP_IVP  = 1,   // channels 5, 6, 7  (current, voltage, power)
    JS320_GROUP_GPIT = 2,   // channels 8..12    (gpi 0..3, trigger T)
};

// Per-channel static descriptor.  Used both for the buffered append path
// (channels 5..12) and the one-shot send path (channels 0, 1, 2, 13).
// data_topic == NULL means the channel is not handled by the table-driven
// dispatch (statistics is special-cased; ch 2 range supplies its topic at
// the call site because it depends on `source`; unused channels also use NULL).
struct js320_port_def_s {
    const char * data_topic;
    const char * ctrl_topic;
    uint8_t  field_id;        // JSDRV_FIELD_*
    uint8_t  index;           // base index; ADC composes runtime index = base | source
    uint8_t  element_type;    // JSDRV_DATA_TYPE_*
    uint8_t  element_size_bits;
    uint8_t  group;           // js320_group_e
    uint8_t  samples_per_u32; // payload samples per device u32 word
    uint32_t sample_rate;     // Hz, 0 for UART
    uint32_t decimate_factor;
};

struct js320_port_s {
    struct jsdrvp_msg_s * msg_in;     // in-progress accumulator (NULL if empty)
    uint64_t sample_id_next;          // next expected sample_id
    uint8_t  current_index;           // index assigned to msg_in (ADC/range may vary)
    bool     enabled;                 // last-seen s/X/ctrl from frontend (used in step 4)
};

static const struct js320_port_def_s PORT_DEFS[JS320_CH_COUNT] = {
    [0]  = { "s/adc/0/!data", "s/adc/0/ctrl", JSDRV_FIELD_RAW,     0x00, JSDRV_DATA_TYPE_INT,   32, JS320_GROUP_NONE, 1, JS320_NATIVE_RATE, JS320_DECIMATE },
    [1]  = { "s/adc/1/!data", "s/adc/1/ctrl", JSDRV_FIELD_RAW,     0x10, JSDRV_DATA_TYPE_INT,   32, JS320_GROUP_NONE, 1, JS320_NATIVE_RATE, JS320_DECIMATE },
    [2]  = { "s/i/range/!data", "s/i/range/ctrl", JSDRV_FIELD_RANGE,   0,    JSDRV_DATA_TYPE_UINT,   4, JS320_GROUP_NONE, 8, JS320_NATIVE_RATE, JS320_DECIMATE },
    [5]  = { "s/i/!data",     "s/i/ctrl",     JSDRV_FIELD_CURRENT, 0,    JSDRV_DATA_TYPE_FLOAT, 32, JS320_GROUP_IVP,  1, JS320_NATIVE_RATE, JS320_DECIMATE },
    [6]  = { "s/v/!data",     "s/v/ctrl",     JSDRV_FIELD_VOLTAGE, 0,    JSDRV_DATA_TYPE_FLOAT, 32, JS320_GROUP_IVP,  1, JS320_NATIVE_RATE, JS320_DECIMATE },
    [7]  = { "s/p/!data",     "s/p/ctrl",     JSDRV_FIELD_POWER,   0,    JSDRV_DATA_TYPE_FLOAT, 32, JS320_GROUP_IVP,  1, JS320_NATIVE_RATE, JS320_DECIMATE },
    // GPI / trigger pass through the configurable gpi_decimate module
    // (decimateN_digital).  The PORT_DEFS decimate_factor is the firmware
    // default (mode=2 "first", N=16, output 1 MHz).  The runtime
    // decimation is recomputed from s/gpi/+/dwnN/{mode,N} via
    // js320_runtime_decimate() and applied at flush time and in the
    // emitted jsdrv_stream_signal_s.decimate_factor field.
    [8]  = { "s/gpi/0/!data", "s/gpi/0/ctrl", JSDRV_FIELD_GPI,     0,    JSDRV_DATA_TYPE_UINT,   1, JS320_GROUP_GPIT, 32, JS320_NATIVE_RATE, JS320_DECIMATE },
    [9]  = { "s/gpi/1/!data", "s/gpi/1/ctrl", JSDRV_FIELD_GPI,     1,    JSDRV_DATA_TYPE_UINT,   1, JS320_GROUP_GPIT, 32, JS320_NATIVE_RATE, JS320_DECIMATE },
    [10] = { "s/gpi/2/!data", "s/gpi/2/ctrl", JSDRV_FIELD_GPI,     2,    JSDRV_DATA_TYPE_UINT,   1, JS320_GROUP_GPIT, 32, JS320_NATIVE_RATE, JS320_DECIMATE },
    [11] = { "s/gpi/3/!data", "s/gpi/3/ctrl", JSDRV_FIELD_GPI,     3,    JSDRV_DATA_TYPE_UINT,   1, JS320_GROUP_GPIT, 32, JS320_NATIVE_RATE, JS320_DECIMATE },
    [12] = { "s/gpi/7/!data", "s/gpi/7/ctrl", JSDRV_FIELD_GPI,     7,    JSDRV_DATA_TYPE_UINT,   1, JS320_GROUP_GPIT, 32, JS320_NATIVE_RATE, JS320_DECIMATE },
    [13] = { "s/uart/!data",  NULL,           JSDRV_FIELD_UART,    0,    JSDRV_DATA_TYPE_UINT,   8, JS320_GROUP_NONE, 4, 0,                 1 },
    // ch 3, 4, 14, 15 unused (data_topic NULL); 14 (statistics) is dispatched separately.
};

static const char * js320_publish_rate_meta = "{"
    "\"dtype\": \"u32\","
    "\"brief\": \"The approximate sample publish frequency.\","
    "\"default\": 20,"
    "\"options\": ["
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

// Sentinel for last_sent_ctrl entries that have never been forwarded.
// Picked outside the [0, 1] range so the first reconcile always sends.
#define JS320_CTRL_UNKNOWN (0xFFU)

// Closed-loop drop-until-ack bookkeeping for a single dwnN family
// (signal or gpi).  Each s/.../dwnN/... change increments
// `acks_outstanding` and sets `dropping`; the firmware brackets the
// register update with streaming disable/enable and publishes
// s/.../dwnN/!ack carrying the sample_id of the first sample at the new
// rate.  That ack decrements the counter and sets
// `drop_until_sample_id`.  While `dropping` is true, the data path
// drops incoming app frames for the affected channels whose sample_id
// is below the target (or drops unconditionally while acks remain
// outstanding).  `drop_timeout_utc` is the deadline after which the
// host resumes with a warning if the ack never arrives.
struct js320_ack_state_s {
    uint32_t acks_outstanding;
    bool     dropping;
    uint64_t drop_until_sample_id;
    int64_t  drop_timeout_utc;
};

struct js320_drv_s {
    struct jsdrvp_mb_drv_s drv;  // MUST BE FIRST
    const struct mb_link_identity_s * identity;
    struct jsdrvp_mb_dev_s * dev;
    struct js320_jtag_s * jtag;
    struct js320_fwup_s * fwup;
    double i_scale;
    double v_scale;
    uint32_t fs;            // host-tracked sample rate, Hz
    uint32_t publish_rate;  // h/fp, Hz
    struct js320_port_s ports[JS320_CH_COUNT];

    // Tracked device-side decimation knobs.  These are intercepted from
    // the frontend in handle_cmd, stored, then forwarded to the device
    // (mb_device.c default forwarding) so the gateware sees the new
    // values.  The host uses them to compute the runtime effective
    // decimation factor for each port.
    uint32_t signal_dwn_n;  // s/dwnN/N: extra i/v/p decimation atop the static 1 MHz output (0 or 1 = passthrough)
    uint32_t gpi_dwn_mode;  // s/gpi/+/dwnN/mode: 0=off, 1=toggle, 2=first, 3=majority
    uint32_t gpi_dwn_n;     // s/gpi/+/dwnN/N: GPI decimation factor (mode!=0)

    // Smart-power compute state.  When all three of i/v/p are enabled
    // at fs >= 1 MSPS, the device is told to stop streaming p (`s/p/ctrl=0`)
    // and the host multiplies i*v on each i/v frame arrival.  At lower
    // rates or when fewer than three are enabled, the device streams
    // power directly.
    struct sbuf_f32_s i_buf;
    struct sbuf_f32_s v_buf;
    struct sbuf_f32_s p_buf;
    bool     power_compute_on_host;
    uint8_t  last_sent_ctrl[JS320_CH_COUNT];  // last enable forwarded to device

    // Closed-loop drop-until-ack state, one per dwnN family.  `signal`
    // scopes i/v/p (ch 5-7), `gpi` scopes GPI + trigger (ch 8-12).
    struct js320_ack_state_s signal_ack;
    struct js320_ack_state_s gpi_ack;

    // Deferred-state-fetch interlock.  After ST_OPEN is entered, the
    // JS320 ctrl firmware has only just told its sensor task to boot
    // (the sensor side powers up in response to the USBD link-connect
    // event).  state_fetch would hit BUSY until the sensor pubsub task
    // is alive, so open_ready returns false and we call
    // jsdrvp_mb_dev_state_fetch_start() from handle_publish when the
    // first `s/...` publish proves the sensor task is running, or from
    // on_timeout as a 2 s fallback.
    bool     waiting_for_sensor;
};

// How long to wait for the sensor task to come up before falling back
// to state_fetch anyway.  The JS320 ctrl firmware powers the sensor on
// USBD connect; typical boot time is < 1 s.
#define JS320_SENSOR_READY_TIMEOUT_NS (2 * JSDRV_TIME_SECOND)

// --- Streaming signal helpers ---

// Map the s/dwnN/N register value to the actual extra decimation factor
// applied by gateware/source/decimate.v on top of the 1 MHz signal rate.
//   N=0 or 1: passthrough (factor 1)
//   N=2 or 3: factor 2 (gateware clamps 3 to 2)
//   N=4..1000: factor N
static inline uint32_t js320_signal_extra_factor(uint32_t n) {
    if (n <= 1U) {
        return 1U;
    }
    if (n == 3U) {
        return 2U;
    }
    return n;
}

// Map (mode, N) for the gateware/source/decimateN_digital.v gpi_decimate
// instance to the actual decimation factor applied to the 16 MHz native
// GPI sample stream.
//   mode=0 OR N<=1: passthrough (factor 1, output @ 16 MHz)
//   else: factor N
static inline uint32_t js320_gpi_factor(uint32_t mode, uint32_t n) {
    if ((mode == 0U) || (n <= 1U)) {
        return 1U;
    }
    return n;
}

// Compute the runtime decimation factor for a given channel: the number
// of native 16 MHz sample_id ticks per output sample, accounting for any
// dynamic dwnN/N or gpi/+/dwnN/{mode,N} the user has set.
static uint32_t js320_runtime_decimate(struct js320_drv_s * self, uint8_t ch) {
    const struct js320_port_def_s * def = &PORT_DEFS[ch];
    if ((def->field_id == JSDRV_FIELD_CURRENT)
            || (def->field_id == JSDRV_FIELD_VOLTAGE)
            || (def->field_id == JSDRV_FIELD_POWER)) {
        // i/v/p: 16 (static, 16 MHz -> 1 MHz) * extra dwnN factor
        return JS320_DECIMATE * js320_signal_extra_factor(self->signal_dwn_n);
    }
    if (def->field_id == JSDRV_FIELD_GPI) {
        // GPI: gpi_decimate runs at 16 MHz native; effective factor = N (or 1 if mode=0)
        return js320_gpi_factor(self->gpi_dwn_mode, self->gpi_dwn_n);
    }
    return def->decimate_factor ? def->decimate_factor : 1U;
}

// Initialize the in-memory header of a fresh stream signal message.
// `decimate_runtime` is the actual sample_id ticks per output sample at
// the moment of allocation (may differ from def->decimate_factor when
// the user has reconfigured s/dwnN/N or s/gpi/+/dwnN/{mode,N}).
static void js320_signal_init(struct jsdrv_stream_signal_s * signal,
                              const struct js320_port_def_s * def,
                              uint8_t index,
                              uint64_t sample_id,
                              uint32_t decimate_runtime) {
    signal->version = 1;
    signal->rsv1_u8 = 0;
    signal->rsv2_u8 = 0;
    signal->rsv3_u8 = 0;
    signal->rsv4_u32 = 0;
    signal->sample_id = sample_id;
    signal->field_id = def->field_id;
    signal->index = index;
    signal->element_type = def->element_type;
    signal->element_size_bits = def->element_size_bits;
    signal->element_count = 0;
    signal->sample_rate = def->sample_rate;
    signal->decimate_factor = decimate_runtime;
}

// Allocate a fresh stream-data message and initialize header + topic + time map.
// Caller appends sample bytes after `m->value.value.bin[m->value.size]` and
// bumps `m->value.size` and `signal->element_count`.
static struct jsdrvp_msg_s * js320_stream_msg_alloc(struct jsdrvp_mb_dev_s * dev,
                                                    const struct js320_port_def_s * def,
                                                    const char * data_topic_override,
                                                    uint8_t index,
                                                    uint64_t sample_id,
                                                    uint32_t decimate_runtime) {
    struct jsdrv_context_s * context = jsdrvp_mb_dev_context(dev);
    const char * prefix = jsdrvp_mb_dev_prefix(dev);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_data(context, "");

    m->value.type = JSDRV_UNION_BIN;
    m->value.app = JSDRV_PAYLOAD_TYPE_STREAM;
    m->value.value.bin = m->payload.bin;
    m->value.size = JSDRV_STREAM_HEADER_SIZE;

    struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) m->payload.bin;
    js320_signal_init(signal, def, index, sample_id, decimate_runtime);

    // The sensor's timesync map is authoritative for sample timestamping.
    const struct jsdrv_time_map_s * mtmap = jsdrvp_mb_dev_time_map(dev, 's');
    if (!mtmap) {
        // no time map yet, so no streaming data
        jsdrvp_msg_free(context, m);
        return NULL;
    }
    signal->time_map = *mtmap;

    const char * dt = data_topic_override ? data_topic_override : def->data_topic;
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, prefix);
    jsdrv_topic_append(&topic, dt);
    jsdrv_cstr_copy(m->topic, topic.topic, sizeof(m->topic));

    return m;
}

// Send the accumulated message and clear the slot.
static void js320_port_flush(struct js320_drv_s * self,
                             struct jsdrvp_mb_dev_s * dev,
                             uint8_t ch) {
    struct js320_port_s * port = &self->ports[ch];
    if (port->msg_in == NULL) {
        return;
    }
    struct jsdrvp_msg_s * m = port->msg_in;
    port->msg_in = NULL;
    jsdrvp_mb_dev_backend_send(dev, m);
}

// Flush every port that belongs to the given group and has pending samples.
// Used to align like-typed signals (i/v/p; gpi 0..3 + trigger T) so the
// frontend receives coherent multi-signal updates.
static void js320_group_flush(struct js320_drv_s * self,
                              struct jsdrvp_mb_dev_s * dev,
                              uint8_t group) {
    if (group == JS320_GROUP_NONE) {
        return;
    }
    for (uint8_t ch = 0; ch < JS320_CH_COUNT; ++ch) {
        if ((PORT_DEFS[ch].group == group) && (self->ports[ch].msg_in != NULL)) {
            js320_port_flush(self, dev, ch);
        }
    }
}

// Append `sample_count` samples to the per-port accumulator.  Allocates
// the accumulator on first use.  If a sample_id discontinuity is detected
// versus the previous accumulation, the partial message is flushed first
// and a fresh accumulator starts at the new sample_id.  Does NOT evaluate
// flush conditions — that is `js320_port_evaluate_flush`'s job.
static void js320_port_append(struct js320_drv_s * self,
                              struct jsdrvp_mb_dev_s * dev,
                              uint8_t ch,
                              uint8_t index,
                              const char * data_topic_override,
                              uint64_t sample_id,
                              const void * data,
                              uint32_t sample_count) {
    const struct js320_port_def_s * def = &PORT_DEFS[ch];
    struct js320_port_s * port = &self->ports[ch];

    // Discontinuity check: if the existing accumulator does not line up
    // with the new frame's sample_id (or the index changed for ADC/range),
    // send what we have and start fresh.
    if (port->msg_in != NULL) {
        if (sample_id != port->sample_id_next) {
            uint32_t decimate_runtime_dbg = js320_runtime_decimate(self, ch);
            JSDRV_LOGI("ch %u sample_id skip: expected=%" PRIu64 " received=%" PRIu64
                       " signal_dwn_n=%u gpi_dwn_mode=%u gpi_dwn_n=%u runtime_decimate=%u",
                       (unsigned) ch, port->sample_id_next, sample_id,
                       (unsigned) self->signal_dwn_n,
                       (unsigned) self->gpi_dwn_mode,
                       (unsigned) self->gpi_dwn_n,
                       (unsigned) decimate_runtime_dbg);
            js320_port_flush(self, dev, ch);
        } else if (index != port->current_index) {
            js320_port_flush(self, dev, ch);
        }
    }

    uint32_t decimate_runtime = js320_runtime_decimate(self, ch);
    if (decimate_runtime == 0U) {
        decimate_runtime = 1U;
    }

    if (port->msg_in == NULL) {
        port->msg_in = js320_stream_msg_alloc(dev, def, data_topic_override,
                                              index, sample_id, decimate_runtime);
        if (port->msg_in == NULL) {
            // Allocation deferred (e.g. sensor !map not yet received);
            // drop this frame — the next one will retry once the map
            // is populated.
            return;
        }
        port->sample_id_next = sample_id;
        port->current_index = index;
    }

    struct jsdrvp_msg_s * m = port->msg_in;
    struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) m->payload.bin;

    uint32_t bit_count = sample_count * (uint32_t) def->element_size_bits;
    uint32_t byte_count = (bit_count + 7U) / 8U;
    JSDRV_ASSERT((m->value.size + byte_count) <= sizeof(struct jsdrv_stream_signal_s));

    uint8_t * dst = (uint8_t *) &m->value.value.bin[m->value.size];
    memcpy(dst, data, byte_count);
    m->value.size += byte_count;
    signal->element_count += sample_count;
    // sample_id is at the device native rate; each output sample
    // represents `decimate_runtime` sample_id ticks.
    port->sample_id_next = sample_id + (uint64_t) sample_count * (uint64_t) decimate_runtime;
}

// Decide whether to flush the named port (and possibly its whole group)
// based on payload-full safety valve and rate-budget thresholds.  Async
// channels (sample_rate=0, i.e. UART) flush per frame.  Grouped channels
// trigger a whole-group flush on rate-budget; non-grouped channels flush
// individually.
static void js320_port_evaluate_flush(struct js320_drv_s * self,
                                      struct jsdrvp_mb_dev_s * dev,
                                      uint8_t ch) {
    const struct js320_port_def_s * def = &PORT_DEFS[ch];
    struct js320_port_s * port = &self->ports[ch];
    if (port->msg_in == NULL) {
        return;
    }

    // Async channel: no rate budget, flush every frame.
    if (def->sample_rate == 0U) {
        js320_port_flush(self, dev, ch);
        return;
    }

    struct jsdrv_stream_signal_s * signal =
        (struct jsdrv_stream_signal_s *) port->msg_in->payload.bin;

    // Payload-full safety valve (single port).
    uint32_t byte_count = ((uint32_t) signal->element_count
                           * (uint32_t) def->element_size_bits + 7U) / 8U;
    if (byte_count >= JS320_STREAM_PAYLOAD_FULL) {
        js320_port_flush(self, dev, ch);
        return;
    }

    // Rate-budget flush.  Use the runtime decimation so that user-driven
    // dwnN/N or gpi/+/dwnN/{mode,N} changes immediately retarget the
    // element_count_max threshold.  Whole-group flush for grouped
    // signals so current/voltage/power and gpi 0..3+T are delivered
    // coherently.
    uint32_t decimate_runtime = js320_runtime_decimate(self, ch);
    if (decimate_runtime == 0U) {
        decimate_runtime = 1U;
    }
    uint32_t effective_rate = def->sample_rate / decimate_runtime;
    uint32_t pub_rate = self->publish_rate ? self->publish_rate : 1U;
    uint32_t element_count_max = effective_rate / pub_rate;
    if (element_count_max < 1U) {
        element_count_max = 1U;
    }
    if (signal->element_count >= element_count_max) {
        if (def->group != JS320_GROUP_NONE) {
            js320_group_flush(self, dev, def->group);
        } else {
            js320_port_flush(self, dev, ch);
        }
    }
}

// --- Smart power computation ---

// Reset a single port's accumulator state.  Used when a stream ctrl
// flips, fs changes, or any other event invalidates in-flight samples.
static void js320_port_reset(struct js320_drv_s * self,
                             struct jsdrvp_mb_dev_s * dev,
                             uint8_t ch) {
    struct js320_port_s * port = &self->ports[ch];
    if (port->msg_in != NULL) {
        struct jsdrv_context_s * context = jsdrvp_mb_dev_context(dev);
        jsdrvp_msg_free(context, port->msg_in);
        port->msg_in = NULL;
    }
    port->sample_id_next = 0;
    port->current_index = 0;
}

// Initialize the host-side i/v/p compute buffers.  sbuf_f32_clear sets
// sample_id_decimate to 2 (a JS220 default); JS320 must override to its
// own native-rate / output-rate ratio so sbuf_f32_add and sbuf_f32_mult
// step the sample_id by the right amount per buffered sample.
static void js320_sbufs_clear(struct js320_drv_s * self) {
    sbuf_f32_clear(&self->i_buf);
    sbuf_f32_clear(&self->v_buf);
    sbuf_f32_clear(&self->p_buf);
    self->i_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
    self->v_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
    self->p_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
}

// Pure decision: given (fs, ports[5/6/7].enabled), compute the desired
// device-side enables for s/i, s/v, s/p and whether the host should
// multiply i*v locally.  Diff against the last-sent cache and forward
// only the differences.  Called from the s/X/ctrl handlers and the
// h/fs handler.
static void js320_reconcile_power(struct js320_drv_s * self,
                                  struct jsdrvp_mb_dev_s * dev) {
    bool i_en = self->ports[5].enabled;
    bool v_en = self->ports[6].enabled;
    bool p_en = self->ports[7].enabled;

    bool want_i_dev = i_en;
    bool want_v_dev = v_en;
    bool want_p_dev = p_en;
    bool host_compute = false;
    if ((self->fs >= JS320_FS_DEFAULT) && i_en && v_en && p_en) {
        // All three at full rate: stream i and v from device, multiply on host.
        want_p_dev = false;
        host_compute = true;
    }

    // If the host-compute flag is changing, drop any stale i/v/p buffers
    // so the next mult starts at the new sample_id watermark.
    if (host_compute != self->power_compute_on_host) {
        js320_sbufs_clear(self);
        self->power_compute_on_host = host_compute;
    }

    static const uint8_t IVP_CHANS[3] = {5U, 6U, 7U};
    const bool wants[3] = {want_i_dev, want_v_dev, want_p_dev};
    for (uint32_t k = 0U; k < 3U; ++k) {
        uint8_t ch = IVP_CHANS[k];
        uint8_t want_u8 = wants[k] ? 1U : 0U;
        if (self->last_sent_ctrl[ch] != want_u8) {
            self->last_sent_ctrl[ch] = want_u8;
            jsdrvp_mb_dev_publish_to_device(dev, PORT_DEFS[ch].ctrl_topic,
                &jsdrv_union_u32_r((uint32_t) want_u8));
        }
    }
}

// Multiply the overlapping i and v sample buffers and inject the result
// into the ch 7 (power) accumulator pipeline.  No-op when the host is
// not in compute mode or when there is no overlap to consume.
static void js320_compute_power(struct js320_drv_s * self,
                                struct jsdrvp_mb_dev_s * dev) {
    if (!self->power_compute_on_host) {
        return;
    }
    // Wait until both i and v buffers have actual sample data before
    // multiplying.  Without this guard, an early mult (e.g., after the
    // first i frame arrives but before the first v frame) calls
    // sbuf_f32_advance on the empty v_buf, setting its head_sample_id
    // to i's sample_id.  When v's first frame later arrives at a
    // slightly different sample_id, sbuf_f32_add interprets the delta
    // as missing samples and NaN-fills them — propagating ~123 NaN
    // values through the synthesized power stream.
    if ((sbuf_f32_length(&self->i_buf) == 0U)
            || (sbuf_f32_length(&self->v_buf) == 0U)) {
        return;
    }
    sbuf_f32_mult(&self->p_buf, &self->i_buf, &self->v_buf);
    uint32_t n = sbuf_f32_length(&self->p_buf);
    if (n == 0U) {
        return;
    }
    // sbuf_f32_mult fills p_buf starting at index 0 (head=n, tail=0 after
    // its internal clear); the buffer is contiguous and the new product
    // samples occupy [0..n).  Compute the u64 starting sample_id from
    // the post-mult head_sample_id minus the count.
    uint64_t end = self->p_buf.head_sample_id;
    uint64_t start = end - (uint64_t) n * (uint64_t) self->p_buf.sample_id_decimate;

    js320_port_append(self, dev, /*ch=*/7U, /*index=*/0U, /*topic_override=*/NULL,
                      start, &self->p_buf.buffer[0], n);
    js320_port_evaluate_flush(self, dev, /*ch=*/7U);
}

// Update the cached enable for one of s/i, s/v, s/p in response to a
// frontend command and re-reconcile.  Returns the rc to send back.
// The s/X/ctrl topic is NOT forwarded directly — reconcile_power decides
// what to forward to the device, which may differ from the user's request
// (e.g. user said "enable power" but at fs=1 MHz with i and v already on,
// we forward s/p/ctrl=0 instead and synthesize p on host).
static int32_t js320_handle_stream_ctrl(struct js320_drv_s * self,
                                        struct jsdrvp_mb_dev_s * dev,
                                        uint8_t ch,
                                        const struct jsdrv_union_s * value) {
    struct jsdrv_union_s v = *value;
    int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
    if (0 != rc) {
        return rc;
    }
    bool enable = (v.value.u32 != 0U);
    self->ports[ch].enabled = enable;

    // Reset the accumulator either way: enabling clears any stale state,
    // disabling drops in-flight samples that would no longer be wanted.
    js320_port_reset(self, dev, ch);
    if (ch == 5U) {
        sbuf_f32_clear(&self->i_buf);
        self->i_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
    } else if (ch == 6U) {
        sbuf_f32_clear(&self->v_buf);
        self->v_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
    } else if (ch == 7U) {
        sbuf_f32_clear(&self->p_buf);
        self->p_buf.sample_id_decimate = (uint8_t) JS320_DECIMATE;
    }

    js320_reconcile_power(self, dev);
    return 0;
}

// --- Driver callbacks ---

static void js320_on_open(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                           const struct mb_link_identity_s * identity) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    self->dev = dev;
    self->identity = identity;
    js320_jtag_on_open(self->jtag, dev);
    js320_fwup_on_open(self->fwup, dev);
    // RAW mode is link-only (recovery fwup); skip frontend-facing metadata
    // publishes that assume the device-side pubsub / streaming will come up.
    if (0xFF != jsdrvp_mb_dev_open_mode(dev)) {
        jsdrvp_mb_dev_send_to_frontend(dev, "h/fp$",
            &jsdrv_union_cjson_r(js320_publish_rate_meta));
    }
    JSDRV_LOGI("JS320 driver opened: vendor=0x%04x product=0x%04x",
               identity->vendor_id, identity->product_id);
}

// The JS320 exposes two top-level pubsub tasks: `c` (ctrl-side) and
// `s` (sensor-side).  state_fetch must enumerate both.
static const char * js320_state_fetch_prefixes(struct jsdrvp_mb_drv_s * drv) {
    (void) drv;
    return "cs";
}

// Defer state_fetch until the sensor task publishes — see
// waiting_for_sensor doc on struct js320_drv_s.  The JS320 ctrl
// firmware powers the sensor only after USBD reports connected, so
// the sensor-side pubsub can take up to ~1 s to come up after the
// host has already reached ST_OPEN.  Arming a 2 s driver timeout here
// lets on_timeout fall back to state_fetch_start if the sensor-ready
// signal never arrives.
static bool js320_open_ready(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    self->waiting_for_sensor = true;
    jsdrvp_mb_dev_set_timeout(dev, jsdrv_time_utc() + JS320_SENSOR_READY_TIMEOUT_NS);
    return false;  // defer
}

static void js320_on_close(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    // Drop any in-flight per-port accumulators so we don't leak the
    // underlying messages back to the free list with stale state.
    for (uint8_t ch = 0U; ch < JS320_CH_COUNT; ++ch) {
        js320_port_reset(self, dev, ch);
    }
    js320_sbufs_clear(self);
    self->power_compute_on_host = false;
    self->signal_dwn_n = JS320_DEFAULT_SIGNAL_DWN_N;
    self->gpi_dwn_mode = JS320_DEFAULT_GPI_DWN_MODE;
    self->gpi_dwn_n = JS320_DEFAULT_GPI_DWN_N;
    memset(&self->signal_ack, 0, sizeof(self->signal_ack));
    memset(&self->gpi_ack,    0, sizeof(self->gpi_ack));
    for (uint32_t k = 0U; k < JS320_CH_COUNT; ++k) {
        self->last_sent_ctrl[k] = JS320_CTRL_UNKNOWN;
    }
    self->waiting_for_sensor = false;
    js320_fwup_on_close(self->fwup);
    js320_jtag_on_close(self->jtag);
    self->dev = NULL;
    JSDRV_LOGI("JS320 driver closed");
}

static void js320_handle_statistics(struct js320_drv_s * self,
                                    struct jsdrvp_mb_dev_s * dev,
                                    const uint32_t * data, uint8_t length) {
    uint32_t size = (uint32_t) length * 4;
    if (size != sizeof(struct js320_statistics_raw_s)) {
        JSDRV_LOGW("statistics size mismatch: %u != %u",
                   size, (uint32_t) sizeof(struct js320_statistics_raw_s));
        return;
    }
    struct jsdrv_context_s * context = jsdrvp_mb_dev_context(dev);
    const char * prefix = jsdrvp_mb_dev_prefix(dev);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, prefix);
    jsdrv_topic_append(&topic, "s/stats/value");
    jsdrv_cstr_copy(m->topic, topic.topic, sizeof(m->topic));

    struct jsdrv_statistics_s * dst = (struct jsdrv_statistics_s *) m->payload.bin;
    m->value = jsdrv_union_cbin_r((uint8_t *) dst, sizeof(*dst));
    m->value.app = JSDRV_PAYLOAD_TYPE_STATISTICS;

    struct js320_statistics_raw_s src;
    memcpy(&src, data, sizeof(src));
    if (0 == js320_stats_convert(&src, dst)) {
        dst->i_avg *= self->i_scale;
        dst->i_std *= self->i_scale;
        dst->i_min *= self->i_scale;
        dst->i_max *= self->i_scale;
        dst->v_avg *= self->v_scale;
        dst->v_std *= self->v_scale;
        dst->v_min *= self->v_scale;
        dst->v_max *= self->v_scale;
        double p_scale = self->i_scale * self->v_scale;
        dst->p_avg *= p_scale;
        dst->p_std *= p_scale;
        dst->p_min *= p_scale;
        dst->p_max *= p_scale;
        dst->charge_f64 *= self->i_scale;
        dst->energy_f64 *= p_scale;
        const struct jsdrv_time_map_s * mtmap = jsdrvp_mb_dev_time_map(dev, 's');
        if (mtmap) {
            dst->time_map = *mtmap;
            jsdrvp_mb_dev_backend_send(dev, m);
        } else {
            jsdrvp_msg_free(context, m);
        }
    } else {
        jsdrvp_msg_free(context, m);
    }
}

// Forward declaration; definition lives with the other ack helpers
// below, grouped with the other dwnN/!ack plumbing.
static bool js320_ack_should_drop(struct js320_ack_state_s * s, uint64_t sample_id);

static void js320_handle_app(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                              uint16_t metadata, const uint32_t * data, uint8_t length) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    uint8_t channel = (uint8_t) (metadata & 0x000fU);
    uint8_t source = (uint8_t) ((metadata >> 4) & 0x000fU);

    if (channel == 14) {
        js320_handle_statistics(self, dev, data, length);
        return;
    }

    if (length < 2) {
        JSDRV_LOGW("app frame too short: length=%d", length);
        return;
    }

    if (channel >= JS320_CH_COUNT) {
        JSDRV_LOGW("unsupported app channel: %d", channel);
        return;
    }
    const struct js320_port_def_s * def = &PORT_DEFS[channel];
    if (def->data_topic == NULL) {
        JSDRV_LOGW("unsupported app channel: %d", channel);
        return;
    }

    // Determine effective topic + index.  Most channels read the table
    // directly; ADC composes index = base | source, range maps source to
    // i/v sub-topic.
    const char * topic_override = NULL;
    uint8_t index = def->index;
    if (channel == 0 || channel == 1) {
        index = (uint8_t) (def->index | source);
    } else if (channel == 2) {
        if (source > 1) {
            JSDRV_LOGW("unsupported source for range channel: %d", source);
            return;
        }
        topic_override = source ? "s/v/range/!data" : "s/i/range/!data";
        index = source;
    }

    // Parse 64-bit sample_id from the first two u32 words.
    uint64_t sample_id = *((const uint64_t *) data);
    const uint32_t * payload_u32 = data + 2;
    uint32_t length_u32 = (uint32_t) (length - 2);
    uint32_t sample_count = length_u32 * (uint32_t) def->samples_per_u32;

    // Drop-until-ack: after a dwnN change, drop affected frames until
    // the firmware's !ack confirms the new decimation has taken effect.
    // While any ack is outstanding, drop unconditionally; once all acks
    // have arrived, drop frames whose sample_id is below the latest
    // ack's target (which is a frame boundary per the firmware
    // contract).  signal_ack scopes i/v/p (ch 5-7); gpi_ack scopes
    // GPI + trigger (ch 8-12).
    if ((channel >= 5U) && (channel <= 7U)) {
        if (js320_ack_should_drop(&self->signal_ack, sample_id)) {
            return;
        }
    } else if ((channel >= 8U) && (channel <= 12U)) {
        if (js320_ack_should_drop(&self->gpi_ack, sample_id)) {
            return;
        }
    }

    // Smart-power: drop device-originated power frames while host is
    // computing power locally.  We forwarded s/p/ctrl=0 to the device
    // already, but be defensive against in-flight frames.
    if ((channel == 7U) && self->power_compute_on_host) {
        return;
    }

    // Smart-power: feed i and v samples into the host-side compute buffers
    // before appending to the per-port accumulator, so the synthesized
    // power samples can join the same group flush boundary.  Skip the
    // sbuf when host compute is disabled to avoid unnecessary copies.
    if (self->power_compute_on_host && ((channel == 5U) || (channel == 6U))) {
        struct sbuf_f32_s * sbuf = (channel == 5U) ? &self->i_buf : &self->v_buf;
        // The shared sbuf was zero-initialized by js320_sbufs_clear, so
        // head_sample_id is 0.  The first incoming frame has a sample_id
        // anchored at the device's free-running 16 MHz counter (typically
        // some large number).  Without rebasing, sbuf_f32_add() interprets
        // the gap from 0 to sample_id as "missing samples" and NaN-fills
        // up to SAMPLE_BUFFER_LENGTH-1 entries.  Those NaNs then propagate
        // through sbuf_f32_mult into the synthesized power stream.
        if (sbuf->head_sample_id == 0) {
            sbuf->head_sample_id = sample_id;
        }
        sbuf_f32_add(sbuf, sample_id, (float *) payload_u32, sample_count);
    }

    js320_port_append(self, dev, channel, index, topic_override,
                      sample_id, payload_u32, sample_count);
    js320_port_evaluate_flush(self, dev, channel);

    if (self->power_compute_on_host && ((channel == 5U) || (channel == 6U))) {
        js320_compute_power(self, dev);
    }
}

// Start a drop window for one ack family: bump the outstanding counter,
// flag dropping, and arm the device thread timer so on_timeout fires if
// the ack is lost.
static void js320_ack_begin(struct js320_ack_state_s * s,
                            struct jsdrvp_mb_dev_s * dev) {
    s->acks_outstanding += 1U;
    s->dropping = true;
    s->drop_timeout_utc = jsdrv_time_utc() + JS320_DWNN_ACK_TIMEOUT;
    // If fwup/jtag has a sooner timeout armed, on_timeout will wake early
    // for that; we re-arm from on_timeout when still within our deadline.
    jsdrvp_mb_dev_set_timeout(dev, s->drop_timeout_utc);
}

// An !ack arrived: latest sample_id wins as the drop high-water mark.
// Expected to arrive with no outstanding request when the corresponding
// dwnN change was applied while no channel in the family was streaming
// (see js320_apply_signal_dwn_n and the gpi counterparts).  Silently
// ignore in that case: there is no drop window to close.
static void js320_ack_received(struct js320_ack_state_s * s,
                               uint64_t sample_id,
                               const char * label) {
    (void) label;
    if (s->acks_outstanding > 0U) {
        s->drop_until_sample_id = sample_id;
        s->acks_outstanding -= 1U;
    }
}

// Returns true if the caller should drop this frame.  Also flips
// `dropping` to false once a frame at or after the target arrives.
static bool js320_ack_should_drop(struct js320_ack_state_s * s,
                                   uint64_t sample_id) {
    if (!s->dropping) {
        return false;
    }
    if (s->acks_outstanding > 0U) {
        return true;
    }
    if (sample_id < s->drop_until_sample_id) {
        return true;
    }
    s->dropping = false;
    return false;
}

// Called from on_timeout: if we've actually passed our deadline, resume
// passing samples and log a warning.  Otherwise the wake was someone
// else's (fwup/jtag) and we re-arm so we still fire at our deadline.
static void js320_ack_timeout_check(struct js320_ack_state_s * s,
                                    struct jsdrvp_mb_dev_s * dev,
                                    const char * label) {
    if (!s->dropping) {
        return;
    }
    if (jsdrv_time_utc() >= s->drop_timeout_utc) {
        JSDRV_LOGW("%s !ack timeout; resuming with %u ack(s) outstanding",
                   label, (unsigned) s->acks_outstanding);
        s->dropping = false;
        s->acks_outstanding = 0U;
        s->drop_until_sample_id = 0;
    } else {
        jsdrvp_mb_dev_set_timeout(dev, s->drop_timeout_utc);
    }
}

// True iff any channel in the signal (i/v/p) family is currently enabled.
static inline bool js320_signal_family_streaming(const struct js320_drv_s * self) {
    return self->ports[5].enabled
        || self->ports[6].enabled
        || self->ports[7].enabled;
}

// True iff any channel in the gpi (GPI + trigger) family is currently enabled.
static inline bool js320_gpi_family_streaming(const struct js320_drv_s * self) {
    for (uint8_t ch = 8U; ch <= 12U; ++ch) {
        if (self->ports[ch].enabled) {
            return true;
        }
    }
    return false;
}

// Apply a new s/dwnN/N value.  This is the single code path both the
// `s/dwnN/N` and `h/fs` handlers funnel into: on JS320 decimation is
// always on-instrument, so `h/fs` is just a rate-to-register translator
// that ultimately produces the same side effects.  Responsibilities:
//   - record the new register value in self->signal_dwn_n
//   - derive the resulting host-visible fs (1 MHz / extra_factor) so
//     smart-power and any other fs-dependent logic stays coherent
//     whether the caller used h/fs or s/dwnN/N
//   - flush the i/v/p accumulators + sbufs
//   - start the drop-until-ack window
//   - forward to the device as `s/dwnN/N`
//   - reconcile smart-power with the (possibly) new fs
// The device handler brackets the decimate register update with a
// streaming disable/enable and publishes s/dwnN/!ack carrying the
// sample_id of the first new-rate sample; that ack releases the drop
// window (handled in js320_handle_publish).
static void js320_apply_signal_dwn_n(struct js320_drv_s * self,
                                     struct jsdrvp_mb_dev_s * dev,
                                     uint32_t n) {
    self->signal_dwn_n = n;
    self->fs = JS320_SIGNAL_RATE_AFTER_DECIMATE / js320_signal_extra_factor(n);
    for (uint8_t ch = 5U; ch <= 7U; ++ch) {
        js320_port_reset(self, dev, ch);
    }
    js320_sbufs_clear(self);
    // Skip the drop-until-ack window when no channel in the signal family is
    // streaming: there are no in-flight old-rate samples to hide, and the
    // firmware may not emit an !ack without an active stream.
    if (js320_signal_family_streaming(self)) {
        js320_ack_begin(&self->signal_ack, dev);
    }
    jsdrvp_mb_dev_publish_to_device(dev, "s/dwnN/N", &jsdrv_union_u32_r(n));
    js320_reconcile_power(self, dev);
}

// Apply a new s/gpi/+/dwnN/mode value.  Resets GPI ports (ch 8-12),
// starts the drop-until-ack window, and forwards to the device.  The
// device handler brackets the gpi_decimate register update with a
// streaming disable/enable and publishes s/gpi/+/dwnN/!ack.
static void js320_apply_gpi_dwn_mode(struct js320_drv_s * self,
                                     struct jsdrvp_mb_dev_s * dev,
                                     uint32_t mode) {
    self->gpi_dwn_mode = mode;
    for (uint8_t ch = 8U; ch <= 12U; ++ch) {
        js320_port_reset(self, dev, ch);
    }
    if (js320_gpi_family_streaming(self)) {
        js320_ack_begin(&self->gpi_ack, dev);
    }
    jsdrvp_mb_dev_publish_to_device(dev, "s/gpi/+/dwnN/mode",
        &jsdrv_union_u32_r(mode));
}

// Apply a new s/gpi/+/dwnN/N value.  Same behavior as apply_gpi_dwn_mode
// but for the factor topic.
static void js320_apply_gpi_dwn_n(struct js320_drv_s * self,
                                  struct jsdrvp_mb_dev_s * dev,
                                  uint32_t n) {
    self->gpi_dwn_n = n;
    for (uint8_t ch = 8U; ch <= 12U; ++ch) {
        js320_port_reset(self, dev, ch);
    }
    if (js320_gpi_family_streaming(self)) {
        js320_ack_begin(&self->gpi_ack, dev);
    }
    jsdrvp_mb_dev_publish_to_device(dev, "s/gpi/+/dwnN/N",
        &jsdrv_union_u32_r(n));
}

// Map a requested host sample rate `fs` to the s/dwnN/N register value.
// The JS320 always runs decimation on-instrument: native 16 MHz is first
// divided by JS320_DECIMATE (16) to 1 MHz, then divided further by
// js320_signal_extra_factor(n).  Returns 0 on success (*n_out written)
// or a jsdrv error code if `fs` does not correspond to a supported value.
static int32_t js320_fs_to_dwn_n(uint32_t fs, uint32_t * n_out) {
    if ((fs == 0U) || (fs > JS320_SIGNAL_RATE_AFTER_DECIMATE)) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if ((JS320_SIGNAL_RATE_AFTER_DECIMATE % fs) != 0U) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    uint32_t extra = JS320_SIGNAL_RATE_AFTER_DECIMATE / fs;
    if (extra == 1U) {
        *n_out = 0U;
    } else if (extra == 2U) {
        *n_out = 2U;
    } else if (extra == 3U) {
        // gateware clamps 3 to factor 2, which would produce a rate that
        // disagrees with the user's request.  Reject to avoid a silent mismatch.
        return JSDRV_ERROR_PARAMETER_INVALID;
    } else if (extra <= 1000U) {
        *n_out = extra;
    } else {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

static bool handle_cmd(struct js320_drv_s * self,
                            struct jsdrvp_mb_dev_s * dev,
                            const char * subtopic,
                            const struct jsdrv_union_s * value) {
    if (0 == strcmp(subtopic, "s/i/ctrl")) {
        int32_t rc = js320_handle_stream_ctrl(self, dev, /*ch=*/5U, value);
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "s/v/ctrl")) {
        int32_t rc = js320_handle_stream_ctrl(self, dev, /*ch=*/6U, value);
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "s/p/ctrl")) {
        int32_t rc = js320_handle_stream_ctrl(self, dev, /*ch=*/7U, value);
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "h/fs")) {
        // h/fs is a thin front-end for s/dwnN/N: translate the requested
        // sample rate to the register value and hand off to the same
        // code path.  Every side effect (device publish of s/dwnN/N,
        // ack bookkeeping, port flush, smart-power reconcile) lives in
        // js320_apply_signal_dwn_n so h/fs and s/dwnN/N stay in lockstep.
        struct jsdrv_union_s v = *value;
        int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
        uint32_t n = 0U;
        if (0 == rc) {
            rc = js320_fs_to_dwn_n(v.value.u32, &n);
        }
        if (0 == rc) {
            js320_apply_signal_dwn_n(self, dev, n);
        }
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "h/fp")) {
        struct jsdrv_union_s v = *value;
        int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
        if (0 == rc) {
            uint32_t fp = v.value.u32;
            if (fp < 1) {
                fp = 1;  // avoid div-by-zero in element_count_max
            }
            self->publish_rate = fp;
        }
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "h/i_scale")) {
        if (value->type == JSDRV_UNION_F64) {
            self->i_scale = value->value.f64;
        } else if (value->type == JSDRV_UNION_F32) {
            self->i_scale = (double) value->value.f32;
        }
        return true;
    } else if (0 == strcmp(subtopic, "h/v_scale")) {
        if (value->type == JSDRV_UNION_F64) {
            self->v_scale = value->value.f64;
        } else if (value->type == JSDRV_UNION_F32) {
            self->v_scale = (double) value->value.f32;
        }
        return true;
    } else if (0 == strcmp(subtopic, "s/dwnN/N")) {
        // Track the device-side i/v/p decimation factor and forward
        // through the shared apply helper.  The apply helper resets
        // the i/v/p ports + sbufs, starts the drop-until-ack window,
        // and sends s/dwnN/N to the device.  Return true (we have
        // forwarded explicitly via the helper) with an explicit rc.
        struct jsdrv_union_s v = *value;
        int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
        if (0 == rc) {
            js320_apply_signal_dwn_n(self, dev, v.value.u32);
        }
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "s/gpi/+/dwnN/mode")) {
        // Track the GPI decimation mode (0=off, 1=toggle, 2=first,
        // 3=majority) and forward through the shared apply helper.
        struct jsdrv_union_s v = *value;
        int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
        if (0 == rc) {
            js320_apply_gpi_dwn_mode(self, dev, v.value.u32);
        }
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else if (0 == strcmp(subtopic, "s/gpi/+/dwnN/N")) {
        // Track the GPI decimation factor and forward through the
        // shared apply helper.
        struct jsdrv_union_s v = *value;
        int32_t rc = jsdrv_union_as_type(&v, JSDRV_UNION_U32);
        if (0 == rc) {
            js320_apply_gpi_dwn_n(self, dev, v.value.u32);
        }
        jsdrvp_mb_dev_send_return_code(dev, subtopic, rc);
        return true;
    } else {
        return false;
    }
}


static bool js320_handle_cmd(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                              const char * subtopic, const struct jsdrv_union_s * value) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    return (false
        || handle_cmd(self, dev, subtopic, value)
        || js320_fwup_handle_cmd(self->fwup, subtopic, value)
        || js320_jtag_handle_cmd(self->jtag, subtopic, value)
    );
}

static bool js320_handle_publish(struct jsdrvp_mb_drv_s * drv,
                                  struct jsdrvp_mb_dev_s * dev,
                                  const char * subtopic,
                                  const struct jsdrv_union_s * value) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;

    // Sensor-ready interlock: any publish from the `s/...` subtree is
    // proof that the sensor task's pubsub is running, and state_fetch
    // can now proceed without hitting BUSY.
    if (self->waiting_for_sensor && (subtopic[0] == 's') && (subtopic[1] == '/')) {
        JSDRV_LOGI("sensor ready: %s", subtopic);
        self->waiting_for_sensor = false;
        jsdrvp_mb_dev_set_timeout(dev, 0);
        jsdrvp_mb_dev_state_fetch_start(dev);
        // Trigger sensor timesync resync so s/ts/!map arrives
        // promptly.  The sensor-side comm link stays up across
        // host reconnects, so ./comm/./!add only fires 
        // on the first open.  This explicit resync forces map update.
        jsdrvp_mb_dev_publish_to_device(dev, "s/ts/!resync",
            &jsdrv_union_u8_r(1));
        // fall through; we want the frontend to see this publish too
    }

    // Firmware acknowledgment that a dwnN change has taken effect.
    // Value is the 16 MHz sample_id at which the new decimation becomes
    // valid.  Latest ack wins; the drop window closes once all
    // outstanding acks have been received AND the data path has seen a
    // frame at or after `drop_until_sample_id`.  Consume; not forwarded
    // to the frontend (internal protocol detail).
    if (0 == strcmp(subtopic, "s/dwnN/!ack")) {
        struct jsdrv_union_s v = *value;
        if (0 == jsdrv_union_as_type(&v, JSDRV_UNION_U64)) {
            js320_ack_received(&self->signal_ack, v.value.u64, "s/dwnN");
        } else {
            JSDRV_LOGW("s/dwnN/!ack bad value type=%u", (unsigned) value->type);
        }
        return true;
    }
    if (0 == strcmp(subtopic, "s/gpi/+/dwnN/!ack")) {
        struct jsdrv_union_s v = *value;
        if (0 == jsdrv_union_as_type(&v, JSDRV_UNION_U64)) {
            js320_ack_received(&self->gpi_ack, v.value.u64, "s/gpi/+/dwnN");
        } else {
            JSDRV_LOGW("s/gpi/+/dwnN/!ack bad value type=%u", (unsigned) value->type);
        }
        return true;
    }
    if (js320_fwup_handle_publish(self->fwup, subtopic, value)) {
        return true;
    }
    return js320_jtag_handle_publish(self->jtag, subtopic, value);
}

static void js320_on_timeout(struct jsdrvp_mb_drv_s * drv,
                              struct jsdrvp_mb_dev_s * dev) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    if (self->waiting_for_sensor) {
        // Sensor never published within JS320_SENSOR_READY_TIMEOUT_NS.
        // Proceed with state_fetch anyway; GET_INIT BUSY retry will
        // backstop if the firmware is genuinely still booting.
        JSDRV_LOGW("sensor ready timeout, proceeding to state_fetch");
        self->waiting_for_sensor = false;
        jsdrvp_mb_dev_state_fetch_start(dev);
    }
    js320_ack_timeout_check(&self->signal_ack, dev, "s/dwnN");
    js320_ack_timeout_check(&self->gpi_ack,    dev, "s/gpi/+/dwnN");
    js320_fwup_on_timeout(self->fwup);
    js320_jtag_on_timeout(self->jtag);
}

static void js320_finalize(struct jsdrvp_mb_drv_s * drv) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    JSDRV_LOGI("JS320 driver finalized");
    js320_fwup_free(self->fwup);
    js320_jtag_free(self->jtag);
    jsdrv_free(self);
}

// --- Factory ---

static int32_t js320_drv_factory(struct jsdrvp_mb_drv_s ** drv) {
    struct js320_drv_s * self = jsdrv_alloc_clr(sizeof(struct js320_drv_s));
    self->i_scale = 1.0;
    self->v_scale = 1.0;
    self->fs = JS320_FS_DEFAULT;
    self->publish_rate = JS320_PUB_RATE_DEFAULT;
    self->signal_dwn_n = JS320_DEFAULT_SIGNAL_DWN_N;
    self->gpi_dwn_mode = JS320_DEFAULT_GPI_DWN_MODE;
    self->gpi_dwn_n = JS320_DEFAULT_GPI_DWN_N;
    js320_sbufs_clear(self);
    for (uint32_t k = 0U; k < JS320_CH_COUNT; ++k) {
        self->last_sent_ctrl[k] = JS320_CTRL_UNKNOWN;
    }
    self->jtag = js320_jtag_alloc();
    self->fwup = js320_fwup_alloc();
    self->drv.on_open = js320_on_open;
    self->drv.on_close = js320_on_close;
    self->drv.handle_app = js320_handle_app;
    self->drv.handle_cmd = js320_handle_cmd;
    self->drv.handle_publish = js320_handle_publish;
    self->drv.on_timeout = js320_on_timeout;
    self->drv.state_fetch_prefixes = js320_state_fetch_prefixes;
    self->drv.open_ready = js320_open_ready;
    self->drv.finalize = js320_finalize;
    *drv = &self->drv;
    return 0;
}

int32_t jsdrvp_js320_device_factory(struct jsdrvp_ul_device_s ** device,
                                     struct jsdrv_context_s * context,
                                     struct jsdrvp_ll_device_s * ll) {
    struct jsdrvp_mb_drv_s * drv = NULL;
    int32_t rv = js320_drv_factory(&drv);
    if (rv) {
        return rv;
    }
    rv = jsdrvp_ul_mb_device_usb_factory(device, context, ll, drv);
    if (rv && drv->finalize) {
        drv->finalize(drv);
    }
    return rv;
}
