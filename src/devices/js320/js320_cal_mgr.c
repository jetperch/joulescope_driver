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
#include "jsdrv_prv/devices/js320/js320_cal_mgr.h"
#include "jsdrv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include "jsdrv/os_event.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/time.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/check32.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/pubsub.h"
#include "mb/stdmsg.h"
#include "tinyprintf.h"
#include <stdlib.h>
#include <string.h>

JSDRV_STATIC_ASSERT(sizeof(struct jsdrv_cal_add_header_s) == 40, cal_add_hdr_size);
JSDRV_STATIC_ASSERT(sizeof(struct jsdrv_cal_add_rsp_s) == 8, cal_add_rsp_size);


// =====================================================================
// Slot -> flash offset
// =====================================================================

#define FLASH_BLOCK_SHIFT  16U  // 64 KiB blocks

// JS320 fpga_mcu flash block assignments for calibration slots.
// Mirrors firmware/fpga_mcu/src/flash.c FLASH_BLOCK_CAL_*.
static const uint8_t SLOT_TO_BLOCK[JSDRV_CAL_SLOT_COUNT] = {
    [JSDRV_CAL_SLOT_ACTIVE]  = 24,
    [JSDRV_CAL_SLOT_TRIM2]   = 25,
    [JSDRV_CAL_SLOT_TRIM1]   = 26,
    [JSDRV_CAL_SLOT_FIELD]   = 27,
    [JSDRV_CAL_SLOT_LAB]     = 28,
    [JSDRV_CAL_SLOT_FACTORY] = 29,
};

#define CAL_MEM_TOPIC      "s/flash/!cmd"
#define CAL_MEM_PAGE_SIZE  256U
#define MEM_OP_TIMEOUT_MS  2000U
#define MEM_OP_WAIT_MS     5000U
#define ERASE_TIMEOUT_MS   10000U
#define ERASE_WAIT_MS      30000U

// Default samples per cal point: 6 power-line cycles at 1 MHz = 100k.
// Matches joulescope_mfg/js320/pcts/calibrate_current.py.
#define DEFAULT_SAMPLES_PER_POINT   100000U
#define SAMPLE_CAPTURE_TIMEOUT_MS   5000U

// Calibration record layout (struct js320_calibration_s).
#define CAL_OFFSET_HEADER           0
#define CAL_OFFSET_CREATE_TIME      32
#define CAL_OFFSET_OFFSETS          88     // int32_t offsets[72]
#define CAL_OFFSET_GAINS            (CAL_OFFSET_OFFSETS + 72 * 4)
#define CAL_OFFSET_SIGNATURE        952
#define CAL_OFFSET_SIGNATURE_SIZE   64
#define CAL_OFFSET_CHECK32          1020   // last 4 bytes
#define CAL_POINTS_PER_CURRENT_CHAN 24     // indices reserved per channel
#define CAL_VOLTAGE_IDX             70     // offsets[70] and offsets[71]
#define CAL_CURRENT_CHANNEL_COUNT   3

