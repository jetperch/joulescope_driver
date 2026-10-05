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


// Host-side parameter metadata, published on open.  Keep this table
// declarative: tools/extractors parse it to document topics offline.
const struct jsdrvp_param_s js320_params[] = {
    {
        .topic = "h/fs",
        .meta = "{"
            "\"dtype\": \"u32\","
            "\"brief\": \"The sampling frequency.\","
            "\"default\": 1000000,"
            "\"options\": ["
                "[1000000, \"1 MHz\"],"
                "[200000, \"200 kHz\"],"
                "[100000, \"100 kHz\"],"
                "[50000, \"50 kHz\"],"
                "[20000, \"20 kHz\"],"
                "[10000, \"10 kHz\"],"
                "[5000, \"5 kHz\"],"
                "[2000, \"2 kHz\"],"
                "[1000, \"1 kHz\"],"  // lowest on-instrument output rate
                "[500, \"500 Hz\"],"
                "[200, \"200 Hz\"],"
                "[100, \"100 Hz\"],"
                "[50, \"50 Hz\"],"
                "[20, \"20 Hz\"],"
                "[10, \"10 Hz\"],"
                "[5, \"5 Hz\"],"
                "[2, \"2 Hz\"],"
                "[1, \"1 Hz\"]"
            "]"
        "}",
    },
    {
        .topic = "h/fp",
        .meta = "{"
            "\"dtype\": \"u32\","
            "\"brief\": \"The approximate sample publish frequency.\","
            "\"default\": 20,"
            "\"options\": ["
                "[100000, \"100 kHz\"],"
                "[50000, \"50 kHz\"],"
                "[20000, \"20 kHz\"],"
                "[10000, \"10 kHz\"],"
                "[5000, \"5 kHz\"],"
                "[2000, \"2 kHz\"],"
                "[1000, \"1 kHz\"],"
                "[500, \"500 Hz\"],"
                "[200, \"200 Hz\"],"
                "[100, \"100 Hz\"],"
                "[50, \"50 Hz\"],"
                "[20, \"20 Hz\"],"
                "[10, \"10 Hz\"],"
                "[5, \"5 Hz\"],"
                "[2, \"2 Hz\"],"
                "[1, \"1 Hz\"]"
            "]"
        "}",
    },
    {
        .topic = "h/i_scale",
        .meta = "{"
            "\"dtype\": \"f32\","
            "\"brief\": \"The current signal scale factor.\","
            "\"default\": 1.0"
        "}",
    },
    {
        .topic = "h/v_scale",
        .meta = "{"
            "\"dtype\": \"f32\","
            "\"brief\": \"The voltage signal scale factor.\","
            "\"default\": 1.0"
        "}",
    },
    {.topic = NULL, .meta = NULL}  // end of list
};
