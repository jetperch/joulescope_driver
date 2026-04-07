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
#include "jsdrv_prv/js320_fwup_mgr.h"
#include "jsdrv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include "jsdrv/os_event.h"
#include "jsdrv/os_thread.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/pubsub.h"
#include "mb/stdmsg.h"
#include "miniz.h"
#include "tinyprintf.h"
#include <stdlib.h>
#include <string.h>

JSDRV_STATIC_ASSERT(sizeof(struct jsdrv_fwup_add_header_s) == 40, fwup_add_hdr_size);


// =====================================================================
// Resource table
// =====================================================================

const struct jsdrv_fwup_resource_s JSDRV_FWUP_RESOURCES[] = {
    {"0/0/app/pubsub_metadata.bin", "c/xspi/!cmd",  0x080000, 0x20000},
    {"0/0/app/mbgen.bin",           "c/xspi/!cmd",  0x0A0000, 0x20000},
    {"1/0/app/pubsub_metadata.bin", "s/flash/!cmd",  0x140000, 0x20000},
    {"1/0/app/mbgen.bin",           "s/flash/!cmd",  0x160000, 0x20000},
    {NULL, NULL, 0, 0},
};

#define CTRL_FW_ZIP_PATH    "0/0/app/image.mbfw"
#define FPGA_BIT_ZIP_PATH   "1/image.bit"
#define FW_PAGE_SIZE        256U
#define WORKER_TIMEOUT_MS   120000U
#define RESOURCE_COUNT      4


// =====================================================================
// ZIP extraction
// =====================================================================

int32_t jsdrv_fwup_zip_extract(
        const uint8_t * zip_data, uint32_t zip_size,
        const char * filename,
        uint8_t ** out_data, uint32_t * out_size) {
    if (!zip_data || !zip_size || !filename || !out_data || !out_size) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    *out_data = NULL;
    *out_size = 0;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, zip_size, 0)) {
        return JSDRV_ERROR_IO;
    }

    size_t file_size = 0;
    void * data = mz_zip_reader_extract_file_to_heap(
        &zip, filename, &file_size, 0);
    mz_zip_reader_end(&zip);

    if (!data) {
        return JSDRV_ERROR_NOT_FOUND;
    }
    *out_data = (uint8_t *) data;
    *out_size = (uint32_t) file_size;
    return 0;
}


// =====================================================================
// JSON helper
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
// Worker -- per-instance update thread
// =====================================================================

enum worker_state_e {
    WST_INIT,
    WST_CTRL_FW,
    WST_WAIT_REMOVE,
    WST_WAIT_ADD,
    WST_REOPEN,
    WST_FPGA_BITSTREAM,
    WST_RESOURCE,
    WST_DONE,
    WST_ERROR,
};

static const char * worker_state_name(enum worker_state_e st) {
    switch (st) {
        case WST_INIT:           return "INIT";
        case WST_CTRL_FW:       return "CTRL_FW";
        case WST_WAIT_REMOVE:   return "WAIT_REMOVE";
        case WST_WAIT_ADD:      return "WAIT_ADD";
        case WST_REOPEN:        return "REOPEN";
        case WST_FPGA_BITSTREAM: return "FPGA_BITSTREAM";
        case WST_RESOURCE:      return "RESOURCE";
        case WST_DONE:          return "DONE";
        case WST_ERROR:         return "ERROR";
        default:                 return "?";
    }
}

struct worker_s {
    uint32_t id;
    volatile bool done;
    volatile bool do_exit;
    volatile bool device_present;
    int32_t result;
    enum worker_state_e state;
    uint32_t resource_idx;

    struct jsdrv_context_s * context;
    char device_prefix[JSDRV_TOPIC_LENGTH_MAX];
    uint32_t flags;
    char status_topic[JSDRV_TOPIC_LENGTH_MAX];

    uint8_t * zip_data;
    uint32_t zip_size;

    uint8_t * ctrl_fw;
    uint32_t  ctrl_fw_size;
    uint8_t * fpga_bit;
    uint32_t  fpga_bit_size;
    uint8_t * resources[RESOURCE_COUNT];
    uint32_t  resource_sizes[RESOURCE_COUNT];

    jsdrv_os_event_t event;
    volatile int32_t rsp_status;

    jsdrv_thread_t thread;
};

