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
