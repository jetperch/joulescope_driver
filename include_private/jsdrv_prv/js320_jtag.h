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
 * @brief JS320 host-side JTAG operations (ECP5 AES key programming).
 */

#ifndef JSDRV_PRV_JS320_JTAG_H_
#define JSDRV_PRV_JS320_JTAG_H_

#include "jsdrv/cmacro_inc.h"
#include "mb/stdmsg.h"
#include <stdbool.h>
#include <stdint.h>

JSDRV_CPP_GUARD_START

struct jsdrvp_mb_dev_s;
struct jsdrv_union_s;

/// Opaque JTAG context.
struct js320_jtag_s;

// --- AES command/response message formats ---

#define ECP5_AES_FLAG_KEY_LOCK          0x01
#define ECP5_AES_FLAG_ENCRYPT_ONLY      0x02


enum js320_jtag_op_e {
    JS320_JTAG_OP_GOTO_STATE    = MB_STDMSG_MEM_OP_CUSTOM_START,
    JS320_JTAG_OP_SHIFT,
    JS320_JTAG_OP_MEM_OPEN,
    JS320_JTAG_OP_MEM_CLOSE,
    JS320_JTAG_OP_MEM_READ_UID,
};

struct jtag_aes_cmd_s {
    uint32_t transaction_id;
    uint32_t flags;             ///< ECP5_AES_FLAG_*
    uint8_t key[16];            ///< 128-bit key, big-endian (key[0]=MSB)
};

struct jtag_aes_rsp_s {
    uint32_t transaction_id;
    int32_t status;             ///< 0=success, JSDRV_ERROR_* on failure
};

// --- Lifecycle ---

struct js320_jtag_s * js320_jtag_alloc(void);
void js320_jtag_free(struct js320_jtag_s * jtag);

/// Called when the device link opens.  Caches the dev handle.
void js320_jtag_on_open(struct js320_jtag_s * jtag, struct jsdrvp_mb_dev_s * dev);

/// Called when the device is closing.  Aborts any in-progress operation.
void js320_jtag_on_close(struct js320_jtag_s * jtag);

// --- Callbacks delegated from js320_drv ---

/// Handle h/jtag/!aes_cmd.  Returns true if consumed.
bool js320_jtag_handle_cmd(struct js320_jtag_s * jtag,
                            const char * subtopic,
                            const struct jsdrv_union_s * value);

/// Intercept c/jtag/!done publishes during AES operation.  Returns true if consumed.
bool js320_jtag_handle_publish(struct js320_jtag_s * jtag,
                                const char * subtopic,
                                const struct jsdrv_union_s * value);

/// Called when the upper driver timer expires.
void js320_jtag_on_timeout(struct js320_jtag_s * jtag);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_JS320_JTAG_H_ */