static bool worker_cancelled(struct worker_s * w) {
    return w->do_exit;
}


// =====================================================================
// Worker helpers
// =====================================================================

static void worker_publish_status(struct worker_s * w, const char * msg) {
    char escaped[192];
    char buf[256];
    json_escape(escaped, sizeof(escaped), msg);
    tfp_snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"msg\":\"%s\",\"rc\":%d}",
        worker_state_name(w->state), escaped, (int) w->result);
    JSDRV_LOGI("fwup/%03u: %s - %s", w->id, worker_state_name(w->state), msg);
    jsdrv_publish(w->context, w->status_topic,
                  &jsdrv_union_cstr_r(buf), 0);
}

static void worker_fail(struct worker_s * w, int32_t rc, const char * msg) {
    w->result = rc;
    w->state = WST_ERROR;
    worker_publish_status(w, msg);
}

static void on_fwup_rsp(void * user_data, const char * topic,
                        const struct jsdrv_union_s * value) {
    struct worker_s * w = (struct worker_s *) user_data;
    (void) topic;
    if (value->type == JSDRV_UNION_BIN && value->size >= 8) {
        const int32_t * status = (const int32_t *)(value->value.bin + 4);
        w->rsp_status = *status;
    } else {
        w->rsp_status = JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    jsdrv_os_event_signal(w->event);
}

static void on_mem_rsp(void * user_data, const char * topic,
                       const struct jsdrv_union_s * value) {
    struct worker_s * w = (struct worker_s *) user_data;
    (void) topic;
    uint32_t hdr_off = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_off = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size >= hdr_off + sizeof(struct mb_stdmsg_mem_s)) {
        const struct mb_stdmsg_mem_s * rsp =
            (const struct mb_stdmsg_mem_s *)(value->value.bin + hdr_off);
        w->rsp_status = rsp->status;
    } else {
        w->rsp_status = JSDRV_ERROR_MESSAGE_INTEGRITY;
    }
    jsdrv_os_event_signal(w->event);
}

static const char * device_event_prefix(const struct jsdrv_union_s * value) {
    if (value->type == JSDRV_UNION_STR) {
        return value->value.str;
    } else if (value->type == JSDRV_UNION_BIN
               && value->app == JSDRV_PAYLOAD_TYPE_DEVICE
               && value->value.bin) {
        return (const char *) value->value.bin;  // prefix is first field
    }
    return NULL;
}

static void on_device_add(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    struct worker_s * w = (struct worker_s *) user_data;
    (void) topic;
    const char * prefix = device_event_prefix(value);
    if (prefix && jsdrv_cstr_starts_with(prefix, w->device_prefix)) {
        w->device_present = true;
        jsdrv_os_event_signal(w->event);
    }
}

static void on_device_remove(void * user_data, const char * topic,
                             const struct jsdrv_union_s * value) {
    struct worker_s * w = (struct worker_s *) user_data;
    (void) topic;
    const char * prefix = device_event_prefix(value);
    if (prefix && jsdrv_cstr_starts_with(prefix, w->device_prefix)) {
        w->device_present = false;
        jsdrv_os_event_signal(w->event);
    }
}

// Send fwup command (ctrl or fpga) and wait for single response.
static int32_t worker_fwup_send_and_wait(struct worker_s * w,
        const char * cmd_subtopic, const char * rsp_subtopic,
        const uint8_t * cmd_data, uint32_t cmd_size,
        uint32_t timeout_ms) {
    struct jsdrv_topic_s topic;

    w->rsp_status = JSDRV_ERROR_TIMED_OUT;
    jsdrv_os_event_reset(w->event);

    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, rsp_subtopic);
    jsdrv_subscribe(w->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_fwup_rsp, w, 0);

    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, cmd_subtopic);
    int32_t rc = jsdrv_publish(w->context, topic.topic,
                               &jsdrv_union_bin(cmd_data, cmd_size), 0);
    if (rc) {
        jsdrv_topic_set(&topic, w->device_prefix);
        jsdrv_topic_append(&topic, rsp_subtopic);
        jsdrv_unsubscribe(w->context, topic.topic, on_fwup_rsp, w, 0);
        return rc;
    }

    rc = jsdrv_os_event_wait(w->event, timeout_ms);

    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, rsp_subtopic);
    jsdrv_unsubscribe(w->context, topic.topic, on_fwup_rsp, w, 0);

    if (rc) return JSDRV_ERROR_TIMED_OUT;
    return w->rsp_status;
}


