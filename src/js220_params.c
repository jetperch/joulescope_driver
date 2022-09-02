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

#include "jsdrv.h"
#include "jsdrv_prv/frontend.h"


const struct jsdrvp_param_s js220_params[] = {
    {
        .topic = "h/state",
        .meta = "{"
            "\"dtype\": \"u32\","
            "\"brief\": \"The current device state.\","
            // "\"default\": 0,"
            "\"options\": ["
                "[0, \"not present\"],"
                "[1, \"closed\"],"
                "[2, \"opening\"],"
                "[3, \"open\"]],"
            "\"flags\": [\"ro\", \"hide\"]"
        "}",
    },
    {.topic = NULL, .meta = NULL}  // end of list
};
