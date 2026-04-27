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
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/devices/js320/js320_fwup.h"
#include "jsdrv_prv/devices/js320/js320_jtag.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
#include "jsdrv_prv/platform.h"
#include <string.h>


// --- Device-side protocol (uses mb_stdmsg_mem_s) ---

#define FW_PAGE_SIZE            256U
#define PIPELINE_DEFAULT        4U
#define PIPELINE_MAX            16U
#define FLASH_BLOCK_64K         65536U

// Granularity for FPGA write/verify progress events: 256 pages == 64 KB,
// matching the device erase block size.  Also emit at the final ack so the
// last partial chunk is reported.
#define FPGA_PROGRESS_PAGE_INTERVAL  256U

// Locked-mode workaround: the JS320 in locked mode cannot program the
// internal flash (slot 0), so the UPDATE flow erases slot 0 and then
// programs the external SPI flash (slot 1) instead.  The caller's
// requested image_slot is ignored for UPDATE.
#define UPDATE_ERASE_SLOT       0U
#define UPDATE_TARGET_SLOT      1U

/// Device-side fwup operation codes (mirrors mb_fwup_op_e + mb_stdmsg_mem_op_e).
enum fwup_dev_op_e {
    FWUP_DEV_OP_READ      = MB_STDMSG_MEM_OP_READ,
    FWUP_DEV_OP_WRITE     = MB_STDMSG_MEM_OP_WRITE,
    FWUP_DEV_OP_ERASE     = MB_STDMSG_MEM_OP_ERASE,
    FWUP_DEV_OP_ALLOCATE  = MB_STDMSG_MEM_OP_CUSTOM_START,
    FWUP_DEV_OP_UPDATE,
    FWUP_DEV_OP_LAUNCH,
};

#define JTAG_MEM_DELAY_US       100U

// --- Ctrl state machine ---

enum ctrl_state_e {
    CTRL_IDLE = 0,
    CTRL_ERASE_PRE,   ///< Pre-update erase of UPDATE_ERASE_SLOT (locked-mode workaround).
    CTRL_ALLOCATE,
    CTRL_WRITE,
    CTRL_VERIFY,
    CTRL_UPDATE,
    CTRL_LAUNCH,
    CTRL_ERASE,
    CTRL_DONE,
};

// --- FPGA state machine ---

enum fpga_state_e {
    FPGA_IDLE = 0,
    FPGA_MODE_SWITCH,
    FPGA_OPEN,
    FPGA_ERASE,
    FPGA_WRITE,
    FPGA_VERIFY,
    FPGA_CLOSE,
    FPGA_MODE_RESTORE,
    FPGA_DONE,
};

// --- Context ---

struct js320_fwup_s {
    struct jsdrvp_mb_dev_s * dev;

    // Ctrl state
    uint8_t ctrl_state;
    uint32_t ctrl_transaction_id;
    uint8_t ctrl_image_slot;

    // FPGA state
    uint8_t fpga_state;
    uint32_t fpga_transaction_id;

    // Shared image buffer (malloc'd copy)
    uint8_t * image;
    uint32_t image_size;

    // Pipeline tracking (shared by ctrl and fpga)
    uint32_t device_cmd_id;
    uint32_t pipeline_depth;
    uint32_t page_count;
    uint32_t send_idx;
    uint32_t recv_idx;
    uint32_t error_count;
    int32_t error_code;

    // FPGA erase tracking
    uint32_t erase_block_count;
    uint32_t erase_block_idx;
};


// --- Helpers ---

static void fwup_cleanup(struct js320_fwup_s * self) {
    if (self->image) {
        jsdrv_free(self->image);
        self->image = NULL;
    }
    self->image_size = 0;
    self->page_count = 0;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    self->error_code = 0;
}