// =====================================================================
// Worker state machine steps
// =====================================================================

static int32_t worker_extract(struct worker_s * w) {
    w->state = WST_INIT;
    worker_publish_status(w, "extracting");
    int32_t rc;

    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_CTRL)) {
        rc = jsdrv_fwup_zip_extract(w->zip_data, w->zip_size,
            CTRL_FW_ZIP_PATH, &w->ctrl_fw, &w->ctrl_fw_size);
        if (rc) {
            worker_fail(w, rc, "ctrl firmware not found in ZIP");
            return rc;
        }
    }

    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_FPGA)) {
        rc = jsdrv_fwup_zip_extract(w->zip_data, w->zip_size,
            FPGA_BIT_ZIP_PATH, &w->fpga_bit, &w->fpga_bit_size);
        if (rc) {
            worker_fail(w, rc, "FPGA bitstream not found in ZIP");
            return rc;
        }
    }

    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_RESOURCES)) {
        for (uint32_t i = 0; JSDRV_FWUP_RESOURCES[i].zip_path; ++i) {
            rc = jsdrv_fwup_zip_extract(w->zip_data, w->zip_size,
                JSDRV_FWUP_RESOURCES[i].zip_path,
                &w->resources[i], &w->resource_sizes[i]);
            if (rc) {
                JSDRV_LOGW("fwup/%03u: %s not in ZIP, skip",
                           w->id, JSDRV_FWUP_RESOURCES[i].zip_path);
            }
        }
    }

    free(w->zip_data);
    w->zip_data = NULL;
    w->zip_size = 0;
    return 0;
}

static int32_t worker_build_fwup_cmd(uint8_t ** out, uint32_t * out_size,
        uint32_t txn_id, uint8_t op, uint8_t slot, uint8_t pipeline,
        const uint8_t * image, uint32_t image_size) {
    uint32_t hdr_size = 8;
    uint32_t cmd_size = hdr_size + image_size;
    uint8_t * buf = (uint8_t *) malloc(cmd_size);
    if (!buf) return JSDRV_ERROR_NOT_ENOUGH_MEMORY;
    memset(buf, 0, hdr_size);
    memcpy(buf, &txn_id, 4);
    buf[4] = op;
    buf[5] = slot;
    buf[6] = pipeline;
    if (image && image_size) {
        memcpy(buf + hdr_size, image, image_size);
    }
    *out = buf;
    *out_size = cmd_size;
    return 0;
}

static int32_t worker_ctrl_fw(struct worker_s * w) {
    if (!w->ctrl_fw) return 0;
    w->state = WST_CTRL_FW;
    worker_publish_status(w, "updating ctrl firmware");

    uint8_t * buf = NULL;
    uint32_t buf_size = 0;
    int32_t rc = worker_build_fwup_cmd(&buf, &buf_size,
        1, 1 /*UPDATE*/, 0 /*slot*/, 4 /*pipeline*/,
        w->ctrl_fw, w->ctrl_fw_size);
    if (rc) {
        worker_fail(w, rc, "malloc");
        return rc;
    }

    rc = worker_fwup_send_and_wait(w,
        "h/fwup/ctrl/!cmd", "h/fwup/ctrl/!rsp",
        buf, buf_size, WORKER_TIMEOUT_MS);
    free(buf);
    if (rc) worker_fail(w, rc, "ctrl fw failed");
    return rc;
}

static int32_t worker_wait_reconnect(struct worker_s * w) {
    w->state = WST_WAIT_REMOVE;
    worker_publish_status(w, "waiting for device removal");

    for (uint32_t i = 0; i < 100 && w->device_present && !worker_cancelled(w); ++i) {
        jsdrv_os_event_reset(w->event);
        if (w->device_present) {
            jsdrv_os_event_wait(w->event, 100);
        }
    }

    if (worker_cancelled(w)) return JSDRV_ERROR_ABORTED;

    w->state = WST_WAIT_ADD;
    worker_publish_status(w, "waiting for device");

    for (uint32_t i = 0; i < 300 && !w->device_present && !worker_cancelled(w); ++i) {
        jsdrv_os_event_reset(w->event);
        if (!w->device_present) {
            jsdrv_os_event_wait(w->event, 100);
        }
    }
    if (worker_cancelled(w)) return JSDRV_ERROR_ABORTED;
    if (!w->device_present) {
        worker_fail(w, JSDRV_ERROR_TIMED_OUT, "device did not reappear");
        return JSDRV_ERROR_TIMED_OUT;
    }

    return 0;
}

