/*
 * Copyright 2026 Jetperch LLC
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
#include "jsdrv/time.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/check32.h"
#include "jsdrv_prv/devices/js320/js320_cal.h"
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "mb/stdmsg.h"
#include "tinyprintf.h"
#include <string.h>


JSDRV_STATIC_ASSERT(sizeof(struct jsdrv_cal_cmd_s) == 12, cal_cmd_size);
JSDRV_STATIC_ASSERT(sizeof(struct jsdrv_cal_rsp_s) == 8, cal_rsp_size);


// --- Flash layout ---

#define FLASH_BLOCK_SHIFT           16U
#define FLASH_PAGE_SIZE             256U
#define FLASH_PAGES_PER_RECORD      (JSDRV_CAL_RECORD_SIZE / FLASH_PAGE_SIZE)
#define FLASH_BLOCK_SIZE            65536U
#define CAL_MEM_TOPIC               "s/flash/!cmd"
#define MEM_OP_TIMEOUT_MS           2000U
#define ERASE_OP_TIMEOUT_MS         10000U

/// Mirrors firmware/fpga_mcu/src/flash.c FLASH_BLOCK_CAL_*.
static const uint8_t SLOT_TO_BLOCK[JSDRV_CAL_SLOT_COUNT] = {
    [JSDRV_CAL_SLOT_ACTIVE]  = 24,
    [JSDRV_CAL_SLOT_TRIM2]   = 25,
    [JSDRV_CAL_SLOT_TRIM1]   = 26,
    [JSDRV_CAL_SLOT_FIELD]   = 27,
    [JSDRV_CAL_SLOT_LAB]     = 28,
    [JSDRV_CAL_SLOT_FACTORY] = 29,
};


// --- Calibration record layout (struct js320_calibration_s) ---

#define CAL_OFFSET_CREATE_TIME       32
#define CAL_OFFSET_OFFSETS           88     // int32_t offsets[72]
#define CAL_OFFSET_SIGNATURE         952
#define CAL_OFFSET_SIGNATURE_SIZE    64
#define CAL_OFFSET_CHECK32           1020   // last 4 bytes
#define CAL_POINTS_PER_CURRENT_CHAN  24
#define CAL_VOLTAGE_IDX              70     // offsets[70] and offsets[71]

static const uint8_t CAL_HEADER_MAGIC[16] = {
    0x4a, 0x53, 0x33, 0x32, 0x30, 0x63, 0x61, 0x6c,
    0x0d, 0x0a, 0x20, 0x0a, 0x1a, 0xb2, 0x1c, 0x00
};


// --- Current cal sweep ---

// (i_select, i_mux_select) -> linear point index within a channel.
// Upper-triangular layout per calibration.h:60-77.
// Indexed by i_select * 6 + i_mux_select; entries below the diagonal
// are unused.
#define IDX_UNUSED 0xFFU
static const uint8_t CAL_INDEX_MAP[36] = {
    /* i_select=0 */ 0,          1,          2,          3,          4,          5,
    /* i_select=1 */ IDX_UNUSED, 6,          7,          8,          9,          10,
    /* i_select=2 */ IDX_UNUSED, IDX_UNUSED, 11,         12,         13,         14,
    /* i_select=3 */ IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, 15,         16,         17,
    /* i_select=4 */ IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, 18,         19,
    /* i_select=5 */ IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, IDX_UNUSED, 20,
};

// 12 power-line cycles at 1 MHz = 200k samples per point.
#define DEFAULT_SAMPLES_PER_POINT    200000U
#define MUX_SETTLE_MS                20U


// --- Save/restore topics ---

#define SAVE_TOPIC_COUNT 11
static const char * const SAVE_TOPICS[SAVE_TOPIC_COUNT] = {
    "s/i/range/mode",
    "s/i/range/select",
    "s/i/i0_sel",
    "s/i/i1_sel",
    "s/i/i2_sel",
    "s/v/range/mode",
    "s/v/range/select",
    "s/adc/0/sel",
    "s/adc/0/ctrl",
    "s/adc/1/sel",
    "s/adc/1/ctrl",
};


// --- State machine ---

enum cal_state_e {
    CAL_IDLE = 0,
    CAL_READ,           ///< Reading record from a slot, one page at a time.
    CAL_ERASE,          ///< Erasing the destination slot's 64 KB block.
    CAL_WRITE,          ///< Writing record pages.
    CAL_VERIFY,         ///< Reading back to verify a written record.
    CAL_SWEEP_SETTLE,   ///< Waiting for mux/range settling before a capture.
    CAL_SWEEP_CAPTURE,  ///< Accumulating ADC samples for a sweep point.
};


// --- Context ---

struct js320_cal_s {
    struct jsdrvp_mb_dev_s * dev;

