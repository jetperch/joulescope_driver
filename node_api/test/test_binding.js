/*
 * Copyright 2024 Jetperch LLC
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

const JoulescopeDriver = require("../lib/binding.js");
const assert = require("assert");

const sleep = (delay) => new Promise((resolve) => setTimeout(resolve, delay));

async function testBasic(){
    await sleep(100);
    var drv = new JoulescopeDriver();
    var device_paths = drv.device_paths();
    console.log(device_paths);
    if (0 == device_paths.length) {
        return;
    }
    var device_path = device_paths[0]
    drv.open(device_path);
    drv.publish(device_path.concat("/s/i/range/mode"), "auto");
    assert(4 == drv.query(device_path.concat("/s/i/range/mode")));
    drv.publish(device_path.concat("/s/v/range/mode"), "auto");

    var cbk = (topic, value) => {
        console.log(topic)
    }
    const i_unsub = drv.subscribe(device_path.concat("/s/i/!data"), 2, cbk);
    const v_unsub = drv.subscribe(device_path.concat("/s/v/!data"), 2, cbk);
    drv.publish(device_path.concat("/s/i/ctrl"), 1, 0);
    drv.publish(device_path.concat("/s/v/ctrl"), 1, 0);
    console.log("wait")
    await sleep(2000)
    console.log("unsub start")
    i_unsub();
    v_unsub();
    console.log("unsub done")

    drv.publish(device_path.concat("/s/i/ctrl"), 0, 0);
    drv.publish(device_path.concat("/s/v/ctrl"), 0);
    drv.close(device_path);
    console.log("close done")
    drv.finalize();
    console.log("finalize done")
}

assert.doesNotThrow(async() => {await testBasic()}, undefined, "testBasic threw an exception");

console.log("Tests passed- everything looks OK!");
