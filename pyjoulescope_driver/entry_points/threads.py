# SPDX-FileCopyrightText: Copyright 2022 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pyjoulescope_driver import Driver
from .device_arg import add_brand_argument, add_device_argument
import threading
import time


_quit = False


def parser_config(p):
    """Threading demonstration for the pyjoulescope_driver.

    For example:
        python -m pyjoulescope_driver --log_level INFO --jsdrv_log_level info threads --threads 10 --duration 10 --timeout 0.1
    """
    p.add_argument('--duration',
                   type=float,
                   default=1.0,
                   help='The duration in seconds to run the threading demonstration.')
    p.add_argument('--threads',
                   type=int,
                   default=10,
                   help='The number of threads to run simultaneously for testing.')
    p.add_argument('--timeout', '-t',
                   type=float,
                   default=0.01,
                   help='The thread processing timeout delay in seconds.')
    add_device_argument(p, 'Defaults to the connected JS220.')
    add_brand_argument(p)
    return on_cmd


def _device_get(d, args):
    # this demonstration uses the h/timeout debug topic, which only the
    # JS220 implements
    devices_paths = [p for p in d.find_devices(args.device, args.brand) if p.model == 'js220']
    if len(devices_paths) == 1:
        return devices_paths[0]
    if len(devices_paths) == 0:
        print('No JS220 devices found (h/timeout is JS220-only)')
        return None
    elif len(devices_paths) > 1:
        device_path = devices_paths[0]
        print(f'Multiple devices found, selecting {device_path}')
        return device_path


def _thread_run(state):
    global _quit
    index = state['index']
    d = state['driver']
    device_path = state['device_path']
    timeout_ms = state['timeout_ms']
    timeout_api = state['timeout_api']
    counter = 0
    while not _quit:
        v64 = timeout_ms | (index << 56) | (counter << 32)
        d.publish(device_path + '/h/timeout', v64, timeout=timeout_api)
        counter += 1
    state['counter'] = counter
    print(f'{index} {counter}')


def on_cmd(args):
    global _quit
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        device_path = _device_get(d, args)
        if device_path is None:
            return 1
        d.open(device_path)
        try:
            d.publish(device_path + '/h/timeout', 100, timeout=0.001)
            print('No timeout when expected')
            return 1
        except TimeoutError:
            pass  # expected

        threads = []
        for idx in range(args.threads):
            state = {
                'name': f'jsdrv {idx}',
                'index': idx,
                'driver': d,
                'device_path': device_path,
                'timeout_ms': int(1000 * args.timeout),
                'timeout_api': args.timeout * args.threads * 2 + 0.25,
                'counter': 0,
            }
            state['thread'] = threading.Thread(name=state['name'], target=_thread_run, args=[state])
            state['thread'].start()
            threads.append(state)

        t_end = time.time() + args.duration
        while time.time() < t_end:
            time.sleep(0.010)

        _quit = True
        counter = 0
        for thread in threads:
            thread['thread'].join()
            counter += thread['counter']
        print(f'Total calls: {counter}')


    return 0