// (i_select, i_mux_select) -> linear point index within a channel.
// Upper-triangular layout per calibration.h:60-77.
// Indexed by i_select * 6 + i_mux_select; entries where i_mux_select <
// i_select are 0xFF (unused).
static const uint8_t CAL_INDEX_MAP[36] = {
    /* i_select=0 */ 0,  1,  2,  3,  4,  5,
    /* i_select=1 */ 0xFF, 6,  7,  8,  9, 10,
    /* i_select=2 */ 0xFF, 0xFF, 11, 12, 13, 14,
    /* i_select=3 */ 0xFF, 0xFF, 0xFF, 15, 16, 17,
    /* i_select=4 */ 0xFF, 0xFF, 0xFF, 0xFF, 18, 19,
    /* i_select=5 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 20,
};

static uint32_t slot_to_offset(uint8_t slot) {
    if (slot >= JSDRV_CAL_SLOT_COUNT) {
        return 0;
    }
    return ((uint32_t) SLOT_TO_BLOCK[slot]) << FLASH_BLOCK_SHIFT;
}

static const char * slot_name(uint8_t slot) {
    switch (slot) {
        case JSDRV_CAL_SLOT_ACTIVE:  return "ACTIVE";
        case JSDRV_CAL_SLOT_TRIM2:   return "TRIM2";
        case JSDRV_CAL_SLOT_TRIM1:   return "TRIM1";
        case JSDRV_CAL_SLOT_FIELD:   return "FIELD";
        case JSDRV_CAL_SLOT_LAB:     return "LAB";
        case JSDRV_CAL_SLOT_FACTORY: return "FACTORY";
        default:                     return "?";
    }
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


// =====================================================================
// JSON helper (copied from js320_fwup_mgr.c)
// =====================================================================

static uint32_t json_escape(char * dst, uint32_t dst_size, const char * src) {
    uint32_t pos = 0;
    for (; *src && pos + 2 < dst_size; ++src) {
        if (*src == '"' || *src == '\\') {
            dst[pos++] = '\\';
        }
        dst[pos++] = *src;
    }
    dst[pos] = '\0';
    return pos;
}


// =====================================================================
// Worker
// =====================================================================

enum worker_state_e {
    WST_INIT,
    WST_OPEN,
    WST_READ_SLOT,
    WST_SWEEP,
    WST_WRITE_SLOT,
    WST_DONE,
    WST_ERROR,
};

static const char * worker_state_name(enum worker_state_e st) {
    switch (st) {
        case WST_INIT:       return "INIT";
        case WST_OPEN:       return "OPEN";
        case WST_READ_SLOT:  return "READ_SLOT";
        case WST_SWEEP:      return "SWEEP";
        case WST_WRITE_SLOT: return "WRITE_SLOT";
        case WST_DONE:       return "DONE";
        case WST_ERROR:      return "ERROR";
        default:             return "?";
    }
}

struct worker_s {
    uint32_t id;
    volatile bool done;
    volatile bool do_exit;
    int32_t result;
    enum worker_state_e state;
    uint16_t pct;                       ///< Overall progress in tenths (0-1000).

    struct jsdrv_context_s * context;
    char device_prefix[JSDRV_TOPIC_LENGTH_MAX];
    char status_topic[JSDRV_TOPIC_LENGTH_MAX];
    char data_topic[JSDRV_TOPIC_LENGTH_MAX];

    // !add header fields
    uint8_t op;
    uint8_t src_slot;
    uint8_t dst_slot;
    uint32_t samples_per_point;

    jsdrv_os_event_t event;
    volatile int32_t rsp_status;

    // For mem READ ops, on_mem_rsp copies data here.
    uint8_t * rsp_read_buf;
    uint32_t rsp_read_max;
    volatile uint32_t rsp_read_actual;

    // ADC sample accumulators.  Two slots match the two stream sources
    // s/adc/0/!data and s/adc/1/!data.  on_adc_data adds incoming
    // 32-bit samples to sum[i] and increments count[i] up to need[i].
    // need[i] = 0 means "slot not in use; ignore".
    volatile int64_t  adc_sum[2];
    volatile uint32_t adc_count[2];
    uint32_t          adc_need[2];

    // Calibration record buffer (1024 bytes).
    uint8_t record[JSDRV_CAL_RECORD_SIZE];

    jsdrv_thread_t thread;
};

static bool worker_cancelled(struct worker_s * w) {
    return w->do_exit;
}

static void worker_publish_status(struct worker_s * w, const char * msg) {
    char escaped[160];
    char buf[256];
    json_escape(escaped, sizeof(escaped), msg);
    uint32_t pct_whole = w->pct / 10;
    uint32_t pct_frac = w->pct % 10;
    tfp_snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"pct\":%u.%u,\"msg\":\"%s\",\"rc\":%d,\"op\":\"%s\"}",
        worker_state_name(w->state),
        pct_whole, pct_frac,
        escaped, (int) w->result, op_name(w->op));
    JSDRV_LOGI("cal/%03u: %s %u.%u%% - %s",
               w->id, worker_state_name(w->state),
               pct_whole, pct_frac, msg);
    jsdrv_publish(w->context, w->status_topic,
                  &jsdrv_union_cstr_r(buf), 0);
}

static void worker_fail(struct worker_s * w, int32_t rc, const char * msg) {
    w->result = rc;
    w->state = WST_ERROR;
    worker_publish_status(w, msg);
}

static void on_mem_rsp(void * user_data, const char * topic,
                       const struct jsdrv_union_s * value) {
    struct worker_s * w = (struct worker_s *) user_data;
    (void) topic;
    uint32_t hdr_off = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_off = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_off + sizeof(struct mb_stdmsg_mem_s)) {
        w->rsp_status = JSDRV_ERROR_MESSAGE_INTEGRITY;
        jsdrv_os_event_signal(w->event);
        return;
    }
    const struct mb_stdmsg_mem_s * rsp =
        (const struct mb_stdmsg_mem_s *)(value->value.bin + hdr_off);
    w->rsp_status = rsp->status;

    // Capture read data when requested and the response carries payload.
    if (rsp->status == 0
            && rsp->operation == MB_STDMSG_MEM_OP_READ
            && w->rsp_read_buf
            && w->rsp_read_max > 0) {
        uint32_t avail = value->size - hdr_off - sizeof(*rsp);
        uint32_t want = rsp->length;
        uint32_t copy = (avail < want) ? avail : want;
        if (copy > w->rsp_read_max) {
            copy = w->rsp_read_max;
        }
        if (copy > 0) {
            memcpy(w->rsp_read_buf, rsp->data, copy);
        }
        w->rsp_read_actual = copy;
    }
    jsdrv_os_event_signal(w->event);
}

// Returns true if every requested slot has reached its sample target.
static bool adc_capture_complete(struct worker_s * w) {
    for (uint32_t i = 0; i < 2; ++i) {
        if (w->adc_need[i] && w->adc_count[i] < w->adc_need[i]) {
            return false;
        }
    }
    return true;
}

// Shared helper: accumulate samples from a s/adc/N/!data stream packet
// into adc_sum[slot] / adc_count[slot].  Stops once adc_need[slot]
// samples have been collected.
static void adc_accumulate(struct worker_s * w, uint32_t slot,
                            const struct jsdrv_union_s * value) {
    if (slot >= 2 || w->adc_need[slot] == 0) {
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
    const int32_t * samples = (const int32_t *) &sig->data[0];
    uint32_t n = sig->element_count;
    uint32_t have = w->adc_count[slot];
    if (have >= w->adc_need[slot]) {
        return;
    }
    uint32_t remaining = w->adc_need[slot] - have;
    if (n > remaining) {
        n = remaining;
    }
    int64_t s = (int64_t) w->adc_sum[slot];
    for (uint32_t i = 0; i < n; ++i) {
        s += samples[i];
    }
    w->adc_sum[slot] = s;
    w->adc_count[slot] = have + n;
    if (adc_capture_complete(w)) {
        jsdrv_os_event_signal(w->event);
    }
}

static void on_adc0_data(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    (void) topic;
    adc_accumulate((struct worker_s *) user_data, 0, value);
}

static void on_adc1_data(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    (void) topic;
    adc_accumulate((struct worker_s *) user_data, 1, value);
}

static int32_t worker_open(struct worker_s * w) {
    worker_publish_status(w, "opening device");
    // RESUME (not RAW) is the mode used by minibitty `mem` for plain
    // s/flash/!cmd access — RAW skips the comm bring-up the sensor flash
    // subsystem relies on.
    int32_t rc = jsdrv_open(w->context, w->device_prefix,
                            JSDRV_DEVICE_OPEN_MODE_RESUME, 10000);
    if (rc) {
        worker_fail(w, rc, "device open failed");
        return rc;
    }
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_subscribe(w->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_mem_rsp, w, JSDRV_TIMEOUT_MS_DEFAULT);
    jsdrv_thread_sleep_ms(500);
    return 0;
}

static void worker_close(struct worker_s * w) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_unsubscribe(w->context, topic.topic, on_mem_rsp, w, 0);
    jsdrv_close(w->context, w->device_prefix, JSDRV_TIMEOUT_MS_DEFAULT);
}

/// Send a memory operation and wait for completion.
/// For READ, dst (if non-NULL) receives the payload (up to length bytes).
static int32_t worker_mem_op(struct worker_s * w, uint32_t txn_id,
                             uint8_t operation, uint32_t offset,
                             uint32_t length,
                             const uint8_t * write_data, uint32_t write_size,
                             uint8_t * read_dst, uint32_t read_dst_size) {
    uint32_t payload_size = sizeof(struct mb_stdmsg_header_s)
                          + sizeof(struct mb_stdmsg_mem_s) + write_size;
    uint8_t * buf = (uint8_t *) malloc(payload_size);
    if (!buf) return JSDRV_ERROR_NOT_ENOUGH_MEMORY;

    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;

    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    memset(cmd, 0, sizeof(*cmd));
    cmd->transaction_id = txn_id;
    cmd->operation = operation;
    cmd->offset = offset;
    cmd->length = length;
    cmd->timeout_ms = (operation == MB_STDMSG_MEM_OP_ERASE)
                        ? ERASE_TIMEOUT_MS : MEM_OP_TIMEOUT_MS;
    if (write_data && write_size) {
        memcpy(cmd->data, write_data, write_size);
    }

    w->rsp_status = JSDRV_ERROR_TIMED_OUT;
    w->rsp_read_buf = read_dst;
    w->rsp_read_max = read_dst_size;
    w->rsp_read_actual = 0;
    jsdrv_os_event_reset(w->event);

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, CAL_MEM_TOPIC);

    struct jsdrv_union_s value;
    value.type = JSDRV_UNION_STDMSG;
    value.value.bin = buf;
    value.size = payload_size;
    value.flags = 0;
    value.app = 0;
    value.op = 0;

    int32_t rc = jsdrv_publish(w->context, topic.topic, &value, 0);
    free(buf);
    if (rc) {
        w->rsp_read_buf = NULL;
        return rc;
    }

    uint32_t timeout = (operation == MB_STDMSG_MEM_OP_ERASE)
                        ? ERASE_WAIT_MS : MEM_OP_WAIT_MS;
    rc = jsdrv_os_event_wait(w->event, timeout);
    w->rsp_read_buf = NULL;
    if (rc) return JSDRV_ERROR_TIMED_OUT;
    return w->rsp_status;
}

/// Read length bytes from flash at offset into dst, in CAL_MEM_PAGE_SIZE chunks.
static int32_t worker_flash_read(struct worker_s * w, uint32_t txn_base,
                                  uint32_t offset, uint32_t length,
                                  uint8_t * dst) {
    uint32_t done = 0;
    while (done < length && !worker_cancelled(w)) {
        uint32_t chunk = length - done;
        if (chunk > CAL_MEM_PAGE_SIZE) {
            chunk = CAL_MEM_PAGE_SIZE;
        }
        int32_t rc = worker_mem_op(w, txn_base + done, MB_STDMSG_MEM_OP_READ,
                                    offset + done, chunk,
                                    NULL, 0, dst + done, chunk);
        if (rc) return rc;
        if (w->rsp_read_actual != chunk) {
            return JSDRV_ERROR_IO;
        }
        done += chunk;
    }
    if (worker_cancelled(w)) return JSDRV_ERROR_ABORTED;
    return 0;
}

/// Erase the 64 KiB block at block_offset, then write src (length bytes,
/// padded to a page with 0xFF), then read back and compare.
/// Used to write a 1024-byte cal record into a slot's first page region.
static int32_t worker_flash_write_verify(struct worker_s * w,
                                          uint32_t txn_base,
                                          uint32_t block_offset,
                                          const uint8_t * src,
                                          uint32_t length) {
    if ((block_offset & 0xFFFFU) != 0) {
        // Erase must be 64 KiB block aligned.
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    int32_t rc = worker_mem_op(w, txn_base, MB_STDMSG_MEM_OP_ERASE,
                                block_offset, 65536U, NULL, 0, NULL, 0);
    if (rc) return rc;

    // Write page-by-page; pad final partial page with 0xFF.
    uint32_t done = 0;
    uint8_t page_buf[CAL_MEM_PAGE_SIZE];
    uint32_t page_num = 1;
    while (done < length && !worker_cancelled(w)) {
        uint32_t chunk = length - done;
        memset(page_buf, 0xFF, CAL_MEM_PAGE_SIZE);
        if (chunk > CAL_MEM_PAGE_SIZE) {
            chunk = CAL_MEM_PAGE_SIZE;
        }
        memcpy(page_buf, src + done, chunk);
        rc = worker_mem_op(w, txn_base + page_num,
                            MB_STDMSG_MEM_OP_WRITE,
                            block_offset + done, CAL_MEM_PAGE_SIZE,
                            page_buf, CAL_MEM_PAGE_SIZE, NULL, 0);
        if (rc) return rc;
        done += chunk;
        ++page_num;
    }
    if (worker_cancelled(w)) return JSDRV_ERROR_ABORTED;

    // Read back and verify.
    uint8_t verify_buf[JSDRV_CAL_RECORD_SIZE];
    if (length > JSDRV_CAL_RECORD_SIZE) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    rc = worker_flash_read(w, txn_base + 1000, block_offset, length,
                            verify_buf);
    if (rc) return rc;
    if (memcmp(verify_buf, src, length) != 0) {
        return JSDRV_ERROR_IO;
    }
    return 0;
}

// Convenience: publish a u32 to <device_prefix>/<subtopic>.
static int32_t worker_publish_u32(struct worker_s * w,
                                   const char * subtopic, uint32_t v) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, subtopic);
    return jsdrv_publish(w->context, topic.topic,
                         &jsdrv_union_u32(v), JSDRV_TIMEOUT_MS_DEFAULT);
}

// Subscribe / unsubscribe to a stream topic under the device prefix.
static int32_t worker_subscribe(struct worker_s * w, const char * subtopic,
                                 jsdrv_subscribe_fn fn) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, subtopic);
    return jsdrv_subscribe(w->context, topic.topic, JSDRV_SFLAG_PUB,
                           fn, w, JSDRV_TIMEOUT_MS_DEFAULT);
}

static void worker_unsubscribe(struct worker_s * w, const char * subtopic,
                                jsdrv_subscribe_fn fn) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, subtopic);
    jsdrv_unsubscribe(w->context, topic.topic, fn, w, 0);
}

// Capture `samples` samples from each adc slot whose adc_need is set.
// Caller must have configured s/adc/{0,1}/sel and !ctrl as needed and
// must subscribe to /!data via worker_subscribe before calling.
// Returns the mean per active slot in mean[0] / mean[1] (0 if not used).
static int32_t worker_capture_means(struct worker_s * w, uint32_t samples,
                                     bool use_slot0, bool use_slot1,
                                     int32_t * out_mean0,
                                     int32_t * out_mean1) {
    w->adc_sum[0] = 0;
    w->adc_sum[1] = 0;
    w->adc_count[0] = 0;
    w->adc_count[1] = 0;
    w->adc_need[0] = use_slot0 ? samples : 0;
    w->adc_need[1] = use_slot1 ? samples : 0;
    jsdrv_os_event_reset(w->event);

    // Wait up to SAMPLE_CAPTURE_TIMEOUT_MS for the target sample count.
    int32_t rc = jsdrv_os_event_wait(w->event, SAMPLE_CAPTURE_TIMEOUT_MS);
    if (rc) {
        return JSDRV_ERROR_TIMED_OUT;
    }
    if (use_slot0) {
        if (w->adc_count[0] == 0) return JSDRV_ERROR_IO;
        *out_mean0 = (int32_t)(w->adc_sum[0] / (int64_t) w->adc_count[0]);
    } else {
        *out_mean0 = 0;
    }
    if (use_slot1) {
        if (w->adc_count[1] == 0) return JSDRV_ERROR_IO;
        *out_mean1 = (int32_t)(w->adc_sum[1] / (int64_t) w->adc_count[1]);
    } else {
        *out_mean1 = 0;
    }
    return 0;
}

/// Policy check for slot_copy: returns 0 if allowed, error otherwise.
///
/// Allowed transitions:
///   dst=ACTIVE: src in {TRIM2, TRIM1, FIELD, LAB, FACTORY}
///   dst=TRIM1:  src=ACTIVE
///   dst=TRIM2:  src=ACTIVE
static int32_t slot_copy_policy(uint8_t src, uint8_t dst) {
    if (src >= JSDRV_CAL_SLOT_COUNT || dst >= JSDRV_CAL_SLOT_COUNT) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (src == dst) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (dst == JSDRV_CAL_SLOT_ACTIVE) {
        // any non-ACTIVE source allowed
        return 0;
    }
    if (dst == JSDRV_CAL_SLOT_TRIM1 || dst == JSDRV_CAL_SLOT_TRIM2) {
        return (src == JSDRV_CAL_SLOT_ACTIVE) ? 0
            : JSDRV_ERROR_PARAMETER_INVALID;
    }
    // FIELD, LAB, FACTORY destinations are not allowed.
    return JSDRV_ERROR_PARAMETER_INVALID;
}


// =====================================================================
// Worker ops
// =====================================================================

static int32_t op_slot_read(struct worker_s * w) {
    if (w->src_slot >= JSDRV_CAL_SLOT_COUNT) {
        worker_fail(w, JSDRV_ERROR_PARAMETER_INVALID, "bad src_slot");
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    w->state = WST_READ_SLOT;
    w->pct = 100;
    char msg[80];
    tfp_snprintf(msg, sizeof(msg), "reading slot %s", slot_name(w->src_slot));
    worker_publish_status(w, msg);

    uint32_t offset = slot_to_offset(w->src_slot);
    int32_t rc = worker_flash_read(w, 1, offset, JSDRV_CAL_RECORD_SIZE,
                                    w->record);
    if (rc) {
        worker_fail(w, rc, "flash read failed");
        return rc;
    }

    // Publish the 1024-byte record on cal/NNN/data.
    w->pct = 950;
    jsdrv_publish(w->context, w->data_topic,
                  &jsdrv_union_cbin_r(w->record, JSDRV_CAL_RECORD_SIZE), 0);
    return 0;
}

// Pack a u32 little-endian into a byte buffer.
static void pack_u32_le(uint8_t * dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
    dst[2] = (uint8_t)((v >> 16) & 0xff);
    dst[3] = (uint8_t)((v >> 24) & 0xff);
}

static void pack_i64_le(uint8_t * dst, int64_t v) {
    uint64_t u = (uint64_t) v;
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)((u >> (i * 8)) & 0xff);
    }
}

// Write the driver signature pattern into bytes [952..1015] of rec.
// Layout: 16 ASCII magic, then 4-byte jsdrv version u32 LE, then 44 zero.
static void cal_write_signature(uint8_t * rec) {
    uint8_t * sig = rec + CAL_OFFSET_SIGNATURE;
    memset(sig, 0, CAL_OFFSET_SIGNATURE_SIZE);
    memcpy(sig, JSDRV_CAL_SIGNATURE_MAGIC, 16);
    pack_u32_le(sig + 16, JSDRV_VERSION_U32);
}

// Recompute and store the 32-bit check32 over the first 1020 bytes.
static void cal_write_check32(uint8_t * rec) {
    // 1020 / 4 = 255 words.  The record is 4-byte aligned at the buffer
    // start; reinterpret as u32 view for the hash.
    uint32_t check = jsdrv_check32_xxhash((const uint32_t *) rec, 255);
    pack_u32_le(rec + CAL_OFFSET_CHECK32, check);
}

// Update the i64 create_time field with the current jsdrv time.
static void cal_update_create_time(uint8_t * rec) {
    int64_t now = jsdrv_time_utc();
    pack_i64_le(rec + CAL_OFFSET_CREATE_TIME, now);
}

// Validate the header magic + check32 of an in-memory record.
static int32_t cal_validate(const uint8_t * rec) {
    static const uint8_t HEADER_MAGIC[16] = {
        0x4a, 0x53, 0x33, 0x32, 0x30, 0x63, 0x61, 0x6c,
        0x0d, 0x0a, 0x20, 0x0a, 0x1a, 0xb2, 0x1c, 0x00
    };
    if (0 != memcmp(rec, HEADER_MAGIC, 16)) {
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    uint32_t expected =
        (uint32_t)(rec[CAL_OFFSET_CHECK32])
        | ((uint32_t)(rec[CAL_OFFSET_CHECK32 + 1]) << 8)
        | ((uint32_t)(rec[CAL_OFFSET_CHECK32 + 2]) << 16)
        | ((uint32_t)(rec[CAL_OFFSET_CHECK32 + 3]) << 24);
    uint32_t actual = jsdrv_check32_xxhash((const uint32_t *) rec, 255);
    if (expected != actual) {
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

// Write a 1Q31 offset into offsets[idx] (little-endian i32).
static void cal_set_offset(uint8_t * rec, uint32_t idx, int32_t q31) {
    pack_u32_le(rec + CAL_OFFSET_OFFSETS + idx * 4, (uint32_t) q31);
}

// Convert a left-aligned 32-bit ADC mean to a 1Q31 offset.
//
// The FPGA datapath subtracts a 1Q31 offset from a 1Qn normalised
// sample.  The ADC stream is already left-aligned 32-bit, so the raw
// sample is already in 1Q31 representation (sign bit + 31 fraction
// bits).  Therefore the offset is simply the i32 mean.
static int32_t cal_q31_from_adc(int32_t adc_mean) {
    return adc_mean;
}

static int32_t op_slot_copy(struct worker_s * w) {
    int32_t rc = slot_copy_policy(w->src_slot, w->dst_slot);
    if (rc) {
        worker_fail(w, rc, "slot_copy policy rejected src/dst");
        return rc;
    }

    // Read source.
    w->state = WST_READ_SLOT;
    w->pct = 100;
    char msg[120];
    tfp_snprintf(msg, sizeof(msg), "reading source slot %s",
                 slot_name(w->src_slot));
    worker_publish_status(w, msg);
    rc = worker_flash_read(w, 1, slot_to_offset(w->src_slot),
                            JSDRV_CAL_RECORD_SIZE, w->record);
    if (rc) {
        worker_fail(w, rc, "source slot read failed");
        return rc;
    }

    // Sanity-check header magic.  Refuse to copy garbage on top of ACTIVE.
    static const uint8_t HEADER_MAGIC[16] = {
        0x4a, 0x53, 0x33, 0x32, 0x30, 0x63, 0x61, 0x6c,
        0x0d, 0x0a, 0x20, 0x0a, 0x1a, 0xb2, 0x1c, 0x00
    };
    if (0 != memcmp(w->record, HEADER_MAGIC, 16)) {
        worker_fail(w, JSDRV_ERROR_MESSAGE_INTEGRITY,
                    "source slot has bad header magic");
        return JSDRV_ERROR_MESSAGE_INTEGRITY;
    }

    // Erase + write + verify destination.
    w->state = WST_WRITE_SLOT;
    w->pct = 500;
    tfp_snprintf(msg, sizeof(msg), "writing destination slot %s",
                 slot_name(w->dst_slot));
    worker_publish_status(w, msg);
    rc = worker_flash_write_verify(w, 2000, slot_to_offset(w->dst_slot),
                                    w->record, JSDRV_CAL_RECORD_SIZE);
    if (rc) {
        worker_fail(w, rc, "destination slot write/verify failed");
        return rc;
    }
    return 0;
}


// Configure both stream slots for the current sweep capture phase.
// Slot 0 source = src0 (physical ADC 0..3), slot 1 source = src1 or
// disabled when src1 < 0.  Caller is responsible for the !ctrl=0
// before calling.
static int32_t stream_arm(struct worker_s * w, int32_t src0, int32_t src1) {
    int32_t rc = worker_publish_u32(w, "s/adc/0/ctrl", 0);
    if (rc) return rc;
    rc = worker_publish_u32(w, "s/adc/1/ctrl", 0);
    if (rc) return rc;
    if (src0 >= 0) {
        rc = worker_publish_u32(w, "s/adc/0/sel", (uint32_t) src0);
        if (rc) return rc;
    }
    if (src1 >= 0) {
        rc = worker_publish_u32(w, "s/adc/1/sel", (uint32_t) src1);
        if (rc) return rc;
    }
    if (src0 >= 0) {
        rc = worker_publish_u32(w, "s/adc/0/ctrl", 1);
        if (rc) return rc;
    }
    if (src1 >= 0) {
        rc = worker_publish_u32(w, "s/adc/1/ctrl", 1);
        if (rc) return rc;
    }
    return 0;
}

static void stream_disarm(struct worker_s * w) {
    (void) worker_publish_u32(w, "s/adc/0/ctrl", 0);
    (void) worker_publish_u32(w, "s/adc/1/ctrl", 0);
}

static int32_t op_current_offset(struct worker_s * w) {
    int32_t rc;
    uint32_t samples = w->samples_per_point
        ? w->samples_per_point : DEFAULT_SAMPLES_PER_POINT;

    // Read ACTIVE.
    w->state = WST_READ_SLOT;
    w->pct = 50;
    worker_publish_status(w, "reading ACTIVE");
    rc = worker_flash_read(w, 1, slot_to_offset(JSDRV_CAL_SLOT_ACTIVE),
                            JSDRV_CAL_RECORD_SIZE, w->record);
    if (rc) {
        worker_fail(w, rc, "ACTIVE read failed");
        return rc;
    }
    rc = cal_validate(w->record);
    if (rc) {
        worker_fail(w, rc, "ACTIVE record invalid (magic or check32)");
        return rc;
    }

    // Configure manual current ranging.
    w->state = WST_SWEEP;
    w->pct = 100;
    worker_publish_status(w, "configuring manual current ranging");
    rc = worker_publish_u32(w, "s/i/range/mode", 5);  // manual
    if (rc) {
        worker_fail(w, rc, "set s/i/range/mode failed");
        return rc;
    }

    // Subscribe to both stream data topics before arming.
    rc = worker_subscribe(w, "s/adc/0/!data", on_adc0_data);
    if (rc) {
        worker_fail(w, rc, "subscribe s/adc/0/!data failed");
        return rc;
    }
    rc = worker_subscribe(w, "s/adc/1/!data", on_adc1_data);
    if (rc) {
        worker_unsubscribe(w, "s/adc/0/!data", on_adc0_data);
        worker_fail(w, rc, "subscribe s/adc/1/!data failed");
        return rc;
    }

    // 21 points * 2 passes = 42 captures spanning pct 100..900.
    const uint32_t total_captures = 42;
    uint32_t captures_done = 0;
    char msg[128];

    for (uint32_t i_select = 0; i_select < 6; ++i_select) {
        rc = worker_publish_u32(w, "s/i/range/select",
                                 0x80U | (1U << i_select));
        if (rc) { goto restore; }

        for (uint32_t i_mux = i_select; i_mux < 6; ++i_mux) {
            uint8_t idx = CAL_INDEX_MAP[i_select * 6 + i_mux];
            if (idx == 0xFF) continue;

            rc = worker_publish_u32(w, "s/i/i0_sel", i_mux);
            if (rc) goto restore;
            rc = worker_publish_u32(w, "s/i/i1_sel", i_mux);
            if (rc) goto restore;
            rc = worker_publish_u32(w, "s/i/i2_sel", i_mux);
            if (rc) goto restore;

            // Allow mux to settle.
            jsdrv_thread_sleep_ms(20);

            // Pass A: slot0=ADC0, slot1=ADC1.
            rc = stream_arm(w, 0, 1);
            if (rc) goto restore;
            int32_t mean0 = 0;
            int32_t mean1 = 0;
            rc = worker_capture_means(w, samples, true, true,
                                       &mean0, &mean1);
            stream_disarm(w);
            if (rc) {
                tfp_snprintf(msg, sizeof(msg),
                             "captureA i_select=%u i_mux=%u failed",
                             (unsigned) i_select, (unsigned) i_mux);
                worker_fail(w, rc, msg);
                goto restore;
            }
            cal_set_offset(w->record, 0 * CAL_POINTS_PER_CURRENT_CHAN + idx,
                            cal_q31_from_adc(mean0));
            cal_set_offset(w->record, 1 * CAL_POINTS_PER_CURRENT_CHAN + idx,
                            cal_q31_from_adc(mean1));
            ++captures_done;
            w->pct = (uint16_t)(100 + (800 * captures_done) / total_captures);

            // Pass B: slot0=ADC2, slot1 idle.
            rc = stream_arm(w, 2, -1);
            if (rc) goto restore;
            int32_t mean2 = 0;
            int32_t mean1_unused = 0;
            rc = worker_capture_means(w, samples, true, false,
                                       &mean2, &mean1_unused);
            stream_disarm(w);
            if (rc) {
                tfp_snprintf(msg, sizeof(msg),
                             "captureB i_select=%u i_mux=%u failed",
                             (unsigned) i_select, (unsigned) i_mux);
                worker_fail(w, rc, msg);
                goto restore;
            }
            cal_set_offset(w->record, 2 * CAL_POINTS_PER_CURRENT_CHAN + idx,
                            cal_q31_from_adc(mean2));
            ++captures_done;
            w->pct = (uint16_t)(100 + (800 * captures_done) / total_captures);

            tfp_snprintf(msg, sizeof(msg),
                         "i_sel=%u mux=%u adc0=%d adc1=%d adc2=%d",
                         (unsigned) i_select, (unsigned) i_mux,
                         (int) mean0, (int) mean1, (int) mean2);
            worker_publish_status(w, msg);
        }
    }

    // Update metadata, signature, check32.
    cal_update_create_time(w->record);
    cal_write_signature(w->record);
    cal_write_check32(w->record);

    // Write ACTIVE.
    w->state = WST_WRITE_SLOT;
    w->pct = 950;
    worker_publish_status(w, "writing ACTIVE");
    rc = worker_flash_write_verify(w, 3000,
                                    slot_to_offset(JSDRV_CAL_SLOT_ACTIVE),
                                    w->record, JSDRV_CAL_RECORD_SIZE);
    if (rc) {
        worker_fail(w, rc, "ACTIVE write/verify failed");
        goto restore_only;
    }

restore:
    stream_disarm(w);
    worker_unsubscribe(w, "s/adc/0/!data", on_adc0_data);
    worker_unsubscribe(w, "s/adc/1/!data", on_adc1_data);
    // Leave s/i/range/mode in manual; caller can restore policy.  We do
    // not know the prior mode value, and the fpga_mcu boots into the
    // configured default at next link bring-up.
    return rc;

restore_only:
    stream_disarm(w);
    worker_unsubscribe(w, "s/adc/0/!data", on_adc0_data);
    worker_unsubscribe(w, "s/adc/1/!data", on_adc1_data);
    return rc;
}


static int32_t op_voltage_offset(struct worker_s * w) {
    int32_t rc;
    uint32_t samples = w->samples_per_point
        ? w->samples_per_point : DEFAULT_SAMPLES_PER_POINT;

    w->state = WST_READ_SLOT;
    w->pct = 50;
    worker_publish_status(w, "reading ACTIVE");
    rc = worker_flash_read(w, 1, slot_to_offset(JSDRV_CAL_SLOT_ACTIVE),
                            JSDRV_CAL_RECORD_SIZE, w->record);
    if (rc) {
        worker_fail(w, rc, "ACTIVE read failed");
        return rc;
    }
    rc = cal_validate(w->record);
    if (rc) {
        worker_fail(w, rc, "ACTIVE record invalid (magic or check32)");
        return rc;
    }

    w->state = WST_SWEEP;
    w->pct = 100;
    worker_publish_status(w, "configuring manual voltage ranging");
    rc = worker_publish_u32(w, "s/v/range/mode", 1);  // manual
    if (rc) {
        worker_fail(w, rc, "set s/v/range/mode failed");
        return rc;
    }

    // Only slot 0 used for voltage cal.
    rc = worker_subscribe(w, "s/adc/0/!data", on_adc0_data);
    if (rc) {
        worker_fail(w, rc, "subscribe s/adc/0/!data failed");
        return rc;
    }

    char msg[128];
    for (uint32_t v_select = 0; v_select < 2; ++v_select) {
        rc = worker_publish_u32(w, "s/v/range/select", v_select);
        if (rc) {
            worker_fail(w, rc, "set s/v/range/select failed");
            goto restore;
        }
        // Settle and capture.
        jsdrv_thread_sleep_ms(20);
        rc = stream_arm(w, 3 /* ADC3 = voltage */, -1);
        if (rc) {
            worker_fail(w, rc, "stream_arm voltage failed");
            goto restore;
        }
        int32_t mean = 0;
        int32_t unused = 0;
        rc = worker_capture_means(w, samples, true, false, &mean, &unused);
        stream_disarm(w);
        if (rc) {
            tfp_snprintf(msg, sizeof(msg),
                         "voltage capture v_select=%u failed",
                         (unsigned) v_select);
            worker_fail(w, rc, msg);
            goto restore;
        }
        cal_set_offset(w->record, CAL_VOLTAGE_IDX + v_select,
                        cal_q31_from_adc(mean));
        tfp_snprintf(msg, sizeof(msg),
                     "v_select=%u adc3=%d",
                     (unsigned) v_select, (int) mean);
        worker_publish_status(w, msg);
        w->pct = (uint16_t)(100 + (800 * (v_select + 1)) / 2);
    }

    cal_update_create_time(w->record);
    cal_write_signature(w->record);
    cal_write_check32(w->record);

    w->state = WST_WRITE_SLOT;
    w->pct = 950;
    worker_publish_status(w, "writing ACTIVE");
    rc = worker_flash_write_verify(w, 3000,
                                    slot_to_offset(JSDRV_CAL_SLOT_ACTIVE),
                                    w->record, JSDRV_CAL_RECORD_SIZE);
    if (rc) {
        worker_fail(w, rc, "ACTIVE write/verify failed");
    }