    // Cached values for save/restore, updated by handle_cmd / handle_publish.
    uint32_t saved_value[SAVE_TOPIC_COUNT];
    uint8_t  saved_known[SAVE_TOPIC_COUNT];

    // Snapshot taken at offset-cal entry; restored at op completion.
    uint32_t prior_value[SAVE_TOPIC_COUNT];
    uint8_t  prior_known[SAVE_TOPIC_COUNT];
    bool     restore_pending;

    // Active command.
    uint8_t  state;                 ///< cal_state_e
    uint8_t  op;                    ///< jsdrv_cal_op_e
    uint32_t transaction_id;
    uint8_t  src_slot;
    uint8_t  dst_slot;
    uint32_t samples_per_point;

    // Record buffer (read source, then mutated for offset cal).
    uint8_t  record[JSDRV_CAL_RECORD_SIZE];

    // Flash op tracking.
    uint32_t mem_cmd_id;             ///< Last txn_id we issued.
    uint8_t  page_idx;               ///< Current page in CAL_READ/WRITE/VERIFY.
    uint8_t  page_count;             ///< Pages in the current phase.
    uint32_t flash_offset_base;      ///< Slot base offset for the current phase.

    // Sweep tracking.
    uint8_t  sweep_i_select;
    uint8_t  sweep_i_mux;
    uint8_t  sweep_pass;             ///< 0 = pass A (ADC0+ADC1), 1 = pass B (ADC2).
    uint8_t  voltage_v_select;       ///< For voltage cal: 0 or 1.

    // ADC sample accumulators.  Two slots match s/adc/0/!data and
    // s/adc/1/!data.  need[i] = 0 means "slot not in use; ignore".
    int64_t  adc_sum[2];
    uint32_t adc_count[2];
    uint32_t adc_need[2];
};


// --- Utility ---

static uint32_t slot_to_offset(uint8_t slot) {
    return ((uint32_t) SLOT_TO_BLOCK[slot]) << FLASH_BLOCK_SHIFT;
}

static const char * op_name(uint8_t op) {
    switch (op) {
        case JSDRV_CAL_OP_SLOT_READ:      return "slot_read";
        case JSDRV_CAL_OP_SLOT_COPY:      return "slot_copy";
        case JSDRV_CAL_OP_CURRENT_OFFSET: return "current_offset";
        case JSDRV_CAL_OP_VOLTAGE_OFFSET: return "voltage_offset";
        default:                          return "?";
    }
}

// Status is never emitted while CAL_IDLE; only active states reach here.
static const char * state_name(uint8_t state) {
    switch (state) {
        case CAL_READ:           return "read";
        case CAL_ERASE:          return "erase";
        case CAL_WRITE:          return "write";
        case CAL_VERIFY:         return "verify";
        case CAL_SWEEP_SETTLE:   return "settle";
        case CAL_SWEEP_CAPTURE:  return "capture";
        default:                 return "?";
    }
}

static int save_index(const char * subtopic) {
    for (int i = 0; i < SAVE_TOPIC_COUNT; ++i) {
        if (0 == strcmp(subtopic, SAVE_TOPICS[i])) {
            return i;
        }
    }
    return -1;
}

static bool union_as_u32(const struct jsdrv_union_s * value, uint32_t * out) {
    struct jsdrv_union_s v = *value;
    if (0 != jsdrv_union_as_type(&v, JSDRV_UNION_U32)) {
        return false;
    }
    *out = v.value.u32;
    return true;
}


// --- Record packers ---

static void pack_u32_le(uint8_t * dst, uint32_t v) {
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

static void pack_i64_le(uint8_t * dst, int64_t v) {
    uint64_t u = (uint64_t) v;
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(u >> (i * 8));
    }
}

static void cal_set_offset(uint8_t * rec, uint32_t idx, int32_t q31) {
    pack_u32_le(rec + CAL_OFFSET_OFFSETS + idx * 4, (uint32_t) q31);
}

