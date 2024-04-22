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

const JoulescopeDriver = require("..");
const assert = require("assert");

const sleep = (delay) => new Promise((resolve) => setTimeout(resolve, delay));

async function testBasic(){
    await sleep(100);
    var drv = new JoulescopeDriver();
    var device_paths = drv.device_paths();
    console.log('device_paths: ' + device_paths);
    if (0 == device_paths.length) {
        return;
    }
    var device_path = device_paths[0]
    drv.open(device_path);
    drv.publish(device_path.concat("/s/i/range/mode"), "auto");
    assert(4 == drv.query(device_path.concat("/s/i/range/mode")));
    drv.publish(device_path.concat("/s/v/range/mode"), "auto");

    var sample_cbk = (topic, value) => {
        // console.log('topic=' + topic + ', sample_id=' + value['sample_id']);
    }
    var stats_cbk = (topic, value) => {
        console.log('topic=' + topic + ', energy=' + value['accumulators']['energy']['value']);
    }

    const s_unsub = drv.subscribe(device_path.concat("/s/stats/value"), 2, stats_cbk);
    const i_unsub = drv.subscribe(device_path.concat("/s/i/!data"), 2, sample_cbk);
    const v_unsub = drv.subscribe(device_path.concat("/s/v/!data"), 2, sample_cbk);
    drv.publish(device_path.concat('/s/stats/ctrl'), 1, 0);
    drv.publish(device_path.concat("/s/i/ctrl"), 1, 0);
    drv.publish(device_path.concat("/s/v/ctrl"), 1, 0);
    await sleep(2000)
    i_unsub();
    v_unsub();
    s_unsub();

    drv.publish(device_path.concat('/s/stats/ctrl'), 0, 0);
    drv.publish(device_path.concat("/s/i/ctrl"), 0, 0);
    drv.publish(device_path.concat("/s/v/ctrl"), 0);
    drv.close(device_path);
    drv.finalize();
}

assert.doesNotThrow(async() => {await testBasic()}, undefined, "testBasic threw an exception");

console.log("Tests passed- everything looks OK!");
