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

const addon = require('../build/Release/joulescope_driver_native');


class JoulescopeDriver {
    constructor() {
        this.jsdrv = new addon.JoulescopeDriver()
    }

    /**
     * Publish a value to a topic.
     *
     * @param topic The topic string.
     * @param value The new value for the topic.
     * @param timeout The optional integer timeout in milliseconds.
     *      -1 (default) use the default timeout value.
     *      Provide 0 for async operation.
     * @returns Undefined.
     */
    publish(topic, value, timeout=-1) {
        return this.jsdrv.publish(topic, value, timeout);
    }

    /**
     * Query the current value for a topic.
     *
     * @param topic The topic string.
     * @param timeout The optional integer timeout in milliseconds.
     *      -1 (default) use the default timeout value.
     *      Provide 0 for async operation.
     * @returns The value for topic.
     */
    query(topic, timeout = -1) {
        return this.jsdrv.query(topic, timeout);
    }

    /**
     * Finalize the driver instance and free all resources.
     */
    finalize() {
        return this.jsdrv.finalize();
    }

    device_paths(timeout=-1) {
        var p = this.query("@/list");
        return p.split(',').sort()
    }

    /**
     * Open a Joulescope device.
     *
     * @param topic The device path to open.
     * @param mode The optional open mode. 0=defaults, 1=resume.
     */
    open(topic, mode=0) {
        topic = topic.concat("/@/!open");
        return this.jsdrv.publish(topic, mode, 2000);
    }

    /**
     * Close a Joulescope device.
     *
     * @param topic The device path to close that was previously openned.
     */
    close(topic) {
        topic = topic.concat("/@/!close");
        return this.jsdrv.publish(topic, 0, 2000);
    }

    /**
     * Subscribe to a topic.
     *
     * @param topic
     * @param flags
     * @param fn
     * @param timeout
     * @returns Callable to unsubscribe.
     */
    subscribe(topic, flags, fn, timeout=-1) {
        return this.jsdrv.subscribe(topic, flags, fn, timeout);
    }
}

module.exports = JoulescopeDriver
