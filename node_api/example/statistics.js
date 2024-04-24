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

const JoulescopeDriver = require("joulescope_driver");

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

        var stats_cbk = (topic, value) => {
            topic_parts = topic.split('/');
            console.log(topic_parts[1] + '-' + topic_parts[2]
                + ',' + value['signals']['current']['avg']['value']
                + ',' + value['signals']['voltage']['avg']['value']
                + ',' + value['signals']['power']['avg']['value']
                + ',' + value['accumulators']['energy']['value']);
        }

        if (device_path.includes("/js110/")) {
            drv.publish(device_path.concat("/s/i/range/select"), "auto");
            unsubs.push(drv.subscribe(device_path.concat("/s/sstats/value"), 2, stats_cbk));
        } else {
            drv.publish(device_path.concat("/s/i/range/mode"), "auto");
            drv.publish(device_path.concat("/s/v/range/mode"), "auto");
            unsubs.push(drv.subscribe(device_path.concat("/s/stats/value"), 2, stats_cbk));
            drv.publish(device_path.concat('/s/stats/ctrl'), 1, 0);
        }
    });

    return () => {
        unsubs.forEach((unsub) => unsub());
        device_paths.forEach((device_path) => {
            drv.publish(device_path.concat('/s/stats/ctrl'), 0, 0);
            drv.close(device_path);
        })
        drv.finalize();
    }
}

finalize_cbk = initialize();
process.on('SIGINT', finalize_cbk);
