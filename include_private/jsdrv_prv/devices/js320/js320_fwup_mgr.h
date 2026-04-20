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

/**
 * @file
 *
 * @brief JS320 full firmware update manager.
 *
 * Manages firmware update worker threads that perform the full
 * JS320 update sequence from a manufacturing ZIP package.
 * Workers use the public jsdrv API and survive device
 * disconnect/reconnect during ctrl firmware update.
 *
 * PubSub topics:
 *   fwup/@/!add    Create a new update instance (BIN: header + ZIP)
 *   fwup/@/list    Retained list of active instance IDs (BIN: u8[])
 *   fwup/NNN/status  Retained status for instance NNN (STR: JSON)
 */

#ifndef JSDRV_PRV_JS320_FWUP_MGR_H__
#define JSDRV_PRV_JS320_FWUP_MGR_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

JSDRV_CPP_GUARD_START

struct jsdrv_context_s;

#define JSDRV_FWUP_MGR_TOPIC_ADD    "fwup/@/!add"
#define JSDRV_FWUP_MGR_TOPIC_LIST   "fwup/@/list"
#define JSDRV_FWUP_INSTANCE_MAX     4

/**
 * @brief Flags for the fwup add command header.
 */
enum jsdrv_fwup_flags_e {
    JSDRV_FWUP_FLAG_SKIP_CTRL      = (1 << 0),
    JSDRV_FWUP_FLAG_SKIP_FPGA      = (1 << 1),
    JSDRV_FWUP_FLAG_SKIP_RESOURCES = (1 << 2),
};

/**
 * @brief Header for the fwup/@/!add command payload.
 *
 * The payload is: header + ZIP data.
 */
struct jsdrv_fwup_add_header_s {
    char device_prefix[32];     ///< Target device prefix (e.g., "u/js320/8w2a")
    uint32_t flags;             ///< jsdrv_fwup_flags_e
    uint32_t zip_size;          ///< Size of ZIP data following this header
};

/**
 * @brief Resource entry describing a flash blob to write.
 */
struct jsdrv_fwup_resource_s {
    const char * zip_path;      ///< Path within ZIP archive
    const char * mem_topic;     ///< Device subtopic (e.g., "c/xspi/!cmd")
    uint32_t offset;            ///< Flash offset
    uint32_t erase_size;        ///< Erase region size
};

/**
 * @brief The resource table for JS320 updates.
 *
 * Terminated by a NULL zip_path entry.
 */
extern const struct jsdrv_fwup_resource_s JSDRV_FWUP_RESOURCES[];

/**
 * @brief Extract a file from a ZIP archive in memory.
 *
 * @param zip_data The ZIP archive data.
 * @param zip_size The size of zip_data in bytes.
 * @param filename The file to extract from the archive.
 * @param[out] out_data On success, receives a malloc'd buffer
 *      with the file contents. Caller must free().
 * @param[out] out_size On success, receives the file size.
 * @return 0 on success, or error code.
 */
int32_t jsdrv_fwup_zip_extract(
    const uint8_t * zip_data, uint32_t zip_size,
    const char * filename,
    uint8_t ** out_data, uint32_t * out_size);

/**
 * @brief Initialize the firmware update manager.
 *
 * @param context The joulescope driver context.
 * @return 0 or error code.
 */
int32_t jsdrv_fwup_mgr_initialize(struct jsdrv_context_s * context);

/**
 * @brief Finalize the firmware update manager.
 */
void jsdrv_fwup_mgr_finalize(void);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_JS320_FWUP_MGR_H__ */