static int32_t worker_open(struct worker_s * w) {
    worker_publish_status(w, "opening device");
    int32_t rc = jsdrv_open(w->context, w->device_prefix,
                            JSDRV_DEVICE_OPEN_MODE_RESUME, 10000);
    if (rc) {
        worker_fail(w, rc, "device open failed");
        return rc;
    }
    // Subscribe to h/!rsp for all mem operations on this device
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_subscribe(w->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_mem_rsp, w, JSDRV_TIMEOUT_MS_DEFAULT);
    jsdrv_thread_sleep_ms(100);
    return 0;
}

static void worker_close(struct worker_s * w) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_unsubscribe(w->context, topic.topic, on_mem_rsp, w, 0);
    jsdrv_close(w->context, w->device_prefix, JSDRV_TIMEOUT_MS_DEFAULT);
}

static int32_t worker_fpga_bitstream(struct worker_s * w) {
    if (!w->fpga_bit) return 0;
    w->state = WST_FPGA_BITSTREAM;
    worker_publish_status(w, "programming FPGA");

    uint8_t * buf = NULL;
    uint32_t buf_size = 0;
    int32_t rc = worker_build_fwup_cmd(&buf, &buf_size,
        2, 1 /*PROGRAM*/, 0, 4 /*pipeline*/,
        w->fpga_bit, w->fpga_bit_size);
    if (rc) {
        worker_fail(w, rc, "malloc");
        return rc;
    }

    rc = worker_fwup_send_and_wait(w,
        "h/fwup/fpga/!cmd", "h/fwup/fpga/!rsp",
        buf, buf_size, WORKER_TIMEOUT_MS);
    free(buf);
    if (rc) worker_fail(w, rc, "fpga program failed");
    return rc;
}

static int32_t worker_mem_op(struct worker_s * w, uint32_t txn_id,
                             const char * mem_topic,
                             uint8_t operation, uint32_t offset,
                             uint32_t cmd_length,
                             const uint8_t * data, uint32_t data_size) {
    uint32_t payload_size = sizeof(struct mb_stdmsg_header_s)
                          + sizeof(struct mb_stdmsg_mem_s) + data_size;
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
    cmd->length = cmd_length;
    cmd->timeout_ms = (operation == MB_STDMSG_MEM_OP_ERASE) ? 5000 : 1000;
    if (data && data_size) {
        memcpy(cmd->data, data, data_size);
    }

    w->rsp_status = JSDRV_ERROR_TIMED_OUT;
    jsdrv_os_event_reset(w->event);

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, w->device_prefix);
    jsdrv_topic_append(&topic, mem_topic);

    struct jsdrv_union_s value;
    value.type = JSDRV_UNION_STDMSG;
    value.value.bin = buf;
    value.size = payload_size;
    value.flags = 0;
    value.app = 0;
    value.op = 0;

    int32_t rc = jsdrv_publish(w->context, topic.topic, &value, 0);
    free(buf);
    if (rc) return rc;

    uint32_t timeout = (operation == MB_STDMSG_MEM_OP_ERASE) ? 30000 : 5000;
    rc = jsdrv_os_event_wait(w->event, timeout);
    if (rc) return JSDRV_ERROR_TIMED_OUT;
    return w->rsp_status;
}