restore:
    stream_disarm(w);
    worker_unsubscribe(w, "s/adc/0/!data", on_adc0_data);
    return rc;
}


// =====================================================================
// Worker thread
// =====================================================================

static JSDRV_THREAD_RETURN_TYPE worker_thread(JSDRV_THREAD_ARG_TYPE lpParam) {
    struct worker_s * w = (struct worker_s *) lpParam;
    int32_t rc = JSDRV_ERROR_NOT_SUPPORTED;
    bool device_opened = false;

    JSDRV_LOGI("cal/%03u: started op=%s for %s",
               w->id, op_name(w->op), w->device_prefix);

    w->state = WST_INIT;
    w->pct = 10;
    worker_publish_status(w, "init");
    if (worker_cancelled(w)) { rc = JSDRV_ERROR_ABORTED; goto done; }

    w->state = WST_OPEN;
    w->pct = 50;
    rc = worker_open(w);
    if (rc) goto done;
    device_opened = true;

    if (worker_cancelled(w)) { rc = JSDRV_ERROR_ABORTED; goto close_done; }

    switch (w->op) {
        case JSDRV_CAL_OP_SLOT_READ:
            rc = op_slot_read(w);
            break;
        case JSDRV_CAL_OP_SLOT_COPY:
            rc = op_slot_copy(w);
            break;
        case JSDRV_CAL_OP_CURRENT_OFFSET:
            rc = op_current_offset(w);
            break;
        case JSDRV_CAL_OP_VOLTAGE_OFFSET:
            rc = op_voltage_offset(w);
            break;
        default:
            rc = JSDRV_ERROR_PARAMETER_INVALID;
            worker_fail(w, rc, "unknown op");
            break;
    }

    if (rc == 0) {
        w->state = WST_DONE;
        w->pct = 1000;
        w->result = 0;
    }

close_done:
    if (device_opened) {
        worker_close(w);
        device_opened = false;
    }
    if (w->state == WST_DONE) {
        worker_publish_status(w, "complete");
    }

done:
    JSDRV_LOGI("cal/%03u: done rc=%d", w->id, (int) w->result);
    w->done = true;
    JSDRV_THREAD_RETURN();
}


