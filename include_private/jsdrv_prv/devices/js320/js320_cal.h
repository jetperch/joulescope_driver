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
 * @brief JS320 host-side calibration: slot read/copy and offset cal.
 *
 * Implements per-device calibration operations over the existing open
 * device.  The caller must open the device before publishing a
 * command.  Streaming state mutated by an offset cal sweep is captured
 * at command entry and restored before the final response is sent.
 *
 * Topics:
 *   h/cal/!cmd       Command (jsdrv_cal_cmd_s)
 *   h/cal/!rsp       Response (jsdrv_cal_rsp_s)
 *   h/cal/!status    Progress status (JSON string, retained)
 *   h/cal/!data      Slot record data (binary 1024 bytes, slot_read only)
 */

#ifndef JSDRV_PRV_JS320_CAL_H_
#define JSDRV_PRV_JS320_CAL_H_

#include "jsdrv/cmacro_inc.h"
#include <stdbool.h>
#include <stdint.h>

JSDRV_CPP_GUARD_START

struct jsdrvp_mb_dev_s;
struct jsdrv_union_s;

/// Opaque calibration context.
struct js320_cal_s;

/// Size of the JS320 calibration record (matches struct js320_calibration_s).
#define JSDRV_CAL_RECORD_SIZE     1024U

/**
 * @brief Magic prefix written into the 64-byte signature field of
 *      records produced by this driver.
 *
 * Exactly 16 ASCII characters, no NUL terminator.  Bytes 16..19 of the
 * signature carry the jsdrv version (u32, little-endian) and bytes
 * 20..63 are zero.  The FPGA does not verify the ECDSA signature, so
 * this pattern serves as a customer-support tag identifying records
 * as driver-generated.
 */
#define JSDRV_CAL_SIGNATURE_MAGIC "JSDRV_OFFSET_CAL"

/**
 * @brief Calibration operation codes.
 */
enum jsdrv_cal_op_e {
    JSDRV_CAL_OP_SLOT_READ      = 0,  ///< Read src_slot, publish 1024-byte record on h/cal/!data.
    JSDRV_CAL_OP_SLOT_COPY      = 1,  ///< Copy src_slot -> dst_slot (policy enforced).
    JSDRV_CAL_OP_CURRENT_OFFSET = 2,  ///< Sweep current offsets, write ACTIVE.
    JSDRV_CAL_OP_VOLTAGE_OFFSET = 3,  ///< Measure voltage offsets, write ACTIVE.
};

/**
 * @brief Calibration slot identifiers.
 *
 * Mirrors FLASH_BLOCK_CAL_* on the fpga_mcu side.  ACTIVE is the
 * runtime cal; FACTORY is read-only from the driver.
 */
enum jsdrv_cal_slot_e {
    JSDRV_CAL_SLOT_ACTIVE  = 0,
    JSDRV_CAL_SLOT_TRIM2   = 1,
    JSDRV_CAL_SLOT_TRIM1   = 2,
    JSDRV_CAL_SLOT_FIELD   = 3,
    JSDRV_CAL_SLOT_LAB     = 4,
    JSDRV_CAL_SLOT_FACTORY = 5,
    JSDRV_CAL_SLOT_COUNT,
};

/**
 * @brief Calibration command published to h/cal/!cmd.
 *
 * For SLOT_READ, only src_slot is used.
 *
 * For SLOT_COPY, src_slot and dst_slot are both used.  Allowed
 * transitions:
 *     dst = ACTIVE: src in {TRIM2, TRIM1, FIELD, LAB, FACTORY}
 *     dst = TRIM1:  src = ACTIVE
 *     dst = TRIM2:  src = ACTIVE
 *
 * For CURRENT_OFFSET and VOLTAGE_OFFSET, src_slot and dst_slot are
 * ignored (ACTIVE is implied).  samples_per_point controls the number
 * of ADC samples averaged at each measurement point; 0 selects the
 * default (200000 samples = 12 power-line cycles at 1 MHz).
 */
struct jsdrv_cal_cmd_s {
    uint32_t transaction_id;
    uint8_t  op;                  ///< jsdrv_cal_op_e
    uint8_t  src_slot;            ///< jsdrv_cal_slot_e
    uint8_t  dst_slot;            ///< jsdrv_cal_slot_e
    uint8_t  flags;               ///< Reserved = 0
    uint32_t samples_per_point;   ///< Per-point sample count; 0 = default
};

/**
 * @brief Calibration final response published to h/cal/!rsp.
 */
struct jsdrv_cal_rsp_s {
    uint32_t transaction_id;
    int32_t  status;              ///< 0 on success, JSDRV_ERROR_* on failure
};

// --- Lifecycle ---

struct js320_cal_s * js320_cal_alloc(void);
void js320_cal_free(struct js320_cal_s * cal);

/// Called when the device link opens.  Caches the dev handle.
void js320_cal_on_open(struct js320_cal_s * cal, struct jsdrvp_mb_dev_s * dev);

/// Called when the device is closing.  Aborts any in-progress operation.
void js320_cal_on_close(struct js320_cal_s * cal);

// --- Callbacks delegated from js320_drv ---

/// Handle h/cal/!cmd and observe host-set streaming state for restore.
/// Returns true if h/cal/!cmd was consumed.
bool js320_cal_handle_cmd(struct js320_cal_s * cal,
                          const char * subtopic,
                          const struct jsdrv_union_s * value);

/// Intercept h/!rsp for outstanding mem ops and observe device-published
/// streaming state for restore.  Returns true only for h/!rsp consumed
/// during an outstanding mem op.
bool js320_cal_handle_publish(struct js320_cal_s * cal,
                              const char * subtopic,
                              const struct jsdrv_union_s * value);

/// Called when the upper driver timer expires.
void js320_cal_on_timeout(struct js320_cal_s * cal);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_JS320_CAL_H_ */
