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

/**
 * @file
 *
 * @brief JS320 host-side firmware update and FPGA programming.
 *
 * This subsystem receives a complete firmware or FPGA image via
 * pubsub, then drives the page-level device protocol internally.
 *
 * Topics:
 *   h/fwup/ctrl/!cmd       — Ctrl firmware command (fwup_ctrl_cmd_s)
 *   h/fwup/ctrl/!rsp       — Ctrl firmware response (fwup_rsp_s)
 *   h/fwup/fpga/!cmd       — FPGA programming command (fwup_fpga_cmd_s)
 *   h/fwup/fpga/!rsp       — FPGA programming response (fwup_rsp_s)
 *   h/fwup/fpga/!prog      — FPGA progress events (fwup_fpga_progress_s)
 */

#ifndef JSDRV_PRV_JS320_FWUP_H_
#define JSDRV_PRV_JS320_FWUP_H_

#include "jsdrv/cmacro_inc.h"
#include <stdbool.h>
#include <stdint.h>

JSDRV_CPP_GUARD_START

struct jsdrvp_mb_dev_s;
struct jsdrv_union_s;

/// Opaque firmware update context.
struct js320_fwup_s;

// --- Ctrl command/response message formats ---

enum fwup_ctrl_op_e {
    FWUP_CTRL_OP_UPDATE = 1,    ///< Write + verify + commit image
    FWUP_CTRL_OP_LAUNCH = 2,    ///< Boot from image slot
    FWUP_CTRL_OP_ERASE  = 3,    ///< Erase image slot
};

struct fwup_ctrl_cmd_s {
    uint32_t transaction_id;
    uint8_t op;                 ///< fwup_ctrl_op_e
    uint8_t image_slot;         ///< Target image slot (0-1)
    uint8_t pipeline_depth;     ///< 0 = default (8), max 16
    uint8_t rsv;
    uint8_t data[];             ///< Image bytes (for UPDATE op)
};

// --- FPGA command/response message formats ---

enum fwup_fpga_op_e {
    FWUP_FPGA_OP_PROGRAM = 1,  ///< Erase + write + verify
};

struct fwup_fpga_cmd_s {
    uint32_t transaction_id;
    uint8_t op;                 ///< fwup_fpga_op_e
    uint8_t pipeline_depth;     ///< 0 = default (8), max 16
    uint16_t rsv;
    uint8_t data[];             ///< FPGA bitstream bytes
};

// --- Shared response format ---

struct fwup_rsp_s {
    uint32_t transaction_id;
    int32_t status;             ///< 0 = success, JSDRV_ERROR_* on failure
};

// --- FPGA progress event ---

enum fwup_fpga_progress_phase_e {
    FWUP_FPGA_PROGRESS_PHASE_ERASE  = 0,
    FWUP_FPGA_PROGRESS_PHASE_WRITE  = 1,
    FWUP_FPGA_PROGRESS_PHASE_VERIFY = 2,
};

struct fwup_fpga_progress_s {
    uint8_t phase;              ///< fwup_fpga_progress_phase_e
    uint8_t reserved[3];
    uint32_t current;           ///< Units completed in this phase
    uint32_t total;              ///< Total units in this phase
};

// --- Lifecycle ---

struct js320_fwup_s * js320_fwup_alloc(void);
void js320_fwup_free(struct js320_fwup_s * fwup);

/// Called when the device link opens.
void js320_fwup_on_open(struct js320_fwup_s * fwup,
                         struct jsdrvp_mb_dev_s * dev);

/// Called when the device is closing.  Aborts any in-progress operation.
void js320_fwup_on_close(struct js320_fwup_s * fwup);

// --- Callbacks delegated from js320_drv ---

/// Handle h/fwup/* commands.  Returns true if consumed.
bool js320_fwup_handle_cmd(struct js320_fwup_s * fwup,
                            const char * subtopic,
                            const struct jsdrv_union_s * value);

/// Intercept device responses during fwup operations.  Returns true if consumed.
bool js320_fwup_handle_publish(struct js320_fwup_s * fwup,
                                const char * subtopic,
                                const struct jsdrv_union_s * value);

/// Called when the upper driver timer expires.
void js320_fwup_on_timeout(struct js320_fwup_s * fwup);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_JS320_FWUP_H_ */
