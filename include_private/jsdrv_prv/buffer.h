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


// topics for the buffer manager
#define JSDRV_BUFFER_MSG_ACTION_ADD                  "m/+/@/!add"
#define JSDRV_BUFFER_MSG_ACTION_REMOVE               "m/+/@/!remove"
#define JSDRV_BUFFER_MSG_ACTION_LIST                 "m/+/@/list"

// topics per buffer: prefix is "m/ZZZ/" where ZZZ is the buffer id
#define JSDRV_BUFFER_MSG_EVENT_SIGNAL_ADD             "e/!add"          // source signal topic
#define JSDRV_BUFFER_MSG_EVENT_SIGNAL_REMOVE          "e/!remove"       // source signal topic or ZZZ
#define JSDRV_BUFFER_MSG_EVENT_SIGNAL_LIST            "e/list"          // [ZZZ, ...]
#define JSDRV_BUFFER_MSG_SIGNAL_DURATION              "s/ZZZ/dur"       // duration_i64
#define JSDRV_BUFFER_MSG_SIGNAL_RANGE                 "s/ZZZ/range"     // start, stop
#define JSDRV_BUFFER_MSG_SIGNAL_SAMPLE_REQ            "s/ZZZ/req/!spl"  // start, stop, rsp_topic, rsp_int64
#define JSDRV_BUFFER_MSG_SIGNAL_SUMMARY_REQ           "s/ZZZ/req/!sum"  // start, stop, incr, rsp_topic, rsp_int64
#define JSDRV_BUFFER_MSG_SIZE                         "g/size"


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