static int32_t cal_validate(const uint8_t * rec) {
    if (0 != memcmp(rec, CAL_HEADER_MAGIC, sizeof(CAL_HEADER_MAGIC))) {
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    uint32_t expected =
        ((uint32_t) rec[CAL_OFFSET_CHECK32])
        | ((uint32_t) rec[CAL_OFFSET_CHECK32 + 1] << 8)
        | ((uint32_t) rec[CAL_OFFSET_CHECK32 + 2] << 16)
        | ((uint32_t) rec[CAL_OFFSET_CHECK32 + 3] << 24);
    uint32_t actual = jsdrv_check32_xxhash((const uint32_t *) rec, 255);
    if (expected != actual) {
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

static void cal_stamp_signature_and_check(uint8_t * rec) {
    pack_i64_le(rec + CAL_OFFSET_CREATE_TIME, jsdrv_time_utc());
    uint8_t * sig = rec + CAL_OFFSET_SIGNATURE;
    memset(sig, 0, CAL_OFFSET_SIGNATURE_SIZE);
    memcpy(sig, JSDRV_CAL_SIGNATURE_MAGIC, 16);
    pack_u32_le(sig + 16, JSDRV_VERSION_U32);
    uint32_t check = jsdrv_check32_xxhash((const uint32_t *) rec, 255);
    pack_u32_le(rec + CAL_OFFSET_CHECK32, check);
}


// --- Status / response publishing ---

static void cal_send_status(struct js320_cal_s * self, uint16_t pct_x10,
                            const char * msg) {
    char buf[192];
    uint32_t pct_whole = pct_x10 / 10;
    uint32_t pct_frac = pct_x10 % 10;
    tfp_snprintf(buf, sizeof(buf),
        "{\"op\":\"%s\",\"state\":\"%s\",\"pct\":%u.%u,\"msg\":\"%s\"}",
        op_name(self->op), state_name(self->state),
        pct_whole, pct_frac, msg);
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/cal/!status",
        &jsdrv_union_cstr_r(buf));
}

static void cal_send_rsp(struct js320_cal_s * self, int32_t status) {
    struct jsdrv_cal_rsp_s rsp;
    rsp.transaction_id = self->transaction_id;
    rsp.status = status;
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/cal/!rsp",
        &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
}


// --- Mem op senders ---

static void cal_send_mem_op(struct js320_cal_s * self, uint8_t op,
                            uint32_t offset, uint32_t length,
                            const uint8_t * data, uint32_t data_size) {
    uint8_t buf[sizeof(struct mb_stdmsg_header_s)
              + sizeof(struct mb_stdmsg_mem_s)
              + FLASH_PAGE_SIZE];
    uint32_t msg_size = sizeof(struct mb_stdmsg_header_s)
                      + sizeof(struct mb_stdmsg_mem_s) + data_size;

    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    memset(cmd, 0, sizeof(*cmd));
    self->mem_cmd_id += 1;
    cmd->transaction_id = self->mem_cmd_id;
    cmd->operation = op;
    cmd->offset = offset;
    cmd->length = length;
    cmd->timeout_ms = (op == MB_STDMSG_MEM_OP_ERASE)
        ? ERASE_OP_TIMEOUT_MS : MEM_OP_TIMEOUT_MS;
    if (data && data_size) {
        memcpy(cmd->data, data, data_size);
    }

    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = msg_size;
    v.value.bin = buf;
    jsdrvp_mb_dev_publish_to_device(self->dev, CAL_MEM_TOPIC, &v);
}

static void cal_send_read_page(struct js320_cal_s * self) {
    uint32_t off = self->flash_offset_base + self->page_idx * FLASH_PAGE_SIZE;
    cal_send_mem_op(self, MB_STDMSG_MEM_OP_READ, off, FLASH_PAGE_SIZE, NULL, 0);
}

static void cal_send_write_page(struct js320_cal_s * self) {
    uint32_t off = self->flash_offset_base + self->page_idx * FLASH_PAGE_SIZE;
    const uint8_t * src = self->record + self->page_idx * FLASH_PAGE_SIZE;
    cal_send_mem_op(self, MB_STDMSG_MEM_OP_WRITE, off, FLASH_PAGE_SIZE,
                    src, FLASH_PAGE_SIZE);
}

static void cal_send_erase(struct js320_cal_s * self) {
    cal_send_mem_op(self, MB_STDMSG_MEM_OP_ERASE,
                    self->flash_offset_base, FLASH_BLOCK_SIZE, NULL, 0);
}


// --- Streaming-state save / restore ---

static void cal_snapshot_prior(struct js320_cal_s * self) {
    memcpy(self->prior_value, self->saved_value, sizeof(self->prior_value));
    memcpy(self->prior_known, self->saved_known, sizeof(self->prior_known));
    self->restore_pending = true;
}

static void cal_restore_prior(struct js320_cal_s * self) {
    if (!self->restore_pending) {
        return;
    }
    self->restore_pending = false;
    for (int i = 0; i < SAVE_TOPIC_COUNT; ++i) {
        if (!self->prior_known[i]) {
            continue;
        }
        jsdrvp_mb_dev_publish_to_device(self->dev, SAVE_TOPICS[i],
            &jsdrv_union_u32(self->prior_value[i]));
    }
}


// --- Op completion ---

static void cal_finish(struct js320_cal_s * self, int32_t status) {
    if (status) {
        JSDRV_LOGW("cal %s: failed status=%d", op_name(self->op), (int) status);
    } else {
        JSDRV_LOGI("cal %s: complete", op_name(self->op));
    }
    cal_restore_prior(self);
    jsdrvp_mb_dev_set_timeout(self->dev, 0);
    cal_send_rsp(self, status);
    self->state = CAL_IDLE;
    self->op = 0xFF;
}


// --- Sweep helpers ---

/// Convert a left-aligned i32 ADC mean to a 1Q31 offset.  The ADC stream
/// is already left-aligned 32-bit, so the raw mean is already 1Q31.
static int32_t adc_mean_to_q31(int64_t sum, uint32_t count) {
    if (count == 0) {
        return 0;
    }
    return (int32_t)(sum / (int64_t) count);
}

static void on_adc_data(struct js320_cal_s * self, int slot,
                        const struct jsdrv_union_s * value);

static void on_adc0_sample(void * user_data, const char * topic,
                           const struct jsdrv_union_s * value) {
    (void) topic;
    on_adc_data((struct js320_cal_s *) user_data, 0, value);
}

static void on_adc1_sample(void * user_data, const char * topic,
                           const struct jsdrv_union_s * value) {
    (void) topic;
    on_adc_data((struct js320_cal_s *) user_data, 1, value);
}

/// Subscribe to a device-prefixed topic via the public pubsub.
static void cal_subscribe(struct js320_cal_s * self, const char * subtopic,
                          jsdrv_subscribe_fn fn) {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    tfp_snprintf(topic, sizeof(topic), "%s/%s",
                 jsdrvp_mb_dev_prefix(self->dev), subtopic);
    jsdrv_subscribe(jsdrvp_mb_dev_context(self->dev), topic,
                    JSDRV_SFLAG_PUB, fn, self, 0);
}

static void cal_unsubscribe(struct js320_cal_s * self, const char * subtopic,
                            jsdrv_subscribe_fn fn) {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    tfp_snprintf(topic, sizeof(topic), "%s/%s",
                 jsdrvp_mb_dev_prefix(self->dev), subtopic);
    jsdrv_unsubscribe(jsdrvp_mb_dev_context(self->dev), topic,
                      fn, self, 0);
}

static void cal_arm_stream(struct js320_cal_s * self,
                           int8_t slot0_adc, int8_t slot1_adc) {
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/0/ctrl",
        &jsdrv_union_u32(0));
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/1/ctrl",
        &jsdrv_union_u32(0));
    if (slot0_adc >= 0) {
        jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/0/sel",
            &jsdrv_union_u32((uint32_t) slot0_adc));
        cal_subscribe(self, "s/adc/0/!data", on_adc0_sample);
        jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/0/ctrl",
            &jsdrv_union_u32(1));
    }
    if (slot1_adc >= 0) {
        jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/1/sel",
            &jsdrv_union_u32((uint32_t) slot1_adc));
        cal_subscribe(self, "s/adc/1/!data", on_adc1_sample);
        jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/1/ctrl",
            &jsdrv_union_u32(1));
    }
}