static int32_t worker_write_resource(struct worker_s * w,
        const struct jsdrv_fwup_resource_s * res,
        const uint8_t * data, uint32_t size) {
    char msg[128];
    int32_t rc;
    uint32_t txn_base = 1000 + w->resource_idx * 1000;

    // Erase in 64KB blocks (device only supports single-block erase)
    tfp_snprintf(msg, sizeof(msg), "erase %s @ 0x%06X (%u B)",
                 res->mem_topic, res->offset, res->erase_size);
    worker_publish_status(w, msg);
    {
        uint32_t erase_off = res->offset;
        uint32_t erase_rem = res->erase_size;
        uint32_t block_num = 0;
        while (erase_rem > 0 && !worker_cancelled(w)) {
            uint32_t block = (erase_rem > 65536) ? 65536 : erase_rem;
            rc = worker_mem_op(w, txn_base + block_num, res->mem_topic,
                               MB_STDMSG_MEM_OP_ERASE,
                               erase_off, block, NULL, 0);
            if (rc) {
                return rc;
            }
            erase_off += block;
            erase_rem -= block;
            ++block_num;
        }
    }

    // Write page by page
    tfp_snprintf(msg, sizeof(msg), "write %s @ 0x%06X (%u B)",
                 res->mem_topic, res->offset, size);
    worker_publish_status(w, msg);

    uint32_t off = res->offset;
    uint32_t rem = size;
    const uint8_t * p = data;
    uint32_t page_num = 1;
    uint8_t page_buf[FW_PAGE_SIZE];
    while (rem > 0 && !worker_cancelled(w)) {
        uint32_t chunk = (rem > FW_PAGE_SIZE) ? FW_PAGE_SIZE : rem;
        // Pad last page to full page size with 0xFF (flash erase value)
        memset(page_buf, 0xFF, FW_PAGE_SIZE);
        memcpy(page_buf, p, chunk);
        rc = worker_mem_op(w, txn_base + page_num, res->mem_topic,
                           MB_STDMSG_MEM_OP_WRITE, off,
                           FW_PAGE_SIZE, page_buf, FW_PAGE_SIZE);
        if (rc) {
            return rc;
        }
        off += FW_PAGE_SIZE;
        p += chunk;
        rem -= chunk;
        ++page_num;
    }
    if (worker_cancelled(w)) {
        rc = JSDRV_ERROR_ABORTED;
    }
    return rc;
}

static bool resource_is_sensor(const struct jsdrv_fwup_resource_s * res) {
    return (res->mem_topic[0] == 's');
}

static int32_t worker_write_resources_filtered(struct worker_s * w,
                                               bool sensor) {
    for (uint32_t i = 0; JSDRV_FWUP_RESOURCES[i].zip_path; ++i) {
        if (!w->resources[i] || !w->resource_sizes[i]) continue;
        if (resource_is_sensor(&JSDRV_FWUP_RESOURCES[i]) != sensor) continue;
        if (worker_cancelled(w)) return JSDRV_ERROR_ABORTED;
        w->resource_idx = i;
        int32_t rc = worker_write_resource(w, &JSDRV_FWUP_RESOURCES[i],
                                           w->resources[i],
                                           w->resource_sizes[i]);
        if (rc) return rc;
    }
    return 0;
}

static int32_t worker_resources(struct worker_s * w) {
    w->state = WST_RESOURCE;
    int32_t rc;

    // Ctrl resources always work immediately
    rc = worker_write_resources_filtered(w, false);
    if (rc) {
        worker_fail(w, rc, "ctrl resource write failed");
        return rc;
    }

    // Sensor resources need sensor comm — retry with reopen if needed
    w->result = 0;  // clear any prior error before first attempt
    rc = worker_write_resources_filtered(w, true);
    if (rc) {
        JSDRV_LOGI("fwup/%03u: sensor resources failed (%d), retrying",
                   w->id, (int) rc);
        w->state = WST_RESOURCE;
        w->result = 0;
        worker_publish_status(w, "reopening for sensor resources");
        worker_close(w);
        jsdrv_thread_sleep_ms(5000);
        rc = worker_open(w);
        if (rc) return rc;
        w->state = WST_RESOURCE;
        rc = worker_write_resources_filtered(w, true);
    }
    if (rc) {
        worker_fail(w, rc, "sensor resource write failed");
    }
    return rc;
}


// =====================================================================
// Worker thread
// =====================================================================

static void worker_free_data(struct worker_s * w) {
    if (w->zip_data) { free(w->zip_data); w->zip_data = NULL; }
    if (w->ctrl_fw) { free(w->ctrl_fw); w->ctrl_fw = NULL; }
    if (w->fpga_bit) { free(w->fpga_bit); w->fpga_bit = NULL; }
    for (uint32_t i = 0; i < RESOURCE_COUNT; ++i) {
        if (w->resources[i]) { free(w->resources[i]); w->resources[i] = NULL; }
    }
}