// =====================================================================
// Manager
// =====================================================================

struct cal_mgr_s {
    struct jsdrv_context_s * context;
    struct worker_s workers[JSDRV_CAL_INSTANCE_MAX];
    uint32_t next_id;
};

static struct cal_mgr_s mgr_;

static uint8_t mgr_on_add(void * user_data, struct jsdrvp_msg_s * msg);

static void mgr_send_to_frontend(const char * topic,
                                 const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(mgr_.context, topic, value);
    jsdrvp_backend_send(mgr_.context, m);
}

static void mgr_publish_list(void) {
    char buf[128];
    char * p = buf;
    bool first = true;
    for (uint32_t i = 0; i < JSDRV_CAL_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context && !w->done) {
            if (!first) { *p++ = ','; }
            if (p + 12 < buf + sizeof(buf)) {
                p += tfp_snprintf(p, (int)(buf + sizeof(buf) - p),
                                  "%03u", w->id);
            }
            first = false;
        }
    }
    *p = '\0';
    mgr_send_to_frontend(JSDRV_CAL_MGR_TOPIC_LIST,
                         &jsdrv_union_cstr_r(buf));
}

static int32_t mgr_send_add_rsp(int32_t rc, uint32_t worker_id) {
    struct jsdrv_cal_add_rsp_s rsp = { .rc = rc, .worker_id = worker_id };
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(mgr_.context, "",
        &jsdrv_union_cbin_r((const uint8_t *) &rsp, sizeof(rsp)));
    tfp_snprintf(m->topic, sizeof(m->topic), "%s%c",
                 JSDRV_CAL_MGR_TOPIC_ADD, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    m->extra.frontend.subscriber.internal_fn = mgr_on_add;
    m->extra.frontend.subscriber.user_data = NULL;
    m->extra.frontend.subscriber.is_internal = 1;
    jsdrvp_backend_send(mgr_.context, m);
    return rc;
}

static void mgr_cleanup_done(void) {
    bool changed = false;
    for (uint32_t i = 0; i < JSDRV_CAL_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context && w->done) {
            jsdrv_thread_join(&w->thread, 5000);
            if (w->event) jsdrv_os_event_free(w->event);
            memset(w, 0, sizeof(*w));
            changed = true;
        }
    }
    if (changed) {
        mgr_publish_list();
    }
}