static void cal_disarm_stream(struct js320_cal_s * self) {
    if (self->adc_need[0]) {
        cal_unsubscribe(self, "s/adc/0/!data", on_adc0_sample);
    }
    if (self->adc_need[1]) {
        cal_unsubscribe(self, "s/adc/1/!data", on_adc1_sample);
    }
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/0/ctrl",
        &jsdrv_union_u32(0));
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/adc/1/ctrl",
        &jsdrv_union_u32(0));
}

static void cal_begin_capture(struct js320_cal_s * self,
                              bool use_slot0, bool use_slot1) {
    uint32_t need = self->samples_per_point;
    if (need == 0) {
        need = DEFAULT_SAMPLES_PER_POINT;
    }
    self->adc_sum[0] = 0;
    self->adc_sum[1] = 0;
    self->adc_count[0] = 0;
    self->adc_count[1] = 0;
    self->adc_need[0] = use_slot0 ? need : 0;
    self->adc_need[1] = use_slot1 ? need : 0;
}

static bool capture_complete(struct js320_cal_s * self) {
    for (int i = 0; i < 2; ++i) {
        if (self->adc_need[i] && self->adc_count[i] < self->adc_need[i]) {
            return false;
        }
    }
    return true;
}


// --- Current cal sweep advance ---

// Find the next valid (i_select, i_mux) point after (cur_i, cur_mux).
// Returns false if no more points.
static bool next_sweep_point(uint8_t cur_i, uint8_t cur_mux,
                             uint8_t * next_i, uint8_t * next_mux) {
    uint8_t i = cur_i;
    uint8_t mux = (uint8_t)(cur_mux + 1);
    while (i < 6) {
        if (mux < 6 && CAL_INDEX_MAP[i * 6 + mux] != IDX_UNUSED) {
            *next_i = i;
            *next_mux = mux;
            return true;
        }
        ++i;
        mux = i;
    }
    return false;
}


// --- Op entry helpers ---