static THREAD_RETURN_TYPE worker_thread(THREAD_ARG_TYPE lpParam) {
    struct worker_s * w = (struct worker_s *) lpParam;
    int32_t rc;
    bool device_opened = false;

    JSDRV_LOGI("fwup/%03u: started for %s", w->id, w->device_prefix);

    jsdrv_subscribe(w->context, JSDRV_MSG_DEVICE_ADD, JSDRV_SFLAG_PUB,
                    on_device_add, w, 0);
    jsdrv_subscribe(w->context, JSDRV_MSG_DEVICE_REMOVE, JSDRV_SFLAG_PUB,
                    on_device_remove, w, 0);

    rc = worker_extract(w);
    if (rc || worker_cancelled(w)) goto done;

    rc = worker_open(w);
    if (rc || worker_cancelled(w)) goto done;
    device_opened = true;

    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_CTRL)) {
        rc = worker_ctrl_fw(w);
        if (rc) goto close_done;
        worker_close(w);
        device_opened = false;
        rc = worker_wait_reconnect(w);
        if (rc) goto done;
        w->state = WST_REOPEN;
        rc = worker_open(w);
        if (rc) goto done;
        device_opened = true;
    }

    // Write resources BEFORE FPGA programming — the sensor comm
    // won't be available after the bitstream is reprogrammed.
    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_RESOURCES)) {
        if (worker_cancelled(w)) goto close_done;
        rc = worker_resources(w);
        if (rc) goto close_done;
    }

    if (!(w->flags & JSDRV_FWUP_FLAG_SKIP_FPGA)) {
        if (worker_cancelled(w)) goto close_done;
        rc = worker_fpga_bitstream(w);
        if (rc) goto close_done;
    }

    w->state = WST_DONE;
    w->result = 0;
    worker_publish_status(w, "complete");

close_done:
    if (device_opened) {
        worker_close(w);
    }

done:
    jsdrv_unsubscribe(w->context, JSDRV_MSG_DEVICE_ADD,
                      on_device_add, w, 0);
    jsdrv_unsubscribe(w->context, JSDRV_MSG_DEVICE_REMOVE,
                      on_device_remove, w, 0);
    worker_free_data(w);
    JSDRV_LOGI("fwup/%03u: done rc=%d", w->id, (int) w->result);
    w->done = true;
    THREAD_RETURN();
}


// =====================================================================
// Manager
// =====================================================================

struct fwup_mgr_s {
    struct jsdrv_context_s * context;
    struct worker_s workers[JSDRV_FWUP_INSTANCE_MAX];
    uint32_t next_id;
};

static struct fwup_mgr_s mgr_;

// Forward declaration
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
    for (uint32_t i = 0; i < JSDRV_FWUP_INSTANCE_MAX; ++i) {
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
    mgr_send_to_frontend(JSDRV_FWUP_MGR_TOPIC_LIST,
                         &jsdrv_union_cstr_r(buf));
}

static int32_t mgr_send_return_code(const char * topic, int32_t rc) {
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(mgr_.context, "", &jsdrv_union_i32(rc));
    tfp_snprintf(m->topic, sizeof(m->topic), "%s%c",
                 topic, JSDRV_TOPIC_SUFFIX_RETURN_CODE);
    m->extra.frontend.subscriber.internal_fn = mgr_on_add;
    m->extra.frontend.subscriber.user_data = NULL;
    m->extra.frontend.subscriber.is_internal = 1;
    jsdrvp_backend_send(mgr_.context, m);
    return rc;
}

static void mgr_cleanup_done(void) {
    bool changed = false;
    for (uint32_t i = 0; i < JSDRV_FWUP_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context && w->done) {
            jsdrv_thread_join(&w->thread, 5000);
            if (w->event) jsdrv_os_event_free(w->event);
            worker_free_data(w);
            memset(w, 0, sizeof(*w));
            changed = true;
        }
    }
    if (changed) {
        mgr_publish_list();
    }
}

