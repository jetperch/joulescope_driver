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


function initialize(){
    const drv = new JoulescopeDriver();
    const device_paths = drv.device_paths();
    console.log('device_paths: ' + device_paths);
    if (0 == device_paths.length) {
        drv.finalize()
        return null;
    }
    let unsubs = [];

    device_paths.forEach((device_path) => {
        drv.open(device_path);

        var sample_cbk = (topic, value) => {
            // Modify this code to do whatever you want with the data
            const avg = value['data'].reduce((accumulator, currentValue) =>
                accumulator + currentValue, 0) / value['data'].length;
            console.log(topic + ' : ' + avg);
        }

        if (device_path.includes("/js110/")) {
            drv.publish(device_path.concat("/s/i/range/select"), "auto");
            drv.publish(device_path.concat("/s/v/range/select"), "15 V");
        } else {
            drv.publish(device_path.concat("/s/i/range/mode"), "auto");
            drv.publish(device_path.concat("/s/v/range/mode"), "auto");
        }

        unsubs.push(drv.subscribe(device_path.concat("/s/i/!data"), 2, sample_cbk));
        unsubs.push(drv.subscribe(device_path.concat("/s/v/!data"), 2, sample_cbk));
        drv.publish(device_path.concat("/s/i/ctrl"), 1, 0);
        drv.publish(device_path.concat("/s/v/ctrl"), 1, 0);
    });

    return () => {
        unsubs.forEach((unsub) => unsub());
        device_paths.forEach((device_path) => {
            drv.publish(device_path.concat('/s/i/ctrl'), 0, 0);
            drv.publish(device_path.concat('/s/v/ctrl'), 0);
            drv.close(device_path);
        })
        drv.finalize();
    }
}

finalize_cbk = initialize();
process.on('SIGINT', finalize_cbk);