static void cal_begin_read(struct js320_cal_s * self, uint8_t slot,
                           const char * status_msg) {
    self->state = CAL_READ;
    self->flash_offset_base = slot_to_offset(slot);
    self->page_idx = 0;
    self->page_count = FLASH_PAGES_PER_RECORD;
    cal_send_status(self, 50, status_msg);
    cal_send_read_page(self);
}

static void cal_begin_erase_active(struct js320_cal_s * self) {
    self->state = CAL_ERASE;
    self->flash_offset_base = slot_to_offset(JSDRV_CAL_SLOT_ACTIVE);
    cal_send_status(self, 900, "erasing cal");
    cal_send_erase(self);
}

static void cal_begin_write(struct js320_cal_s * self) {
    self->state = CAL_WRITE;
    self->page_idx = 0;
    self->page_count = FLASH_PAGES_PER_RECORD;
    cal_send_status(self, 930, "writing cal");
    cal_send_write_page(self);
}

static void cal_begin_verify(struct js320_cal_s * self) {
    self->state = CAL_VERIFY;
    self->page_idx = 0;
    self->page_count = FLASH_PAGES_PER_RECORD;
    cal_send_status(self, 970, "verifying cal");
    cal_send_read_page(self);
}

static void cal_arm_settle(struct js320_cal_s * self) {
    self->state = CAL_SWEEP_SETTLE;
    jsdrvp_mb_dev_set_timeout(self->dev,
        jsdrv_time_utc() + (int64_t) MUX_SETTLE_MS * JSDRV_TIME_MILLISECOND);
}


// --- Current cal step ---

static void cal_current_arm_mux(struct js320_cal_s * self) {
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/i/range/select",
        &jsdrv_union_u32(0x80U | (1U << self->sweep_i_select)));
    uint32_t mux = self->sweep_i_mux;
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/i/i0_sel",
        &jsdrv_union_u32(mux));
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/i/i1_sel",
        &jsdrv_union_u32(mux));
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/i/i2_sel",
        &jsdrv_union_u32(mux));
}

static void cal_current_begin_pass(struct js320_cal_s * self) {
    if (self->sweep_pass == 0) {
        cal_arm_stream(self, 0, 1);   // pass A: ADC0 + ADC1
        cal_begin_capture(self, true, true);
    } else {
        cal_arm_stream(self, 2, -1);  // pass B: ADC2 only
        cal_begin_capture(self, true, false);
    }
    self->state = CAL_SWEEP_CAPTURE;
}

static uint16_t cal_current_pct(struct js320_cal_s * self) {
    // 21 points * 2 passes = 42 captures spanning 100..880 (tenths).
    uint8_t captures_done = 0;
    for (uint8_t i = 0; i < self->sweep_i_select; ++i) {
        for (uint8_t m = i; m < 6; ++m) {
            captures_done += 2;
        }
    }
    for (uint8_t m = self->sweep_i_select; m < self->sweep_i_mux; ++m) {
        captures_done += 2;
    }
    if (self->sweep_pass == 1) {
        captures_done += 1;
    }
    return (uint16_t)(100U + ((uint32_t) captures_done * 780U) / 42U);
}

static void cal_current_after_capture(struct js320_cal_s * self) {
    uint8_t idx = CAL_INDEX_MAP[self->sweep_i_select * 6 + self->sweep_i_mux];
    int32_t mean0 = adc_mean_to_q31(self->adc_sum[0], self->adc_count[0]);
    int32_t mean1 = adc_mean_to_q31(self->adc_sum[1], self->adc_count[1]);
    cal_disarm_stream(self);
    if (self->sweep_pass == 0) {
        cal_set_offset(self->record,
            0 * CAL_POINTS_PER_CURRENT_CHAN + idx, mean0);
        cal_set_offset(self->record,
            1 * CAL_POINTS_PER_CURRENT_CHAN + idx, mean1);
        self->sweep_pass = 1;
        char msg[80];
        tfp_snprintf(msg, sizeof(msg),
            "i_sel=%u mux=%u adc0=%d adc1=%d",
            (unsigned) self->sweep_i_select, (unsigned) self->sweep_i_mux,
            (int) mean0, (int) mean1);
        cal_send_status(self, cal_current_pct(self), msg);
        cal_current_begin_pass(self);
        return;
    }
    cal_set_offset(self->record,
        2 * CAL_POINTS_PER_CURRENT_CHAN + idx, mean0);
    {
        char msg[80];
        tfp_snprintf(msg, sizeof(msg),
            "i_sel=%u mux=%u adc2=%d",
            (unsigned) self->sweep_i_select, (unsigned) self->sweep_i_mux,
            (int) mean0);
        cal_send_status(self, cal_current_pct(self), msg);
    }

    // Advance to the next sweep point or finish the sweep.
    uint8_t next_i = 0, next_mux = 0;
    if (!next_sweep_point(self->sweep_i_select, self->sweep_i_mux,
                          &next_i, &next_mux)) {
        cal_stamp_signature_and_check(self->record);
        cal_begin_erase_active(self);
        return;
    }
    self->sweep_i_select = next_i;
    self->sweep_i_mux = next_mux;
    self->sweep_pass = 0;
    cal_current_arm_mux(self);
    cal_arm_settle(self);
}

