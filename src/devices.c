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

#include "jsdrv_prv/devices.h"
#include "jsdrv_prv/frontend.h"
#include <string.h>


// Use python to generate UUIDs
//     import uuid
//     uuid.uuid4()
const struct device_type_s device_types[] = {
    // JS110 app '{576d606f-f3de-4e4e-8a87-065b9fd21eb0}',
    {"js110", {0x576d606f, 0xf3de, 0x4e4e, {0x8a, 0x87, 0x06, 0x5b, 0x9f, 0xd2, 0x1e, 0xb0}}, 0x16D0, 0x0E88, jsdrvp_ul_js110_usb_factory},

    // JS110 bootloader '{09f5f2f2-9725-4bce-9079-5e8184f9d587}',
    {"&js110", {0x09f5f2f2, 0x9725, 0x4bce, {0x90, 0x79, 0x5e, 0x81, 0x84, 0xf9, 0xd5, 0x87}}, 0x16D0, 0x0E87, NULL},

    // JS220 app "{e27188c8-98ff-41de-be50-653324c6b19c}"
    {"js220", {0xe27188c8, 0x98ff, 0x41de, {0xbe, 0x50, 0x65, 0x33, 0x24, 0xc6, 0xb1, 0x9c}}, 0x16D0, 0x10BA, jsdrvp_ul_js220_usb_factory},

    // JS220 updater "{0149f96d-54af-4a91-88a9-1652cb1ecc1f}"
    {"&js220", {0x0149f96d, 0x54af, 0x4a91, {0x88, 0xa9, 0x16, 0x52, 0xcb, 0x1e, 0xcc, 0x1f}}, 0x16D0, 0x10B9, jsdrvp_ul_js220_usb_factory},

    // Minibitty app 7002dee0-4fec-427a-a4c3-cb95603f6c3f,
    // bytes: {0x70, 0x02, 0xde, 0xe0, 0x4f, 0xec, 0x42, 0x7a, 0xa4, 0xc3, 0xcb, 0x95, 0x60, 0x3f, 0x6c, 0x3f}
    {"mb", {0x7002dee0, 0x4fec, 0x427a, {0xa4, 0xc3, 0xcb, 0x95, 0x60, 0x3f, 0x6c, 0x3f}}, 0x16D0U, 0x1359U, jsdrvp_mb_device_factory},

    // JS320 '{0d2d4fac-cf4a-49b2-b961-681a96411685}',
    // bytes: {0x0d, 0x2d, 0x4f, 0xac, 0xcf, 0x4a, 0x49, 0xb2, 0xb9, 0x61, 0x68, 0x1a, 0x96, 0x41, 0x16, 0x85}
    {"js320", {0x0d2d4fac, 0xcf4a, 0x49b2, {0xb9, 0x61, 0x68, 0x1a, 0x96, 0x41, 0x16, 0x85}}, 0x16D0U, 0x135AU, jsdrvp_js320_device_factory},

    {NULL, {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}}, 0, 0, NULL}
};

const struct device_type_s * device_type_lookup(const char * model) {
    for (const struct device_type_s * dt = device_types; dt->model; ++dt) {
        if (0 == strcmp(model, dt->model)) {
            return dt;
        }
    }
    return NULL;
}
