/*
* Copyright 2022 Jetperch LLC
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
 * @brief Frontend device thread.
 */

#ifndef JSDRV_PRV_DEVICE_H_
#define JSDRV_PRV_DEVICE_H_

#include "jsdrv/cmacro_inc.h"

#define JSDRV_MSG_FINALIZE         "@/finalize"

JSDRV_CPP_GUARD_START

struct jsdrvbk_s {
    char prefix;                            // the unique prefix 0-9, a-z, A-Z
    void (*finalize)(struct jsdrvbk_s *);
    struct msg_queue_s * cmd_q;
};

typedef int32_t (*jsdrv_backend_factory)(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

int32_t jsdrv_usb_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

int32_t jsdrv_unittest_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_BACKEND_H_ */