static void cal_current_after_active_read(struct js320_cal_s * self) {
    if (cal_validate(self->record)) {
        cal_finish(self, JSDRV_ERROR_MESSAGE_INTEGRITY);
        return;
    }
    cal_snapshot_prior(self);
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/i/range/mode",
        &jsdrv_union_u32(5));
    self->sweep_i_select = 0;
    self->sweep_i_mux = 0;
    self->sweep_pass = 0;
    cal_current_arm_mux(self);
    cal_send_status(self, 100, "sweeping current offsets");
    cal_arm_settle(self);
}


// --- Voltage cal step ---

static void cal_voltage_begin_capture(struct js320_cal_s * self) {
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/v/range/select",
        &jsdrv_union_u32(self->voltage_v_select));
    cal_arm_stream(self, 3, -1);   // ADC3 = voltage
    cal_begin_capture(self, true, false);
    self->state = CAL_SWEEP_CAPTURE;
}

static void cal_voltage_after_capture(struct js320_cal_s * self) {
    int32_t mean = adc_mean_to_q31(self->adc_sum[0], self->adc_count[0]);
    cal_disarm_stream(self);
    cal_set_offset(self->record, CAL_VOLTAGE_IDX + self->voltage_v_select,
                   mean);
    char msg[64];
    tfp_snprintf(msg, sizeof(msg), "v_sel=%u adc3=%d",
                 (unsigned) self->voltage_v_select, (int) mean);
    cal_send_status(self,
                    (uint16_t)(100U + 400U * (self->voltage_v_select + 1U)),
                    msg);
    if (self->voltage_v_select == 0) {
        self->voltage_v_select = 1;
        cal_arm_settle(self);
        return;
    }
    cal_stamp_signature_and_check(self->record);
    cal_begin_erase_active(self);
}

static void cal_voltage_after_active_read(struct js320_cal_s * self) {
    if (cal_validate(self->record)) {
        cal_finish(self, JSDRV_ERROR_MESSAGE_INTEGRITY);
        return;
    }
    cal_snapshot_prior(self);
    jsdrvp_mb_dev_publish_to_device(self->dev, "s/v/range/mode",
        &jsdrv_union_u32(1));
    self->voltage_v_select = 0;
    cal_send_status(self, 100, "sweeping voltage offsets");
    cal_arm_settle(self);
}


// --- slot_copy policy ---

static int32_t slot_copy_policy(uint8_t src, uint8_t dst) {
    if (src >= JSDRV_CAL_SLOT_COUNT || dst >= JSDRV_CAL_SLOT_COUNT) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (src == dst) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (dst == JSDRV_CAL_SLOT_ACTIVE) {
        return 0;
    }
    if (dst == JSDRV_CAL_SLOT_TRIM1 || dst == JSDRV_CAL_SLOT_TRIM2) {
        return (src == JSDRV_CAL_SLOT_ACTIVE) ? 0
            : JSDRV_ERROR_PARAMETER_INVALID;
    }
    return JSDRV_ERROR_PARAMETER_INVALID;
}


// --- Mem-response dispatch by state ---

static void on_read_done(struct js320_cal_s * self,
                         const uint8_t * data, uint32_t data_size) {
    if (data_size < FLASH_PAGE_SIZE) {
        cal_finish(self, JSDRV_ERROR_IO);
        return;
    }
    memcpy(self->record + self->page_idx * FLASH_PAGE_SIZE,
           data, FLASH_PAGE_SIZE);
    self->page_idx += 1;
    if (self->page_idx < self->page_count) {
        cal_send_read_page(self);
        return;
    }

    switch (self->op) {
        case JSDRV_CAL_OP_SLOT_READ:
            jsdrvp_mb_dev_send_to_frontend(self->dev, "h/cal/!data",
                &jsdrv_union_cbin_r(self->record, JSDRV_CAL_RECORD_SIZE));
            cal_finish(self, 0);
            break;

        case JSDRV_CAL_OP_SLOT_COPY:
            if (0 != memcmp(self->record, CAL_HEADER_MAGIC,
                            sizeof(CAL_HEADER_MAGIC))) {
                cal_finish(self, JSDRV_ERROR_MESSAGE_INTEGRITY);
                return;
            }
            self->flash_offset_base = slot_to_offset(self->dst_slot);
            self->state = CAL_ERASE;
            cal_send_status(self, 500, "erasing destination");
            cal_send_erase(self);
            break;

        case JSDRV_CAL_OP_CURRENT_OFFSET:
            cal_current_after_active_read(self);
            break;

        case JSDRV_CAL_OP_VOLTAGE_OFFSET:
            cal_voltage_after_active_read(self);
            break;

        default:
            cal_finish(self, JSDRV_ERROR_UNSPECIFIED);
            break;
    }
}

