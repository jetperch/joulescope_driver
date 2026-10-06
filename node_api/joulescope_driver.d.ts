/*
 * SPDX-FileCopyrightText: Copyright 2025 Jetperch LLC
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

declare module 'joulescope_driver' {

    export interface Value {
        data: number[]
        decimate_factor: number
        field_id: number
        index: number
        sample_id: number
        sample_rate: number
        time_map: {
            offset_time: number
            offset_counter: number
            counter_rate: number
        }
        utc: number
    }

    class JoulescopeDriver {
        constructor()

        publish(topic: string, value: any, timeout?: number): void

        query(topic: string, timeout?: number): any

        finalize(): void

        device_paths(timeout?: number): string[]

        open(topic: string, mode?: number): void

        close(topic: string): void

        subscribe(
            topic: string,
            flags: number,
            fn: (topic: string, value: Value) => void,
            timeout?: number
        ): () => void
    }

    export default JoulescopeDriver
}
