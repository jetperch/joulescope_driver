#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
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

"""Measure the actual scale of the statistics i128 accumulators.

Subscribe to s/stats/value and, for each block, compare the f64 charge and
energy with the raw 128-bit integer accumulators from the same message.
The scale is f64 / int_value, reported as log2 so that 2**-31 shows as -31
and 2**-52 as -52.  Attach a load so the accumulators are nonzero.  The expected result on
every model is 2**-52 (JSDRV_STATISTICS_I128_Q).

Example::

    python tools/stats_i128_scale.py --device js320 --duration 5
"""

import argparse
import math
import time
from pyjoulescope_driver import Driver

FIELDS = ['charge', 'energy']


def get_parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--device', '-d',
                   help='The device specification, such as "js220" or "8W2A".  '
                        'Defaults to the single connected Joulescope.')
    p.add_argument('--duration', type=float, default=5.0,
                   help='The capture duration in seconds.  Defaults to 5.')
    p.add_argument('--scnt', type=int, default=500_000,
                   help='The s/stats/scnt samples per block.  Defaults to 500000.')
    p.add_argument('--verbose', '-v', action='store_true',
                   help='Print every statistics block.')
    return p


def run(args):
    log2_scales = {field: [] for field in FIELDS}
    blocks = []

    def on_statistics(topic, value):
        blocks.append(value)

    with Driver() as d:
        device_path = d.find_one_device(args.device, brand='joulescope')
        print(f'Device: {device_path}')
        with d.open(device_path, mode='defaults') as device:
            if device_path.model == 'js110':
                device.publish('s/i/range/select', 'auto')
                for signal in ['i', 'v', 'p']:
                    device.publish(f's/{signal}/ctrl', 1)  # host-side statistics
            else:
                device.publish('s/i/range/mode', 'auto')
            device.publish('s/stats/scnt', args.scnt)
            device.publish('s/stats/ctrl', 1)
            device.subscribe('s/stats/value', 'pub', on_statistics)
            time.sleep(args.duration)

    for idx, value in enumerate(blocks):
        acc = value['accumulators']
        line = [f'{idx:3d}']
        for field in FIELDS:
            f64 = acc[field]['value']
            i128 = acc[field]['int_value']
            claimed = acc[field]['int_scale']
            if i128 == 0 or f64 == 0.0:
                line.append(f'{field}: f64={f64:.9g} int={i128} (zero, skipped)')
                continue
            ratio = f64 / i128
            if ratio <= 0:
                line.append(f'{field}: f64={f64:.9g} int={i128} SIGN MISMATCH')
                continue
            log2_scale = math.log2(ratio)
            log2_scales[field].append(log2_scale)
            line.append(f'{field}: f64={f64:.9g} int={i128} log2(scale)={log2_scale:.4f} '
                        f'(binding claims {math.log2(claimed):.0f})')
        if args.verbose:
            print('  '.join(line))

    print(f'\n{len(blocks)} statistics blocks')
    rv = 0
    for field in FIELDS:
        values = log2_scales[field]
        if not values:
            print(f'{field}: no nonzero blocks, attach a load')
            rv = 1
            continue
        nearest = round(sum(values) / len(values))
        spread = max(values) - min(values)
        print(f'{field}: int_scale = 2**{nearest}  (log2 mean {sum(values) / len(values):.4f}, '
              f'min {min(values):.4f}, max {max(values):.4f}, blocks {len(values)})')
        if spread > 0.01:
            print(f'{field}: WARNING log2 spread {spread:.4f} exceeds 0.01, scale not constant')
            rv = 1
    return rv


if __name__ == '__main__':
    raise SystemExit(run(get_parser().parse_args()))