static void ctrl_send_rsp(struct js320_fwup_s * self, int32_t status) {
    struct fwup_rsp_s rsp;
    rsp.transaction_id = self->ctrl_transaction_id;
    rsp.status = status;
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/fwup/ctrl/!rsp",
        &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
}

static void fpga_send_rsp(struct js320_fwup_s * self, int32_t status) {
    struct fwup_rsp_s rsp;
    rsp.transaction_id = self->fpga_transaction_id;
    rsp.status = status;
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/fwup/fpga/!rsp",
        &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
}

static void fpga_send_progress(struct js320_fwup_s * self,
                                uint8_t phase,
                                uint32_t current,
                                uint32_t total) {
    struct fwup_fpga_progress_s p;
    p.phase = phase;
    p.reserved[0] = 0;
    p.reserved[1] = 0;
    p.reserved[2] = 0;
    p.current = current;
    p.total = total;
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/fwup/fpga/!prog",
        &jsdrv_union_bin((uint8_t *) &p, sizeof(p)));
}

static uint32_t pipeline_depth_validate(uint8_t depth) {
    if (depth == 0) {
        return PIPELINE_DEFAULT;
    }
    if (depth > PIPELINE_MAX) {
        return PIPELINE_MAX;
    }
    return depth;
}


// =====================================================================
// Ctrl firmware update
// =====================================================================

static void ctrl_send_fw_cmd(struct js320_fwup_s * self,
                              uint8_t op, uint8_t image,
                              uint32_t offset, uint32_t length,
                              const uint8_t * data, uint32_t data_size) {
    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + FW_PAGE_SIZE];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    uint32_t msg_size = sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + data_size;

    memset(cmd, 0, sizeof(*cmd));
    cmd->transaction_id = ++self->device_cmd_id;
    cmd->target = image;
    cmd->operation = op;
    cmd->offset = offset;
    cmd->length = length;
    if (data && data_size > 0) {
        memcpy(buf + sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s), data, data_size);
    }

    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = msg_size;
    v.value.bin = buf;
    jsdrvp_mb_dev_publish_to_device(self->dev, "c/fwup/!cmd", &v);
}

static void ctrl_get_page_data(struct js320_fwup_s * self,
                                uint32_t page_idx,
                                uint8_t * page_buf) {
    uint32_t offset = page_idx * FW_PAGE_SIZE;
    uint32_t remaining = self->image_size - offset;
    uint32_t chunk = (remaining > FW_PAGE_SIZE) ? FW_PAGE_SIZE : remaining;
    memcpy(page_buf, self->image + offset, chunk);
    if (chunk < FW_PAGE_SIZE) {
        memset(page_buf + chunk, 0xFF, FW_PAGE_SIZE - chunk);
    }
}

static void ctrl_send_writes(struct js320_fwup_s * self) {
    uint8_t page_buf[FW_PAGE_SIZE];
    while (self->send_idx < self->page_count &&
           (self->send_idx - self->recv_idx) < self->pipeline_depth) {
        uint32_t offset = self->send_idx * FW_PAGE_SIZE;
        ctrl_get_page_data(self, self->send_idx, page_buf);
        ctrl_send_fw_cmd(self, FWUP_DEV_OP_WRITE, 0,
                          offset, FW_PAGE_SIZE,
                          page_buf, FW_PAGE_SIZE);
        self->send_idx++;
    }
}

static void ctrl_send_reads(struct js320_fwup_s * self) {
    while (self->send_idx < self->page_count &&
           (self->send_idx - self->recv_idx) < self->pipeline_depth) {
        uint32_t offset = self->send_idx * FW_PAGE_SIZE;
        ctrl_send_fw_cmd(self, FWUP_DEV_OP_READ, 0,
                          offset, FW_PAGE_SIZE, NULL, 0);
        self->send_idx++;
    }
}

static void ctrl_enter_write(struct js320_fwup_s * self) {
    JSDRV_LOGI("fwup ctrl: write %u pages (pipeline=%u)",
               self->page_count, self->pipeline_depth);
    self->ctrl_state = CTRL_WRITE;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    ctrl_send_writes(self);
}

