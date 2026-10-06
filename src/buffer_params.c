/*
* SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
* SPDX-License-Identifier: Apache-2.0
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

#include "jsdrv.h"
#include "jsdrv_prv/frontend.h"


// Buffer manager metadata, published on initialize.  Keep this table
// declarative: tools/extractors parse it to document topics offline.
// Topics must match JSDRV_BUFFER_MGR_MSG_ACTION_* in jsdrv_prv/buffer.h.
const struct jsdrvp_param_s buffer_mgr_params[] = {
    {
        .topic = "m/@/!add",
        .meta = "{"
            "\"dtype\": \"u32\","
            "\"brief\": \"Add a memory buffer.\""  // any u8 value between 1 and 16, inclusive
        "}",
    },
    {
        .topic = "m/@/!remove",
        .meta = "{"
            "\"dtype\": \"u32\","
            "\"brief\": \"Remove a memory buffer.\""
        "}",
    },
    {
        .topic = "m/@/list",
        .meta = "{"
            "\"brief\": \"The list of available buffers, 0 terminated.\""
        "}",
    },
    {.topic = NULL, .meta = NULL}  // end of list
};

// Per-buffer metadata, published when the buffer is added.
// "{buf}" expands to the 3-digit buffer id.  Topics must match
// JSDRV_BUFFER_MSG_* in jsdrv_prv/buffer.h.
const struct jsdrvp_param_s buffer_params[] = {
    {
        .topic = "m/{buf}/a/!add",
        .meta = "{"
            "\"dtype\": \"u8\","
            "\"brief\": \"Add a signal to this buffer.\","
            "\"detail\": \"The signal id, 1 to 254.  Then publish the source data topic to s/{sig}/topic.\""
        "}",
    },
    {
        .topic = "m/{buf}/a/!remove",
        .meta = "{"
            "\"dtype\": \"u8\","
            "\"brief\": \"Remove a signal from this buffer.\""
        "}",
    },
    {
        .topic = "m/{buf}/g/!clear",
        .meta = "{"
            "\"brief\": \"Clear the buffer contents.\""
        "}",
    },
    {
        .topic = "m/{buf}/g/list",
        .meta = "{"
            "\"dtype\": \"bin\","
            "\"brief\": \"The list of active signal ids, 0 terminated.\","
            "\"flags\": [\"ro\"]"
        "}",
    },
    {
        .topic = "m/{buf}/g/size",
        .meta = "{"
            "\"dtype\": \"u64\","
            "\"brief\": \"The buffer memory size in bytes.\","
            "\"detail\": \"Changing the size clears the buffer.  0 releases the buffer memory.\""
        "}",
    },
    {
        .topic = "m/{buf}/g/hold",
        .meta = "{"
            "\"dtype\": \"bool\","
            "\"brief\": \"Hold the buffer contents.\","
            "\"detail\": \"When true, discard new samples.  The buffer clears on the true to false transition.\","
            "\"default\": 0"
        "}",
    },
    {.topic = NULL, .meta = NULL}  // end of list
};

// Per-signal metadata, published when the signal is added.
// "{buf}" and "{sig}" expand to the 3-digit buffer and signal ids.
const struct jsdrvp_param_s bufsig_params[] = {
    {
        .topic = "m/{buf}/s/{sig}/topic",
        .meta = "{"
            "\"dtype\": \"str\","
            "\"brief\": \"The source data topic for this signal.\""
        "}",
    },
    {
        .topic = "m/{buf}/s/{sig}/info",
        .meta = "{"
            "\"dtype\": \"bin\","
            "\"brief\": \"The signal buffer information, jsdrv_buffer_info_s.\","
            "\"flags\": [\"ro\"]"
        "}",
    },
    {
        .topic = "m/{buf}/s/{sig}/!req",
        .meta = "{"
            "\"dtype\": \"bin\","
            "\"brief\": \"Request samples or summary data, jsdrv_buffer_request_s.\","
            "\"detail\": \"The response is published to the request's rsp_topic.\""
        "}",
    },
    {.topic = NULL, .meta = NULL}  // end of list
};
