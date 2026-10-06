/*
 * SPDX-FileCopyrightText: Copyright 2024-2026 Jetperch LLC
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

const JoulescopeDriver = require("..");
const assert = require("assert");

const sleep = (delay) => new Promise((resolve) => setTimeout(resolve, delay));

async function testBasic(){
    await sleep(100);
    var drv = new JoulescopeDriver();
    var device_paths = drv.device_paths();
    console.log('device_paths: ' + device_paths);
    assert.ok(Array.isArray(device_paths), 'device_paths must be an array');
    device_paths.forEach((p) => assert.ok(p.length > 0, 'device_paths must not contain empty paths'));
    drv.finalize();
    console.log('testBasic passed');
}

// assert.doesNotThrow cannot observe an async rejection; await instead so
// any failure rejects the promise and fails the process with exit code 1.
testBasic().catch((err) => {
    console.error('testBasic FAILED:', err);
    process.exit(1);
});
