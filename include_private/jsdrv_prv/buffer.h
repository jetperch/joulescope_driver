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
 * @brief Signal sample memory buffer.
 */

#ifndef JSDRV_PRV_BUFFER_H_
#define JSDRV_PRV_BUFFER_H_

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

// Forward declarations from "jsdrv.h"
struct jsdrv_context_s;

#ifndef JSDRV_BUFFER_COUNT_MAX
#define JSDRV_BUFFER_COUNT_MAX                       16
#endif

#ifndef JSDRV_BUFSIG_COUNT_MAX
#define JSDRV_BUFSIG_COUNT_MAX                       256
#endif

// topics for the buffer manager: add/remove buffers
#define JSDRV_BUFFER_MGR_MSG_ACTION_ADD               "m/@/!add"      // u8: 1 <= id <= JSDRV_BUFFER_COUNT_MAX
#define JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE            "m/@/!remove"   // u8 id
#define JSDRV_BUFFER_MGR_MSG_ACTION_LIST              "m/@/list"      // bin ro: u8[N] ids

// topics per buffer: prefix is "m/BBB/" where BBB is the buffer_id
#define JSDRV_BUFFER_MSG_ACTION_SIGNAL_ADD            "a/!add"          // u8 id
#define JSDRV_BUFFER_MSG_ACTION_SIGNAL_REMOVE         "a/!remove"       // u8 id
#define JSDRV_BUFFER_MSG_LIST                         "g/list"          // bin ro: u8[N] ids
#define JSDRV_BUFFER_MSG_SIZE                         "g/size"          // u64 size in bytes
#define JSDRV_BUFFER_MSG_HOLD                         "g/hold"          // u8: 0=run (default), 1=hold, clear on 1->0
#define JSDRV_BUFFER_MSG_MODE                         "g/mode"          // 0:continuous, 1:fill & hold
#define JSDRV_BUFFER_MSG_SIGNAL_TOPIC                 "s/ZZZ/topic"     // str: source data topic
#define JSDRV_BUFFER_MSG_SIGNAL_INFO                  "s/ZZZ/info"      // ro: jsdrv_buffer_info_s
#define JSDRV_BUFFER_MSG_SIGNAL_SAMPLE_REQ            "s/ZZZ/!req"      // jsdrv_buffer_request_s

JSDRV_CPP_GUARD_START

/**
 * @brief Initialize the singleton memory buffer manager.
 *
 * @return 0 or error code.
 */
int32_t jsdrv_buffer_initialize(struct jsdrv_context_s * context);


/**
 * @brief Finalize the singleton buffer manager.
 */
void jsdrv_buffer_finalize();



JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_BUFFER_H_ */
