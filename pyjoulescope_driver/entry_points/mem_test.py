# Copyright 2026 Jetperch LLC
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

"""Hardware-in-the-loop memory regression test."""

from pyjoulescope_driver import Driver
from pyjoulescope_driver.mem_client import (
    MemClient, FLASH_BLOCK_64K,
)
import time


NAME = 'mem_test'


def _parse_int(value):
    return int(value, 0)


def parser_config(p):
    """Hardware-in-the-loop memory regression test."""
    p.add_argument('topic',
                   help='Memory command sub-topic '
                        '(e.g. s/flash/!cmd).')
    p.add_argument('target', type=int,
                   help='Target memory region ID.')
    p.add_argument('offset', type=_parse_int,
                   help='Flash byte offset (e.g. 0x140000).')
    p.add_argument('size', type=_parse_int,
                   help='Test region size in bytes '
                        '(e.g. 0x10000).')
    p.add_argument('--device', '-d', default=None,
                   help='Device path filter. '
                        'Default uses first device found.')
    return on_cmd


def _erase_region(mem, offset, size):
    start = offset & ~(FLASH_BLOCK_64K - 1)
    end = (offset + size + FLASH_BLOCK_64K - 1) & ~(FLASH_BLOCK_64K - 1)
    for addr in range(start, end, FLASH_BLOCK_64K):
        mem.erase_64k(addr)


def _verify_erased(mem, offset, size):
    data = mem.read(offset, size)
    expected = b'\xff' * size
    if data != expected:
        for i, (a, b) in enumerate(zip(data, expected)):
            if a != b:
                print(f'  FAIL: offset 0x{offset + i:06X} '
                      f'expected 0xFF, got 0x{a:02X}')
                return False
    return True


def _make_pattern(size):
    return bytes(i & 0xFF for i in range(size))


def _verify_pattern(mem, offset, size, pattern):
    data = mem.read(offset, size)
    if data != pattern:
        for i, (a, b) in enumerate(zip(data, pattern)):
            if a != b:
                print(f'  FAIL: offset 0x{offset + i:06X} '
                      f'expected 0x{b:02X}, got 0x{a:02X}')
                return False
    return True


def on_cmd(args):
    topic = args.topic
    target = args.target
    offset = args.offset
    size = args.size

    print(f'mem_test: topic={topic} target={target} '
          f'offset=0x{offset:06X} size=0x{size:X}')

    with Driver() as driver:
        driver.log_level = args.jsdrv_log_level
        devices = driver.device_paths()
        if not devices:
            print('ERROR: no devices found')
            return 1

        device = args.device
        if device is None:
            device = devices[0]
        elif device not in devices:
            print(f'ERROR: device {device} not found')
            print(f'  available: {devices}')
            return 1

        driver.open(device, mode='restore')
        time.sleep(0.5)
        try:
            with MemClient(driver, device, topic, target) as mem:
                # Step 1: erase
                print('Step 1: erase...')
                _erase_region(mem, offset, size)

                # Step 2: verify erased
                print('Step 2: verify erased...')
                if not _verify_erased(mem, offset, size):
                    print('FAIL: erase verification')
                    return 1
                print('  OK')

                # Step 3: write pattern
                print('Step 3: write pattern...')
                pattern = _make_pattern(size)
                mem.write_pages(offset, pattern)

                # Step 4: verify pattern
                print('Step 4: verify pattern...')
                if not _verify_pattern(mem, offset, size, pattern):
                    print('FAIL: pattern verification')
                    return 1
                print('  OK')

                # Step 5: erase again
                print('Step 5: erase...')
                _erase_region(mem, offset, size)

                # Step 6: verify erased again
                print('Step 6: verify erased...')
                if not _verify_erased(mem, offset, size):
                    print('FAIL: final erase verification')
                    return 1
                print('  OK')

                print('PASS')
                return 0
        finally:
            driver.close(device)