static void ctrl_enter_verify(struct js320_fwup_s * self) {
    JSDRV_LOGI("fwup ctrl: verify %u pages", self->page_count);
    self->ctrl_state = CTRL_VERIFY;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    ctrl_send_reads(self);
}

static void ctrl_finish(struct js320_fwup_s * self, int32_t status) {
    if (status) {
        JSDRV_LOGW("fwup ctrl: finished with error %d", status);
    } else {
        JSDRV_LOGI("fwup ctrl: complete");
    }
    ctrl_send_rsp(self, status);
    fwup_cleanup(self);
    self->ctrl_state = CTRL_IDLE;
}

static void ctrl_on_cmd(struct js320_fwup_s * self,
                          const struct jsdrv_union_s * value) {
    if (self->ctrl_state != CTRL_IDLE) {
        JSDRV_LOGW("fwup ctrl: command received while busy");
        if (value->type == JSDRV_UNION_BIN && value->size >= sizeof(uint32_t)) {
            self->ctrl_transaction_id = *((const uint32_t *) value->value.bin);
        }
        ctrl_send_rsp(self, JSDRV_ERROR_BUSY);
        return;
    }

    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct fwup_ctrl_cmd_s)) {
        JSDRV_LOGW("fwup ctrl: invalid command format");
        ctrl_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
        return;
    }

    const struct fwup_ctrl_cmd_s * cmd = (const struct fwup_ctrl_cmd_s *) value->value.bin;
    self->ctrl_transaction_id = cmd->transaction_id;
    self->ctrl_image_slot = cmd->image_slot;
    self->pipeline_depth = pipeline_depth_validate(cmd->pipeline_depth);
    self->error_code = 0;

    uint32_t image_size = value->size - sizeof(struct fwup_ctrl_cmd_s);

    switch (cmd->op) {
        case FWUP_CTRL_OP_UPDATE:
            if (image_size == 0) {
                JSDRV_LOGW("fwup ctrl: update with no image data");
                ctrl_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
                return;
            }
            JSDRV_LOGI("fwup ctrl: update size=%u pipeline=%u "
                       "(erase slot %u, program slot %u)",
                       image_size, self->pipeline_depth,
                       UPDATE_ERASE_SLOT, UPDATE_TARGET_SLOT);
            self->image = jsdrv_alloc(image_size);
            memcpy(self->image, cmd->data, image_size);
            self->image_size = image_size;
            self->page_count = (image_size + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;
            // Locked-mode workaround: erase slot 0 (internal flash) before
            // programming slot 1 (external SPI flash).
            self->ctrl_state = CTRL_ERASE_PRE;
            ctrl_send_fw_cmd(self, FWUP_DEV_OP_ERASE, UPDATE_ERASE_SLOT,
                              0, 0, NULL, 0);
            break;

        case FWUP_CTRL_OP_LAUNCH:
            JSDRV_LOGI("fwup ctrl: launch image_slot=%u", cmd->image_slot);
            self->ctrl_state = CTRL_LAUNCH;
            ctrl_send_fw_cmd(self, FWUP_DEV_OP_LAUNCH, cmd->image_slot,
                              0, 0, NULL, 0);
            break;

        case FWUP_CTRL_OP_ERASE:
            JSDRV_LOGI("fwup ctrl: erase image_slot=%u", cmd->image_slot);
            self->ctrl_state = CTRL_ERASE;
            ctrl_send_fw_cmd(self, FWUP_DEV_OP_ERASE, cmd->image_slot,
                              0, 0, NULL, 0);
            break;

        default:
            JSDRV_LOGW("fwup ctrl: unknown op %u", cmd->op);
            ctrl_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
            break;
    }
}