static int32_t mgr_start_worker(const char * device_prefix, uint32_t flags,
                                const uint8_t * zip_data, uint32_t zip_size) {
    mgr_cleanup_done();

    struct worker_s * w = NULL;
    for (uint32_t i = 0; i < JSDRV_FWUP_INSTANCE_MAX; ++i) {
        if (!mgr_.workers[i].context) {
            w = &mgr_.workers[i];
            break;
        }
    }
    if (!w) return JSDRV_ERROR_BUSY;

    memset(w, 0, sizeof(*w));
    w->id = mgr_.next_id++;
    w->context = mgr_.context;
    w->flags = flags;
    jsdrv_cstr_copy(w->device_prefix, device_prefix, sizeof(w->device_prefix));
    tfp_snprintf(w->status_topic, sizeof(w->status_topic),
                 "fwup/%03u/status", w->id);

    w->zip_data = (uint8_t *) malloc(zip_size);
    if (!w->zip_data) {
        memset(w, 0, sizeof(*w));
        return JSDRV_ERROR_NOT_ENOUGH_MEMORY;
    }
    memcpy(w->zip_data, zip_data, zip_size);
    w->zip_size = zip_size;

    w->event = jsdrv_os_event_alloc();
    w->device_present = true;

    if (jsdrv_thread_create(&w->thread, worker_thread, w, 0)) {
        jsdrv_os_event_free(w->event);
        free(w->zip_data);
        memset(w, 0, sizeof(*w));
        return JSDRV_ERROR_UNSPECIFIED;
    }

    mgr_publish_list();
    JSDRV_LOGI("fwup: started %03u for %s", w->id, device_prefix);
    return 0;
}

static uint8_t mgr_on_add(void * user_data, struct jsdrvp_msg_s * msg) {
    (void) user_data;
    if (msg->value.type != JSDRV_UNION_BIN
            || msg->value.size < sizeof(struct jsdrv_fwup_add_header_s)) {
        JSDRV_LOGW("fwup: invalid add payload");
        return mgr_send_return_code(JSDRV_FWUP_MGR_TOPIC_ADD,
                                    JSDRV_ERROR_PARAMETER_INVALID);
    }

    const struct jsdrv_fwup_add_header_s * hdr =
        (const struct jsdrv_fwup_add_header_s *) msg->value.value.bin;
    const uint8_t * zip_data =
        msg->value.value.bin + sizeof(struct jsdrv_fwup_add_header_s);
    uint32_t zip_size = hdr->zip_size;

    if (msg->value.size < sizeof(struct jsdrv_fwup_add_header_s) + zip_size) {
        JSDRV_LOGW("fwup: payload too small for zip_size");
        return mgr_send_return_code(JSDRV_FWUP_MGR_TOPIC_ADD,
                                    JSDRV_ERROR_PARAMETER_INVALID);
    }

    int32_t rc = mgr_start_worker(hdr->device_prefix, hdr->flags,
                                  zip_data, zip_size);
    return mgr_send_return_code(JSDRV_FWUP_MGR_TOPIC_ADD, rc);
}

// Internal subscribe/unsubscribe helpers (follows buffer.c pattern)
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


int32_t jsdrv_fwup_mgr_initialize(struct jsdrv_context_s * context) {
    memset(&mgr_, 0, sizeof(mgr_));
    mgr_.context = context;
    mgr_subscribe(JSDRV_FWUP_MGR_TOPIC_ADD, JSDRV_SFLAG_PUB, mgr_on_add);
    mgr_publish_list();
    JSDRV_LOGI("fwup manager initialized");
    return 0;
}

void jsdrv_fwup_mgr_finalize(void) {
    if (!mgr_.context) return;
    mgr_unsubscribe(JSDRV_FWUP_MGR_TOPIC_ADD, mgr_on_add);

    // Signal all workers to exit, then join
    for (uint32_t i = 0; i < JSDRV_FWUP_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context && !w->done) {
            w->do_exit = true;
            jsdrv_os_event_signal(w->event);
        }
    }
    for (uint32_t i = 0; i < JSDRV_FWUP_INSTANCE_MAX; ++i) {
        struct worker_s * w = &mgr_.workers[i];
        if (w->context) {
            jsdrv_thread_join(&w->thread, 15000);
            if (w->event) jsdrv_os_event_free(w->event);
            worker_free_data(w);
        }
    }
    memset(&mgr_, 0, sizeof(mgr_));
    JSDRV_LOGI("fwup manager finalized");
}