static void on_erase_done(struct js320_cal_s * self) {
    cal_begin_write(self);
}

static void on_write_done(struct js320_cal_s * self) {
    self->page_idx += 1;
    if (self->page_idx < self->page_count) {
        cal_send_write_page(self);
        return;
    }
    cal_begin_verify(self);
}

static void on_verify_done(struct js320_cal_s * self,
                           uint32_t rsp_offset,
                           const uint8_t * data, uint32_t data_size) {
    if (data_size < FLASH_PAGE_SIZE) {
        cal_finish(self, JSDRV_ERROR_IO);
        return;
    }
    uint32_t page = (rsp_offset - self->flash_offset_base) / FLASH_PAGE_SIZE;
    const uint8_t * expected = self->record + page * FLASH_PAGE_SIZE;
    if (0 != memcmp(data, expected, FLASH_PAGE_SIZE)) {
        cal_finish(self, JSDRV_ERROR_IO);
        return;
    }
    self->page_idx += 1;
    if (self->page_idx < self->page_count) {
        cal_send_read_page(self);
        return;
    }
    cal_finish(self, 0);
}

static void on_mem_response(struct js320_cal_s * self,
                            const struct jsdrv_union_s * value) {
    uint32_t hdr_offset = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_offset = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_offset + sizeof(struct mb_stdmsg_mem_s)) {
        cal_finish(self, JSDRV_ERROR_MESSAGE_INTEGRITY);
        return;
    }
    const struct mb_stdmsg_mem_s * rsp =
        (const struct mb_stdmsg_mem_s *) (value->value.bin + hdr_offset);
    if (rsp->transaction_id != self->mem_cmd_id) {
        return;  // Not ours; could be another subsystem's response.
    }
    if (rsp->status != 0) {
        cal_finish(self, JSDRV_ERROR_IO);
        return;
    }
    const uint8_t * data = value->value.bin + hdr_offset + sizeof(*rsp);
    uint32_t data_size = value->size - hdr_offset - sizeof(*rsp);

    switch (self->state) {
        case CAL_READ:
            on_read_done(self, data, data_size);
            break;
        case CAL_ERASE:
            on_erase_done(self);
            break;
        case CAL_WRITE:
            on_write_done(self);
            break;
        case CAL_VERIFY:
            on_verify_done(self, rsp->offset, data, data_size);
            break;
        default:
            break;
    }
}


// --- ADC sample accumulator ---

static void on_adc_data(struct js320_cal_s * self, int slot,
                        const struct jsdrv_union_s * value) {
    if (self->state != CAL_SWEEP_CAPTURE) {
        return;
    }
    if (self->adc_need[slot] == 0) {
        return;
    }
    if (value->type != JSDRV_UNION_BIN
            || value->size < JSDRV_STREAM_HEADER_SIZE) {
        return;
    }
    const struct jsdrv_stream_signal_s * sig =
        (const struct jsdrv_stream_signal_s *) value->value.bin;
    if (sig->version != 1 || sig->element_size_bits != 32) {
        return;
    }
    uint32_t have = self->adc_count[slot];
    uint32_t need = self->adc_need[slot];
    if (have >= need) {
        return;
    }
    uint32_t n = sig->element_count;
    if (n > need - have) {
        n = need - have;
    }
    const int32_t * samples = (const int32_t *) &sig->data[0];
    int64_t s = self->adc_sum[slot];
    for (uint32_t i = 0; i < n; ++i) {
        s += samples[i];
    }
    self->adc_sum[slot] = s;
    self->adc_count[slot] = have + n;

    if (!capture_complete(self)) {
        return;
    }
    if (self->op == JSDRV_CAL_OP_CURRENT_OFFSET) {
        cal_current_after_capture(self);
    } else {
        cal_voltage_after_capture(self);
    }
}


// --- Command entry ---