static void ctrl_on_dev_rsp(struct js320_fwup_s * self,
                              const struct jsdrv_union_s * value) {
    uint32_t hdr_offset = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_offset = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_offset + sizeof(struct mb_stdmsg_mem_s)) {
        JSDRV_LOGW("fwup ctrl: rsp too short (%u bytes)", value->size);
        return;
    }

    const struct mb_stdmsg_mem_s * rsp = (const struct mb_stdmsg_mem_s *) (value->value.bin + hdr_offset);

    if (rsp->status != 0) {
        JSDRV_LOGW("fwup ctrl: device error status=%u op=%u offset=0x%06X",
                   rsp->status, rsp->operation, rsp->offset);
        self->error_count++;
        if (self->error_code == 0) {
            self->error_code = JSDRV_ERROR_IO;
        }
    }

    switch (self->ctrl_state) {
        case CTRL_ERASE_PRE:
            if (self->error_count > 0) {
                ctrl_finish(self, self->error_code);
            } else {
                self->ctrl_state = CTRL_ALLOCATE;
                ctrl_send_fw_cmd(self, FWUP_DEV_OP_ALLOCATE, 0, 0,
                                  self->image_size, NULL, 0);
            }
            break;

        case CTRL_ALLOCATE:
            if (self->error_count > 0) {
                ctrl_finish(self, self->error_code);
            } else {
                ctrl_enter_write(self);
            }
            break;

        case CTRL_WRITE:
            self->recv_idx++;
            if (self->error_count > 0) {
                // Drain remaining responses then fail
                if (self->recv_idx >= self->send_idx) {
                    ctrl_finish(self, self->error_code);
                }
            } else if (self->recv_idx >= self->page_count) {
                ctrl_enter_verify(self);
            } else {
                ctrl_send_writes(self);
            }
            break;

        case CTRL_VERIFY: {
            self->recv_idx++;
            // Compare read data against expected
            if (rsp->status == 0 && self->error_code == 0) {
                uint32_t page_idx = rsp->offset / FW_PAGE_SIZE;
                uint32_t data_size = value->size - hdr_offset - sizeof(struct mb_stdmsg_mem_s);
                const uint8_t * read_data = value->value.bin + hdr_offset + sizeof(struct mb_stdmsg_mem_s);
                uint8_t expected[FW_PAGE_SIZE];
                ctrl_get_page_data(self, page_idx, expected);
                if (data_size >= FW_PAGE_SIZE &&
                    0 != memcmp(read_data, expected, FW_PAGE_SIZE)) {
                    JSDRV_LOGW("fwup ctrl: verify mismatch at offset 0x%06X",
                               rsp->offset);
                    self->error_count++;
                    self->error_code = JSDRV_ERROR_IO;
                }
            }
            if (self->error_count > 0) {
                if (self->recv_idx >= self->send_idx) {
                    ctrl_finish(self, self->error_code);
                }
            } else if (self->recv_idx >= self->page_count) {
                // All verified, commit to slot 1 (locked-mode workaround).
                JSDRV_LOGI("fwup ctrl: commit to slot %u", UPDATE_TARGET_SLOT);
                self->ctrl_state = CTRL_UPDATE;
                ctrl_send_fw_cmd(self, FWUP_DEV_OP_UPDATE,
                                  UPDATE_TARGET_SLOT, 0, 0, NULL, 0);
            } else {
                ctrl_send_reads(self);
            }
            break;
        }

        case CTRL_UPDATE:
            ctrl_finish(self, self->error_code);
            break;

        case CTRL_LAUNCH:
            ctrl_finish(self, self->error_code);
            break;

        case CTRL_ERASE:
            ctrl_finish(self, self->error_code);
            break;

        default:
            JSDRV_LOGW("fwup ctrl: unexpected rsp in state %u",
                       self->ctrl_state);
            break;
    }
}


// =====================================================================
// FPGA programming
// =====================================================================

