/*
* Copyright 2023 Jetperch LLC
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
 * @brief Memory buffer for samples from a single signal.
 */

#ifndef JSDRV_PRV_BUFFER_SIGNAL_H_
#define JSDRV_PRV_BUFFER_SIGNAL_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/msg_queue.h"
#include <stdint.h>


struct buffer_s;

struct bufsig_s {
    uint32_t idx;
    bool active;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct buffer_s * parent;

    // todo summary data.
    // todo level 0 data
};

void jsdrv_bufsig_recv_data(struct bufsig_s * self, struct jsdrvp_msg_s * msg);

// implemented by buffer.c
void jsdrv_bufsig_send(struct bufsig_s * self, struct jsdrvp_msg_s * msg);


JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_BUFFER_SIGNAL_H_ */