static void cal_start(struct js320_cal_s * self,
                      const struct jsdrv_cal_cmd_s * cmd) {
    self->transaction_id = cmd->transaction_id;
    self->op = cmd->op;
    self->src_slot = cmd->src_slot;
    self->dst_slot = cmd->dst_slot;
    self->samples_per_point = cmd->samples_per_point;
    self->mem_cmd_id = 0;
    self->restore_pending = false;

    switch (cmd->op) {
        case JSDRV_CAL_OP_SLOT_READ:
            if (cmd->src_slot >= JSDRV_CAL_SLOT_COUNT) {
                cal_finish(self, JSDRV_ERROR_PARAMETER_INVALID);
                return;
            }
            cal_begin_read(self, cmd->src_slot, "reading slot");
            break;

        case JSDRV_CAL_OP_SLOT_COPY: {
            int32_t rc = slot_copy_policy(cmd->src_slot, cmd->dst_slot);
            if (rc) {
                cal_finish(self, rc);
                return;
            }
            cal_begin_read(self, cmd->src_slot, "reading source slot");
            break;
        }

        case JSDRV_CAL_OP_CURRENT_OFFSET:
            cal_begin_read(self, JSDRV_CAL_SLOT_ACTIVE, "reading ACTIVE");
            break;

        case JSDRV_CAL_OP_VOLTAGE_OFFSET:
            cal_begin_read(self, JSDRV_CAL_SLOT_ACTIVE, "reading ACTIVE");
            break;

        default:
            cal_finish(self, JSDRV_ERROR_PARAMETER_INVALID);
            break;
    }
}


// --- Public API ---

struct js320_cal_s * js320_cal_alloc(void) {
    struct js320_cal_s * self = jsdrv_alloc_clr(sizeof(struct js320_cal_s));
    self->op = 0xFF;
    return self;
}

void js320_cal_free(struct js320_cal_s * cal) {
    jsdrv_free(cal);
}

void js320_cal_on_open(struct js320_cal_s * cal, struct jsdrvp_mb_dev_s * dev) {
    cal->dev = dev;
    cal->state = CAL_IDLE;
    cal->op = 0xFF;
    cal->restore_pending = false;
    memset(cal->saved_known, 0, sizeof(cal->saved_known));
    memset(cal->prior_known, 0, sizeof(cal->prior_known));
}

void js320_cal_on_close(struct js320_cal_s * cal) {
    if (cal->state != CAL_IDLE) {
        JSDRV_LOGW("cal: device closed during %s, aborting",
                   op_name(cal->op));
        jsdrvp_mb_dev_set_timeout(cal->dev, 0);
        cal->state = CAL_IDLE;
        cal->op = 0xFF;
        cal->restore_pending = false;
    }
    cal->dev = NULL;
}

bool js320_cal_handle_cmd(struct js320_cal_s * cal,
                          const char * subtopic,
                          const struct jsdrv_union_s * value) {
    // Track host-set values for save/restore.
    int idx = save_index(subtopic);
    if (idx >= 0) {
        uint32_t v;
        if (union_as_u32(value, &v)) {
            cal->saved_value[idx] = v;
            cal->saved_known[idx] = 1;
        }
        return false;  // observe only; let mb_device handle the publish
    }

    if (0 != strcmp(subtopic, "h/cal/!cmd")) {
        return false;
    }
    if (cal->state != CAL_IDLE) {
        struct jsdrv_cal_rsp_s rsp;
        rsp.transaction_id = 0;
        rsp.status = JSDRV_ERROR_BUSY;
        if (value->type == JSDRV_UNION_BIN
                && value->size >= sizeof(uint32_t)) {
            rsp.transaction_id = *((const uint32_t *) value->value.bin);
        }
        jsdrvp_mb_dev_send_to_frontend(cal->dev, "h/cal/!rsp",
            &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
        return true;
    }
    if (value->type != JSDRV_UNION_BIN
            || value->size < sizeof(struct jsdrv_cal_cmd_s)) {
        struct jsdrv_cal_rsp_s rsp = {0, JSDRV_ERROR_PARAMETER_INVALID};
        jsdrvp_mb_dev_send_to_frontend(cal->dev, "h/cal/!rsp",
            &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
        return true;
    }
    cal_start(cal, (const struct jsdrv_cal_cmd_s *) value->value.bin);
    return true;
}

bool js320_cal_handle_publish(struct js320_cal_s * cal,
                              const char * subtopic,
                              const struct jsdrv_union_s * value) {
    // Track device-published values for save/restore.
    int idx = save_index(subtopic);
    if (idx >= 0) {
        uint32_t v;
        if (union_as_u32(value, &v)) {
            cal->saved_value[idx] = v;
            cal->saved_known[idx] = 1;
        }
        return false;
    }

    if (cal->state == CAL_IDLE) {
        return false;
    }
    if (0 == strcmp(subtopic, "h/!rsp")) {
        on_mem_response(cal, value);
        return true;
    }
    return false;
}

void js320_cal_on_timeout(struct js320_cal_s * cal) {
    if (cal->state != CAL_SWEEP_SETTLE) {
        return;
    }
    if (cal->op == JSDRV_CAL_OP_CURRENT_OFFSET) {
        cal_current_begin_pass(cal);
    } else if (cal->op == JSDRV_CAL_OP_VOLTAGE_OFFSET) {
        cal_voltage_begin_capture(cal);
    }
}