static void fpga_send_jtag_mem_cmd(struct js320_fwup_s * self,
                                     uint8_t op, uint32_t offset,
                                     uint32_t length,
                                     const uint8_t * data,
                                     uint32_t data_size,
                                     uint16_t timeout_ms) {
    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + FW_PAGE_SIZE];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    uint32_t msg_size = sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + data_size;

    memset(cmd, 0, sizeof(*cmd));
    cmd->transaction_id = ++self->device_cmd_id;
    cmd->operation = op;
    cmd->timeout_ms = timeout_ms;
    cmd->offset = offset;
    cmd->length = length;
    cmd->delay_us = JTAG_MEM_DELAY_US;
    if (data && data_size > 0) {
        memcpy(buf + sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s), data, data_size);
    }

    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = msg_size;
    v.value.bin = buf;
    jsdrvp_mb_dev_publish_to_device(self->dev, "c/jtag/!cmd", &v);
}

static void fpga_get_page_data(struct js320_fwup_s * self,
                                uint32_t page_idx,
                                uint8_t * page_buf) {
    uint32_t offset = page_idx * FW_PAGE_SIZE;
    uint32_t remaining = self->image_size - offset;
    uint32_t chunk = (remaining > FW_PAGE_SIZE) ? FW_PAGE_SIZE : remaining;
    memcpy(page_buf, self->image + offset, chunk);
    if (chunk < FW_PAGE_SIZE) {
        memset(page_buf + chunk, 0xFF, FW_PAGE_SIZE - chunk);
    }
}

static void fpga_send_writes(struct js320_fwup_s * self) {
    uint8_t page_buf[FW_PAGE_SIZE];
    while (self->send_idx < self->page_count &&
           (self->send_idx - self->recv_idx) < self->pipeline_depth) {
        uint32_t offset = self->send_idx * FW_PAGE_SIZE;
        fpga_get_page_data(self, self->send_idx, page_buf);
        fpga_send_jtag_mem_cmd(self, MB_STDMSG_MEM_OP_WRITE,
                                offset, FW_PAGE_SIZE,
                                page_buf, FW_PAGE_SIZE, 1000);
        self->send_idx++;
    }
}

static void fpga_send_reads(struct js320_fwup_s * self) {
    while (self->send_idx < self->page_count &&
           (self->send_idx - self->recv_idx) < self->pipeline_depth) {
        uint32_t offset = self->send_idx * FW_PAGE_SIZE;
        fpga_send_jtag_mem_cmd(self, MB_STDMSG_MEM_OP_READ,
                                offset, FW_PAGE_SIZE,
                                NULL, 0, 1000);
        self->send_idx++;
    }
}

static void fpga_enter_close(struct js320_fwup_s * self);

static void fpga_finish(struct js320_fwup_s * self, int32_t status) {
    if (status) {
        JSDRV_LOGW("fwup fpga: finished with error %d", status);
    } else {
        JSDRV_LOGI("fwup fpga: complete");
    }
    fpga_send_rsp(self, status);
    fwup_cleanup(self);
    self->fpga_state = FPGA_IDLE;
}

static void fpga_enter_error_cleanup(struct js320_fwup_s * self) {
    // On error, try to close JTAG and restore mode
    switch (self->fpga_state) {
        case FPGA_OPEN:
        case FPGA_ERASE:
        case FPGA_WRITE:
        case FPGA_VERIFY:
            fpga_enter_close(self);
            break;
        case FPGA_CLOSE:
            // Already closing, let it finish
            break;
        default:
            fpga_finish(self, self->error_code);
            break;
    }
}

static void fpga_enter_erase(struct js320_fwup_s * self) {
    self->erase_block_count = (self->image_size + FLASH_BLOCK_64K - 1) / FLASH_BLOCK_64K;
    self->erase_block_idx = 0;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    JSDRV_LOGI("fwup fpga: erase %u blocks", self->erase_block_count);
    self->fpga_state = FPGA_ERASE;
    fpga_send_jtag_mem_cmd(self, MB_STDMSG_MEM_OP_ERASE,
                            0, FLASH_BLOCK_64K, NULL, 0, 5000);
}