static int32_t mgr_start_worker(const struct jsdrv_cal_add_header_s * hdr,
                                uint32_t * out_worker_id) {
    mgr_cleanup_done();

    if (hdr->op > JSDRV_CAL_OP_VOLTAGE_OFFSET) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (hdr->op == JSDRV_CAL_OP_SLOT_READ
            && hdr->src_slot >= JSDRV_CAL_SLOT_COUNT) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (hdr->op == JSDRV_CAL_OP_SLOT_COPY
            && (hdr->src_slot >= JSDRV_CAL_SLOT_COUNT
                || hdr->dst_slot >= JSDRV_CAL_SLOT_COUNT)) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }

    struct worker_s * w = NULL;
    for (uint32_t i = 0; i < JSDRV_CAL_INSTANCE_MAX; ++i) {
        if (!mgr_.workers[i].context) {
            w = &mgr_.workers[i];
            break;
        }
    }
    if (!w) return JSDRV_ERROR_BUSY;

    memset(w, 0, sizeof(*w));
    w->id = mgr_.next_id++;
    w->context = mgr_.context;
    w->op = hdr->op;
    w->src_slot = hdr->src_slot;
    w->dst_slot = hdr->dst_slot;
    w->samples_per_point = hdr->samples_per_point;
    jsdrv_cstr_copy(w->device_prefix, hdr->device_prefix,
                    sizeof(w->device_prefix));
    tfp_snprintf(w->status_topic, sizeof(w->status_topic),
                 "cal/%03u/status", w->id);
    tfp_snprintf(w->data_topic, sizeof(w->data_topic),
                 "cal/%03u/data", w->id);

    w->event = jsdrv_os_event_alloc();
    if (jsdrv_thread_create(&w->thread, worker_thread, w, 0)) {
        jsdrv_os_event_free(w->event);
        memset(w, 0, sizeof(*w));
        return JSDRV_ERROR_UNSPECIFIED;
    }

    mgr_publish_list();
    JSDRV_LOGI("cal: started %03u op=%s for %s",
               w->id, op_name(w->op), w->device_prefix);
    if (out_worker_id) {
        *out_worker_id = w->id;
    }
    return 0;
}

