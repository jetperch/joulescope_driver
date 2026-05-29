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
 * @brief JS320 calibration manager.
 *
 * Drives the JS320 calibration flash slots and offset-calibration
 * sweeps from the host using only the public jsdrv pubsub API.  The
 * worker uses the existing s/flash/!cmd memory protocol to read and
 * write the 1024-byte js320_calibration_s record in each slot.
 *
 * The fpga_mcu searches slots ACTIVE -> TRIM2 -> TRIM1 -> FIELD -> LAB
 * -> FACTORY at boot and uses the first valid record (by
 * mb_check32_xxhash).  The FPGA does not verify the 64-byte ECDSA
 * signature, so driver-generated offset cal records populate it with a
 * known pattern (see JSDRV_CAL_SIGNATURE_MAGIC) so customer support
 * can identify a record as having been written by this driver.
 *
 * PubSub topics (no device prefix; same shape as fwup):
 *   cal/@/!add        Create a new cal instance (BIN: jsdrv_cal_add_header_s)
 *   cal/@/!add#       Response to !add (BIN: jsdrv_cal_add_rsp_s)
 *   cal/@/list        Retained, comma-separated list of active worker ids
 *   cal/NNN/status    Retained JSON {state, pct, msg, rc, op}
 *   cal/NNN/data      On slot_read success: 1024-byte calibration record
 */

#ifndef JSDRV_PRV_JS320_CAL_MGR_H__
#define JSDRV_PRV_JS320_CAL_MGR_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

JSDRV_CPP_GUARD_START

struct jsdrv_context_s;

#define JSDRV_CAL_MGR_TOPIC_ADD   "cal/@/!add"
#define JSDRV_CAL_MGR_TOPIC_LIST  "cal/@/list"
#define JSDRV_CAL_INSTANCE_MAX    4

/// Size of the JS320 calibration record (matches struct js320_calibration_s).
#define JSDRV_CAL_RECORD_SIZE     1024U

/**
 * @brief Magic prefix written into the 64-byte signature field of
 *      records produced by this driver.
 *
 * Exactly 16 ASCII characters, no NUL terminator.  Bytes 16..19 of the
 * signature carry the jsdrv version (u32, little-endian) and bytes
 * 20..63 are zero.  The FPGA does not verify the ECDSA signature, so
 * this pattern serves as a customer-support tag identifying records as
 * driver-generated.
 */
#define JSDRV_CAL_SIGNATURE_MAGIC "JSDRV_OFFSET_CAL"

/**
 * @brief Operation selector for the !add command.
 */
enum jsdrv_cal_op_e {
    JSDRV_CAL_OP_SLOT_READ      = 0,  ///< Read src_slot, publish 1024 bytes on cal/NNN/data
    JSDRV_CAL_OP_SLOT_COPY      = 1,  ///< Copy src_slot -> dst_slot (policy-constrained)
    JSDRV_CAL_OP_CURRENT_OFFSET = 2,  ///< Sweep current offsets, write ACTIVE
    JSDRV_CAL_OP_VOLTAGE_OFFSET = 3,  ///< Measure voltage offsets, write ACTIVE
};

/**
 * @brief Calibration slot identifiers.
 *
 * Maps to the FLASH_BLOCK_CAL_* enum on the device side.  ACTIVE is
 * the runtime cal; FACTORY is read-only.
 */
enum jsdrv_cal_slot_e {
    JSDRV_CAL_SLOT_ACTIVE  = 0,  ///< Runtime calibration (FLASH_BLOCK_CAL_ACTIVE)
    JSDRV_CAL_SLOT_TRIM2   = 1,  ///< Trim slot 2 (FLASH_BLOCK_CAL_TRIM2)
    JSDRV_CAL_SLOT_TRIM1   = 2,  ///< Trim slot 1 (FLASH_BLOCK_CAL_TRIM1)
    JSDRV_CAL_SLOT_FIELD   = 3,  ///< Field cal (FLASH_BLOCK_CAL_FIELD)
    JSDRV_CAL_SLOT_LAB     = 4,  ///< Lab cal (FLASH_BLOCK_CAL_LAB)
    JSDRV_CAL_SLOT_FACTORY = 5,  ///< Factory cal, read-only (FLASH_BLOCK_CAL_FACTORY)
    JSDRV_CAL_SLOT_COUNT,
};

/**
 * @brief Header for the cal/@/!add command payload.
 *
 * For all ops, @ref device_prefix selects the target JS320 (e.g.,
 * "u/js320/8W2A").  Other fields are op-specific:
 *
 * - SLOT_READ: src_slot selects which slot to read.
 * - SLOT_COPY: src_slot and dst_slot select the source and destination.
 *   Only these src/dst combinations are accepted by the worker:
 *     dst=ACTIVE: src in {TRIM2, TRIM1, FIELD, LAB, FACTORY}
 *     dst=TRIM1:  src=ACTIVE
 *     dst=TRIM2:  src=ACTIVE
 * - CURRENT_OFFSET / VOLTAGE_OFFSET: src_slot and dst_slot ignored
 *   (always operates on ACTIVE).  samples_per_point overrides the
 *   default sample count (0 -> default).
 */
struct jsdrv_cal_add_header_s {
    char device_prefix[32];      ///< Target device prefix (e.g., "u/js320/8W2A")
    uint8_t op;                  ///< jsdrv_cal_op_e
    uint8_t src_slot;            ///< jsdrv_cal_slot_e for read/copy ops
    uint8_t dst_slot;            ///< jsdrv_cal_slot_e for copy op
    uint8_t flags;               ///< Reserved = 0
    uint32_t samples_per_point;  ///< Per-point sample count; 0 = default
};

/**
 * @brief Response published to cal/@/!add# on each command.
 *
 * On success, @ref rc is 0 and @ref worker_id identifies the worker so
 * the caller can subscribe to cal/NNN/status (and cal/NNN/data for
 * slot_read) for results.
 */
struct jsdrv_cal_add_rsp_s {
    int32_t rc;                  ///< 0 on success, JSDRV_ERROR_* on failure
    uint32_t worker_id;          ///< Assigned worker id (valid iff rc == 0)
};

/**
 * @brief Initialize the calibration manager.
 *
 * @param context The joulescope driver context.
 * @return 0 or error code.
 */
int32_t jsdrv_cal_mgr_initialize(struct jsdrv_context_s * context);

/**
 * @brief Finalize the calibration manager.
 */
void jsdrv_cal_mgr_finalize(void);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_JS320_CAL_MGR_H__ */