static void fpga_enter_write(struct js320_fwup_s * self) {
    JSDRV_LOGI("fwup fpga: write %u pages (pipeline=%u)",
               self->page_count, self->pipeline_depth);
    self->fpga_state = FPGA_WRITE;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    fpga_send_writes(self);
}

static void fpga_enter_verify(struct js320_fwup_s * self) {
    JSDRV_LOGI("fwup fpga: verify %u pages", self->page_count);
    self->fpga_state = FPGA_VERIFY;
    self->send_idx = 0;
    self->recv_idx = 0;
    self->error_count = 0;
    fpga_send_reads(self);
}

static void fpga_enter_close(struct js320_fwup_s * self) {
    JSDRV_LOGI("fwup fpga: close JTAG");
    self->fpga_state = FPGA_CLOSE;
    fpga_send_jtag_mem_cmd(self, JS320_JTAG_OP_MEM_CLOSE,
                            0, 0, NULL, 0, 5000);
}

static void fpga_on_cmd(struct js320_fwup_s * self,
                          const struct jsdrv_union_s * value) {
    if (self->fpga_state != FPGA_IDLE) {
        JSDRV_LOGW("fwup fpga: command received while busy");
        if (value->type == JSDRV_UNION_BIN && value->size >= sizeof(uint32_t)) {
            self->fpga_transaction_id = *((const uint32_t *) value->value.bin);
        }
        fpga_send_rsp(self, JSDRV_ERROR_BUSY);
        return;
    }

    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct fwup_fpga_cmd_s)) {
        JSDRV_LOGW("fwup fpga: invalid command format");
        fpga_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
        return;
    }

    const struct fwup_fpga_cmd_s * cmd = (const struct fwup_fpga_cmd_s *) value->value.bin;
    self->fpga_transaction_id = cmd->transaction_id;
    self->pipeline_depth = pipeline_depth_validate(cmd->pipeline_depth);
    self->error_code = 0;

    uint32_t image_size = value->size - sizeof(struct fwup_fpga_cmd_s);

    switch (cmd->op) {
        case FWUP_FPGA_OP_PROGRAM:
            if (image_size == 0) {
                JSDRV_LOGW("fwup fpga: program with no image data");
                fpga_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
                return;
            }
            JSDRV_LOGI("fwup fpga: program size=%u pipeline=%u",
                       image_size, self->pipeline_depth);
            self->image = jsdrv_alloc(image_size);
            memcpy(self->image, cmd->data, image_size);
            self->image_size = image_size;
            self->page_count = (image_size + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;
            // Disconnect sensor comm before JTAG mode to ensure
            // a clean reconnect after the bitstream is reprogrammed.
            jsdrvp_mb_dev_publish_to_device(self->dev, "c/comm/sensor/!req",
                &jsdrv_union_u8(0));
            // Start with mode switch to JTAG
            self->fpga_state = FPGA_MODE_SWITCH;
            jsdrvp_mb_dev_publish_to_device(self->dev, "c/mode",
                &jsdrv_union_u32(2));
            jsdrvp_mb_dev_set_timeout(self->dev,
                jsdrv_time_utc() + JSDRV_TIME_MILLISECOND * 100);
            break;

        default:
            JSDRV_LOGW("fwup fpga: unknown op %u", cmd->op);
            fpga_send_rsp(self, JSDRV_ERROR_PARAMETER_INVALID);
            break;
    }
}

