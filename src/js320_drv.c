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
#include "jsdrv/topic.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/js320_fwup.h"
#include "jsdrv_prv/js320_jtag.h"
#include "jsdrv_prv/js320_stats.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/mb_drv.h"
#include "jsdrv_prv/platform.h"
#include "mb/comm/link.h"
#include <string.h>


struct js320_drv_s {
    struct jsdrvp_mb_drv_s drv;  // MUST BE FIRST
    const struct mb_link_identity_s * identity;
    struct jsdrvp_mb_dev_s * dev;
    struct js320_jtag_s * jtag;
    struct js320_fwup_s * fwup;
    double i_scale;
    double v_scale;
};

// --- Streaming signal helpers ---

static void signal_adc(struct jsdrv_stream_signal_s * signal) {
    signal->field_id = JSDRV_FIELD_RAW;
    signal->index = 0;
    signal->element_type = JSDRV_DATA_TYPE_INT;
    signal->element_size_bits = 32;
    signal->sample_rate = 16000000;  // 16 MHz
    signal->decimate_factor = 16;    // to 1 MHz
}

static void signal_float(struct jsdrv_stream_signal_s * signal, uint8_t field_id) {
    signal->field_id = field_id;
    signal->index = 0;
    signal->element_type = JSDRV_DATA_TYPE_FLOAT;
    signal->element_size_bits = 32;
    signal->sample_rate = 16000000;  // 16 MHz
    signal->decimate_factor = 16;    // to 1 MHz
}

static void signal_gpi(struct jsdrv_stream_signal_s * signal, uint8_t channel) {
    signal->field_id = JSDRV_FIELD_GPI;
    signal->index = channel;
    signal->element_type = JSDRV_DATA_TYPE_UINT;
    signal->element_size_bits = 1;
    signal->sample_rate = 16000000;  // 16 MHz
    signal->decimate_factor = 16;    // to 1 MHz
}

// --- Driver callbacks ---

static void js320_on_open(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                           const struct mb_link_identity_s * identity) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    self->dev = dev;
    self->identity = identity;
    js320_jtag_on_open(self->jtag, dev);
    js320_fwup_on_open(self->fwup, dev);
    JSDRV_LOGI("JS320 driver opened: vendor=0x%04x product=0x%04x",
               identity->vendor_id, identity->product_id);
}

static void js320_on_close(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    (void) dev;
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
        // Lazy-init: until the sensor's timesync converges, seed the
        // device time_map with host UTC + the current statistics sample
        // id and the native counter rate.  Mirrors JS220 behavior in
        // js220_usb.c::time_map_update().
        struct jsdrv_time_map_s * mtmap = jsdrvp_mb_dev_time_map_mut(dev);
        if (mtmap->offset_time == 0) {
            mtmap->offset_time = jsdrv_time_utc();
            mtmap->offset_counter = dst->block_sample_id;
            mtmap->counter_rate = (double) dst->sample_freq;
        }
        dst->time_map = *mtmap;
        jsdrvp_mb_dev_backend_send(dev, m);
    } else {
        jsdrvp_msg_free(context, m);
    }
}

