#!/usr/bin/env python3
# Copyright 2025 Jetperch LLC
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

"""Development build, firmware update, and time sync test."""

import argparse
import os
import subprocess
import sys
import time

REPO_HOST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_MINIBITTY = os.path.normpath(
    os.path.join(REPO_HOST, '..', 'minibitty'))
REPO_JS320 = os.path.normpath(
    os.path.join(REPO_HOST, '..', 'js320'))
HOST_BUILD_DIR = os.path.join(REPO_HOST, 'cmake-build')
FIRMWARE_PATH = os.path.join(
    REPO_JS320, 'mbbuild', 'js320_p1_0_0_app',
    'js320_p1_0_0_app.mbfw')
MINIBITTY_EXE = os.path.join(
    HOST_BUILD_DIR, 'example', 'Debug', 'minibitty.exe')


def _run(args, cwd=None, env=None):
    """Run a command, printing it first."""
    cmd_str = ' '.join(args) if isinstance(args, (list, tuple)) else args
    print(f'  > {cmd_str}')
    result = subprocess.run(
        args, cwd=cwd, env=env,
        capture_output=True, text=True)
    if result.stdout:
        for line in result.stdout.strip().splitlines():
            print(f'    {line}')
    if result.returncode != 0:
        if result.stderr:
            for line in result.stderr.strip().splitlines():
                print(f'    ERR: {line}')
        raise RuntimeError(
            f'Command failed with rc={result.returncode}: {cmd_str}')
    return result


def build_firmware():
    """Build the JS320 controller firmware."""
    print('=== Building JS320 firmware ===')
    env = os.environ.copy()
    env['PYTHONPATH'] = REPO_MINIBITTY
    _run([sys.executable, '-m', 'pyminibitty', 'product',
          'generate', 'product.js320'],
         cwd=REPO_JS320, env=env)
    _run([sys.executable, '-m', 'pyminibitty', 'product', 'build'],
         cwd=REPO_JS320, env=env)
    _run([sys.executable, '-m', 'pyminibitty', 'product', 'package'],
         cwd=REPO_JS320, env=env)
    print(f'  Firmware: {FIRMWARE_PATH}')
    if not os.path.isfile(FIRMWARE_PATH):
        raise FileNotFoundError(FIRMWARE_PATH)
    print('  OK')


def build_host():
    """Build the host C library and run unit tests."""
    print('=== Building host (C) ===')
    _run(['cmake', '--build', '.'], cwd=HOST_BUILD_DIR)
    _run(['ctest', '.', '--output-on-failure'], cwd=HOST_BUILD_DIR)
    print('=== Building host (Python) ===')
    _run([sys.executable, 'setup.py', 'build_ext', '--inplace'],
         cwd=REPO_HOST)
    print('  OK')


def firmware_update():
    """Update the JS320 controller firmware."""
    print('=== Updating firmware ===')
    _run([MINIBITTY_EXE, 'firmware', 'update',
          '-i', '0', FIRMWARE_PATH])
    print('  Waiting for device to reboot...')
    time.sleep(5)
    print('  OK')


def test_timesync():
    """Test time synchronization using pyjoulescope_driver."""
    print('=== Testing time synchronization ===')

    # Import here so build steps can run without the Python bindings
    sys.path.insert(0, REPO_HOST)
    from pyjoulescope_driver import Driver

    with Driver() as d:
        devices = d.device_paths()
        print(f'  Devices: {devices}')
        if not devices:
            raise RuntimeError('No devices found')

        device = devices[0]
        print(f'  Using: {device}')
        d.open(device)
        time.sleep(1)  # allow initial timesync exchange

        results = []

        def on_stats(topic, value):
            tm = value['time']['time_map']
            results.append(tm)

        d.publish(f'{device}/s/i/range/mode', 'auto')
        scnt = 1_000_000  # 1 second at 1 MHz sample rate
        d.publish(f'{device}/s/stats/scnt', scnt)
        d.publish(f'{device}/s/stats/ctrl', 1)
        d.subscribe(f'{device}/s/stats/value', 'pub', on_stats)

        print('  Collecting statistics for 15 seconds...')
        t_start = time.time()
        while (time.time() - t_start) < 15:
            time.sleep(0.5)
            if results:
                tm = results[-1]
                rate = tm['counter_rate']
                print(f'    counter_rate={rate:.1f} Hz, '
                      f'offset_time={tm["offset_time"]}, '
                      f'offset_counter={tm["offset_counter"]}')
                if rate > 0:
                    break

        d.close(device)

    if not results:
        print('  FAIL: No statistics received')
        return 1

    final = results[-1]
    rate = final['counter_rate']
    print(f'\n  Final time_map:')
    print(f'    counter_rate  = {rate:.1f} Hz')
    print(f'    offset_time   = {final["offset_time"]}')
    print(f'    offset_counter = {final["offset_counter"]}')

    if rate > 0:
        print('\n  PASS: time synchronization working')
        return 0
    else:
        print('\n  FAIL: counter_rate is still 0')
        return 1


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('steps', nargs='*',
                   default=['firmware', 'host', 'update', 'test'],
                   choices=['firmware', 'host', 'update', 'test'],
                   help='Steps to run (default: all)')
    args = p.parse_args()

    steps = {
        'firmware': build_firmware,
        'host': build_host,
        'update': firmware_update,
        'test': test_timesync,
    }

    for step in args.steps:
        steps[step]()

    print('\nDone.')


if __name__ == '__main__':
    main()
