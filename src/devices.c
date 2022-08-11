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


// Use python to generate UUIDs
//     import uuid
//     uuid.uuid4()
const struct device_type_s device_types[] = {
    // JS110 app '{576d606f-f3de-4e4e-8a87-065b9fd21eb0}',
    {JSDRV_DEVICE_TYPE_JS110_APP, "js110", {0x576d606f, 0xf3de, 0x4e4e, {0x8a, 0x87, 0x06, 0x5b, 0x9f, 0xd2, 0x1e, 0xb0}}, 0x16D0, 0x0E88},

    // JS110 bootloader '{09f5f2f2-9725-4bce-9079-5e8184f9d587}',
    {JSDRV_DEVICE_TYPE_JS110_BL,  "&js110", {0x09f5f2f2, 0x9725, 0x4bce, {0x90, 0x79, 0x5e, 0x81, 0x84, 0xf9, 0xd5, 0x87}}, 0x16D0, 0x0E87},

    // JS220 app "{e27188c8-98ff-41de-be50-653324c6b19c}"
    {JSDRV_DEVICE_TYPE_JS220_APP, "js220", {0xe27188c8, 0x98ff, 0x41de, {0xbe, 0x50, 0x65, 0x33, 0x24, 0xc6, 0xb1, 0x9c}}, 0x16D0, 0x10B9},

    // JS220 updater "{0149f96d-54af-4a91-88a9-1652cb1ecc1f}"
    {JSDRV_DEVICE_TYPE_JS220_BL, "&js220", {0x0149f96d, 0x54af, 0x4a91, {0x88, 0xa9, 0x16, 0x52, 0xcb, 0x1e, 0xcc, 0x1f}}, 0x16D0, 0x10BA},

    {JSDRV_DEVICE_TYPE_INVALID, "----", {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}}, 0, 0}
};
