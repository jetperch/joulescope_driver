/*
* Copyright 2014-2022 Jetperch LLC
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
 * @brief Joulescope host driver backend API.
 */

#ifndef JSDRV_PRV_BACKEND_H_
#define JSDRV_PRV_BACKEND_H_

#include "jsdrv/cmacro_inc.h"


#define JSDRV_USBBK_MSG_CTRL_IN                 "!ctrl_in"
#define JSDRV_USBBK_MSG_CTRL_OUT                "!ctrl_out"
#define JSDRV_USBBK_MSG_STREAM_IN_DATA          "s/in/!data"        // produced by ll.  ul must respond to free buffer.
#define JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN     "bulk/in/s/!open"
#define JSDRV_USBBK_MSG_BULK_IN_STREAM_CLOSE    "bulk/in/s/!close"
#define JSDRV_USBBK_MSG_BULK_OUT_DATA           "bulk/out/!data"

JSDRV_CPP_GUARD_START

enum jsdrvbk_status_e {
    JSDRVBK_STATUS_INITIALIZING,
    JSDRVBK_STATUS_READY,
    JSDRVBK_STATUS_FATAL_ERROR,
};


struct jsdrvbk_s {
    char prefix;                            // the unique prefix 0-9, a-z, A-Z
    uint8_t status;                         // jsdrvbk_status_e used by frontend
    void (*finalize)(struct jsdrvbk_s *);
    struct msg_queue_s * cmd_q;
};

typedef int32_t (*jsdrv_backend_factory)(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

int32_t jsdrv_usb_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

int32_t jsdrv_unittest_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

int32_t jsdrv_emulation_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_BACKEND_H_ */