static uint8_t mgr_on_add(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    if (msg->value.type != JSDRV_UNION_BIN
            || msg->value.size < sizeof(struct jsdrv_cal_add_header_s)) {
        JSDRV_LOGW("cal: invalid add payload");
        return (uint8_t) mgr_send_add_rsp(JSDRV_ERROR_PARAMETER_INVALID, 0);
    }
    const struct jsdrv_cal_add_header_s * hdr =
        (const struct jsdrv_cal_add_header_s *) msg->value.value.bin;

    uint32_t worker_id = 0;
    int32_t rc = mgr_start_worker(hdr, &worker_id);
    return (uint8_t) mgr_send_add_rsp(rc, (rc == 0) ? worker_id : 0);
}

static void mgr_subscribe(const char * topic, uint8_t flags,
                          jsdrv_pubsub_subscribe_fn fn) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(mgr_.context);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_SUBSCRIBE, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic,
                    sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.internal_fn = fn;
    m->payload.sub.subscriber.user_data = NULL;
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.flags = flags;
    jsdrvp_backend_send(mgr_.context, m);
}

static void mgr_unsubscribe(const char * topic,
                            jsdrv_pubsub_subscribe_fn fn) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(mgr_.context);
    jsdrv_cstr_copy(m->topic, JSDRV_PUBSUB_UNSUBSCRIBE, sizeof(m->topic));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->value.app = JSDRV_PAYLOAD_TYPE_SUB;
    jsdrv_cstr_copy(m->payload.sub.topic, topic,
                    sizeof(m->payload.sub.topic));
    m->payload.sub.subscriber.internal_fn = fn;
    m->payload.sub.subscriber.user_data = NULL;
    m->payload.sub.subscriber.is_internal = 1;
    m->payload.sub.subscriber.flags = JSDRV_SFLAG_PUB;
    jsdrvp_backend_send(mgr_.context, m);
}

int32_t jsdrv_cal_mgr_initialize(struct jsdrv_context_s * context) {
    memset(&mgr_, 0, sizeof(mgr_));
    mgr_.context = context;
    mgr_subscribe(JSDRV_CAL_MGR_TOPIC_ADD, JSDRV_SFLAG_PUB, mgr_on_add);
    mgr_publish_list();
    JSDRV_LOGI("cal manager initialized");
    return 0;
}

void jsdrv_cal_mgr_finalize(void) {
    if (!mgr_.context) return;
    mgr_unsubscribe(JSDRV_CAL_MGR_TOPIC_ADD, mgr_on_add);

    for (uint32_t i = 0; i < JSDRV_CAL_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context && !w->done) {
            w->do_exit = true;
            jsdrv_os_event_signal(w->event);
        }
    }
    for (uint32_t i = 0; i < JSDRV_CAL_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context) {
            jsdrv_thread_join(&w->thread, 15000);
            if (w->event) jsdrv_os_event_free(w->event);
        }
    }
    memset(&mgr_, 0, sizeof(mgr_));
    JSDRV_LOGI("cal manager finalized");
}