static void fpga_on_dev_rsp(struct js320_fwup_s * self,
                              const struct jsdrv_union_s * value) {
    uint32_t hdr_offset = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_offset = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_offset + sizeof(struct mb_stdmsg_mem_s)) {
        JSDRV_LOGW("fwup fpga: rsp too short (%u bytes)", value->size);
        return;
    }

    const struct mb_stdmsg_mem_s * rsp = (const struct mb_stdmsg_mem_s *) (value->value.bin + hdr_offset);

    if (rsp->status != 0) {
        JSDRV_LOGW("fwup fpga: device error status=%u op=%u offset=0x%06X",
                   rsp->status, rsp->operation, rsp->offset);
        self->error_count++;
        if (self->error_code == 0) {
            self->error_code = JSDRV_ERROR_IO;
        }
    }

    switch (self->fpga_state) {
        case FPGA_OPEN:
            if (self->error_count > 0) {
                fpga_enter_error_cleanup(self);
            } else {
                fpga_enter_erase(self);
            }
            break;

        case FPGA_ERASE:
            if (self->error_count > 0) {
                fpga_enter_error_cleanup(self);
            } else {
                self->erase_block_idx++;
                fpga_send_progress(self, FWUP_FPGA_PROGRESS_PHASE_ERASE,
                                    self->erase_block_idx,
                                    self->erase_block_count);
                if (self->erase_block_idx >= self->erase_block_count) {
                    fpga_enter_write(self);
                } else {
                    uint32_t offset = self->erase_block_idx * FLASH_BLOCK_64K;
                    fpga_send_jtag_mem_cmd(self, MB_STDMSG_MEM_OP_ERASE,
                                            offset, FLASH_BLOCK_64K, NULL, 0, 5000);
                }
            }
            break;

        case FPGA_WRITE:
            self->recv_idx++;
            if (self->error_count > 0) {
                if (self->recv_idx >= self->send_idx) {
                    fpga_enter_error_cleanup(self);
                }
            } else if (self->recv_idx >= self->page_count) {
                fpga_send_progress(self, FWUP_FPGA_PROGRESS_PHASE_WRITE,
                                    self->recv_idx, self->page_count);
                fpga_enter_verify(self);
            } else {
                if ((self->recv_idx % FPGA_PROGRESS_PAGE_INTERVAL) == 0) {
                    fpga_send_progress(self, FWUP_FPGA_PROGRESS_PHASE_WRITE,
                                        self->recv_idx, self->page_count);
                }
                fpga_send_writes(self);
            }
            break;

        case FPGA_VERIFY: {
            self->recv_idx++;
            if (rsp->status == 0 && self->error_code == 0) {
                uint32_t page_idx = rsp->offset / FW_PAGE_SIZE;
                uint32_t data_size = value->size - hdr_offset - sizeof(struct mb_stdmsg_mem_s);
                const uint8_t * read_data = value->value.bin + hdr_offset + sizeof(struct mb_stdmsg_mem_s);
                uint8_t expected[FW_PAGE_SIZE];
                fpga_get_page_data(self, page_idx, expected);
                if (data_size >= FW_PAGE_SIZE &&
                    0 != memcmp(read_data, expected, FW_PAGE_SIZE)) {
                    JSDRV_LOGW("fwup fpga: verify mismatch at offset 0x%06X",
                               rsp->offset);
                    self->error_count++;
                    self->error_code = JSDRV_ERROR_IO;
                }
            }
            if (self->error_count > 0) {
                if (self->recv_idx >= self->send_idx) {
                    fpga_enter_error_cleanup(self);
                }
            } else if (self->recv_idx >= self->page_count) {
                fpga_send_progress(self, FWUP_FPGA_PROGRESS_PHASE_VERIFY,
                                    self->recv_idx, self->page_count);
                fpga_enter_close(self);
            } else {
                if ((self->recv_idx % FPGA_PROGRESS_PAGE_INTERVAL) == 0) {
                    fpga_send_progress(self, FWUP_FPGA_PROGRESS_PHASE_VERIFY,
                                        self->recv_idx, self->page_count);
                }
                fpga_send_reads(self);
            }
            break;
        }

        case FPGA_CLOSE:
            // Restore normal mode
            self->fpga_state = FPGA_MODE_RESTORE;
            jsdrvp_mb_dev_publish_to_device(self->dev, "c/mode",
                &jsdrv_union_u32(0));
            jsdrvp_mb_dev_set_timeout(self->dev,
                jsdrv_time_utc() + JSDRV_TIME_MILLISECOND * 100);
            break;

        default:
            JSDRV_LOGW("fwup fpga: unexpected rsp in state %u",
                       self->fpga_state);
            break;
    }
}