static void js320_handle_app(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                              uint16_t metadata, const uint32_t * data, uint8_t length) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    uint8_t channel = metadata & 0x000fU;
    uint8_t source = (metadata >> 4) & 0x000fU;

    if (channel == 14) {
        js320_handle_statistics(self, dev, data, length);
        return;
    }

    if (length < 2) {
        JSDRV_LOGW("app frame too short: length=%d", length);
        return;
    }

    // Determine subtopic before allocating message
    const char * subtopic = NULL;
    switch (channel) {
        case 0:  subtopic = "s/adc/0/!data"; break;
        case 1:  subtopic = "s/adc/1/!data"; break;
        case 2:
            if (source > 1) {
                JSDRV_LOGW("unsupported source for range channel: %d", source);
            } else {
                subtopic = source ? "s/v/range/!data" : "s/i/range/!data";
            }
            break;
        case 5:  subtopic = "s/i/!data"; break;
        case 6:  subtopic = "s/v/!data"; break;
        case 7:  subtopic = "s/p/!data"; break;
        case 8:  subtopic = "s/gpi/0/!data"; break;
        case 9:  subtopic = "s/gpi/1/!data"; break;
        case 10: subtopic = "s/gpi/2/!data"; break;
        case 11: subtopic = "s/gpi/3/!data"; break;
        case 12: subtopic = "s/gpi/7/!data"; break;
        case 13: subtopic = "s/uart/!data"; break;
        default: break;
    }
    if (NULL == subtopic) {
        JSDRV_LOGW("unsupported app channel: %d", channel);
        return;
    }

    struct jsdrv_context_s * context = jsdrvp_mb_dev_context(dev);
    const char * prefix = jsdrvp_mb_dev_prefix(dev);
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);

    // process value
    m->value.type = JSDRV_UNION_BIN;
    m->value.app = JSDRV_PAYLOAD_TYPE_STREAM;
    m->value.value.bin = m->payload.bin;
    struct jsdrv_stream_signal_s * signal = (struct jsdrv_stream_signal_s *) m->payload.bin;
    signal->sample_id = *((const uint64_t *) data);
    data += 2;
    uint32_t length_u32 = (length - 2);  // sample_id is 8 bytes
    m->value.size = length_u32 << 2;
    signal->element_count = length_u32;

    // Configure signal fields based on channel
    switch (channel) {
        case 0:
            signal_adc(signal);
            signal->index = 0x00 | source;
            signal->element_count = length_u32;
            break;
        case 1:
            signal_adc(signal);
            signal->index = 0x10 | source;
            signal->element_count = length_u32;
            break;
        case 2:
            signal->field_id = JSDRV_FIELD_RANGE;
            signal->index = source;
            signal->element_type = JSDRV_DATA_TYPE_UINT;
            signal->element_size_bits = 4;
            signal->element_count *= 8;  // nibbles (32 / 4)
            signal->sample_rate = 16000000;  // 16 MHz
            signal->decimate_factor = 16;    // to 1 MHz
            break;
        case 5:
            signal_float(signal, JSDRV_FIELD_CURRENT);
            break;
        case 6:
            signal_float(signal, JSDRV_FIELD_VOLTAGE);
            break;
        case 7:
            signal_float(signal, JSDRV_FIELD_POWER);
            break;
        case 8:
            signal_gpi(signal, 0);
            signal->element_count *= 32;
            break;
        case 9:
            signal_gpi(signal, 1);
            signal->element_count *= 32;
            break;
        case 10:
            signal_gpi(signal, 2);
            signal->element_count *= 32;
            break;
        case 11:
            signal_gpi(signal, 3);
            signal->element_count *= 32;
            break;
        case 12:
            signal_gpi(signal, 7);
            signal->element_count *= 32;
            break;
        case 13:
            signal->field_id = JSDRV_FIELD_UART;
            signal->index = 0;
            signal->element_type = JSDRV_DATA_TYPE_UINT;
            signal->element_size_bits = 8;
            signal->element_count *= 4;
            signal->sample_rate = 0;
            signal->decimate_factor = 1;
            break;
        default:
            break;  // unreachable (validated above)
    }

    // Lazy-init: until the sensor's timesync converges, seed the device
    // time_map with host UTC + the current sample_id and the signal's
    // native sample rate.  Mirrors JS220 behavior in
    // js220_usb.c::time_map_update().  After the first s/ts/!map arrives,
    // mb_device.c::handle_in_timesync_map() refines the values.
    struct jsdrv_time_map_s * mtmap = jsdrvp_mb_dev_time_map_mut(dev);
    if (mtmap->offset_time == 0) {
        mtmap->offset_time = jsdrv_time_utc();
        mtmap->offset_counter = signal->sample_id;
        mtmap->counter_rate = (double) signal->sample_rate;
    }
    signal->time_map = *mtmap;

    // process topic
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, prefix);
    jsdrv_topic_append(&topic, subtopic);
    jsdrv_cstr_copy(m->topic, topic.topic, sizeof(m->topic));

    memcpy(signal->data, data, m->value.size);
    jsdrvp_mb_dev_backend_send(dev, m);
}

static bool handle_cmd(struct js320_drv_s * self,
                            const char * subtopic,
                            const struct jsdrv_union_s * value) {
    if (0 == strcmp(subtopic, "h/fs")) {
        return true;  // todo
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
    } else {
        return false;
    }
}


static bool js320_handle_cmd(struct jsdrvp_mb_drv_s * drv, struct jsdrvp_mb_dev_s * dev,
                              const char * subtopic, const struct jsdrv_union_s * value) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    (void) dev;
    return (false
        || handle_cmd(self, subtopic, value)
        || js320_fwup_handle_cmd(self->fwup, subtopic, value)
        || js320_jtag_handle_cmd(self->jtag, subtopic, value)
    );
}

static bool js320_handle_publish(struct jsdrvp_mb_drv_s * drv,
                                  struct jsdrvp_mb_dev_s * dev,
                                  const char * subtopic,
                                  const struct jsdrv_union_s * value) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    (void) dev;
    if (js320_fwup_handle_publish(self->fwup, subtopic, value)) {
        return true;
    }
    return js320_jtag_handle_publish(self->jtag, subtopic, value);
}

static void js320_on_timeout(struct jsdrvp_mb_drv_s * drv,
                              struct jsdrvp_mb_dev_s * dev) {
    struct js320_drv_s * self = (struct js320_drv_s *) drv;
    (void) dev;
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
    self->jtag = js320_jtag_alloc();
    self->fwup = js320_fwup_alloc();
    self->drv.on_open = js320_on_open;
    self->drv.on_close = js320_on_close;
    self->drv.handle_app = js320_handle_app;
    self->drv.handle_cmd = js320_handle_cmd;
    self->drv.handle_publish = js320_handle_publish;
    self->drv.on_timeout = js320_on_timeout;
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