static void fpga_on_timeout(struct js320_fwup_s * self) {
    switch (self->fpga_state) {
        case FPGA_MODE_SWITCH:
            // Mode switch delay done, open JTAG
            JSDRV_LOGI("fwup fpga: open JTAG");
            self->fpga_state = FPGA_OPEN;
            fpga_send_jtag_mem_cmd(self, JS320_JTAG_OP_MEM_OPEN,
                                    0, 0, NULL, 0, 5000);
            break;

        case FPGA_MODE_RESTORE:
            // Reconnect sensor comm after FPGA bitstream reload
            jsdrvp_mb_dev_publish_to_device(self->dev, "c/comm/sensor/!req",
                &jsdrv_union_u8(1));
            fpga_finish(self, self->error_code);
            break;

        default:
            JSDRV_LOGW("fwup fpga: unexpected timeout in state %u",
                       self->fpga_state);
            break;
    }
}


// =====================================================================
// Public API
// =====================================================================

struct js320_fwup_s * js320_fwup_alloc(void) {
    return jsdrv_alloc_clr(sizeof(struct js320_fwup_s));
}

void js320_fwup_free(struct js320_fwup_s * fwup) {
    fwup_cleanup(fwup);
    jsdrv_free(fwup);
}

void js320_fwup_on_open(struct js320_fwup_s * fwup,
                         struct jsdrvp_mb_dev_s * dev) {
    fwup->dev = dev;
}

void js320_fwup_on_close(struct js320_fwup_s * fwup) {
    if (fwup->ctrl_state != CTRL_IDLE) {
        JSDRV_LOGW("fwup ctrl: device closed during operation, aborting");
        fwup->ctrl_state = CTRL_IDLE;
    }
    if (fwup->fpga_state != FPGA_IDLE) {
        JSDRV_LOGW("fwup fpga: device closed during operation, aborting");
        jsdrvp_mb_dev_set_timeout(fwup->dev, 0);
        fwup->fpga_state = FPGA_IDLE;
    }
    fwup_cleanup(fwup);
    fwup->dev = NULL;
}

bool js320_fwup_handle_cmd(struct js320_fwup_s * fwup,
                            const char * subtopic,
                            const struct jsdrv_union_s * value) {
    if (0 == strcmp(subtopic, "h/fwup/ctrl/!cmd")) {
        ctrl_on_cmd(fwup, value);
        return true;
    }
    if (0 == strcmp(subtopic, "h/fwup/fpga/!cmd")) {
        fpga_on_cmd(fwup, value);
        return true;
    }
    return false;
}

bool js320_fwup_handle_publish(struct js320_fwup_s * fwup,
                                const char * subtopic,
                                const struct jsdrv_union_s * value) {
    if (fwup->ctrl_state != CTRL_IDLE &&
        0 == strcmp(subtopic, "h/!rsp")) {
        ctrl_on_dev_rsp(fwup, value);
        return true;
    }
    if (fwup->fpga_state != FPGA_IDLE &&
        0 == strcmp(subtopic, "h/!rsp")) {
        fpga_on_dev_rsp(fwup, value);
        return true;
    }
    return false;
}

void js320_fwup_on_timeout(struct js320_fwup_s * fwup) {
    if (fwup->fpga_state != FPGA_IDLE) {
        fpga_on_timeout(fwup);
    }
}
