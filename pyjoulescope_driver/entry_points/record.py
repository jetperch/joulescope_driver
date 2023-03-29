# Copyright 2023 Jetperch LLC
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

from pyjoulescope_driver import Driver, Record, time64
import time


def parser_config(p):
    """Capture streaming samples to a JLS v2 file."""
    p.add_argument('--verbose', '-v',
                   action='store_true',
                   help='Display verbose information.')
    p.add_argument('--duration',
                   type=time64.duration_to_seconds,
                   help='The capture duration in float seconds. '
                        + 'Add a suffix for other units: s=seconds, m=minutes, h=hours, d=days')
    p.add_argument('--frequency', '-f',
                   type=int,
                   help='The sampling frequency in Hz.')
    p.add_argument('--serial_number',
                   help='The serial number of the Joulescope for this capture.')
    p.add_argument('--signals',
                   default='current,voltage',
                   help='The comma-separated list of signals to capture which include '
                        + 'current, voltage, power, current_range, gpi[0], gpi[1], gpi[2], gpi[3], trigger_in. '
                        + 'You can also use the short form i, v, p, r, 0, 1, 2, 3, T '
                        + 'Defaults to current,voltage.')
    p.add_argument('filename',
                   nargs='?',
                   default=time64.filename(),
                   help='The JLS filename to record. '
                        + 'If not provided, construct filename from current time.')
    return on_cmd


def on_cmd(args):
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        if args.serial_number is not None:
            device_paths = [p for p in d.device_paths() if p.lower().endswith(args.serial_number.lower())]
        else:
            device_paths = d.device_paths()
        if len(device_paths) == 0:
            print('Device not found')
            return
        elif len(device_paths) > 1:
            print(f'Selecting {device_paths[0]}')
        device_path = device_paths[0]

        d.open(device_path, 'restore')
        try:  # configure the device
            fs = args.frequency
            if fs is None:
                fs = 2_000_000 if 'js110' in device_path else 1_000_000
            d.publish(f'{device_path}/h/fs', fs)
            if 'js110' in device_path:
                d.publish(f'{device_path}/s/i/range/select', 'auto')
                d.publish(f'{device_path}/s/v/range/select', '15 V')
            else:
                d.publish(f'{device_path}/s/i/range/mode', 'auto')
                d.publish(f'{device_path}/s/v/range/select', '15 V')
                d.publish(f'{device_path}/s/v/range/mode', 'manual')  # as of 2023-03-24, auto not working well
        except Exception:
            print('failed to configure device')
            d.close(device_path)
            return 1

        wr = Record(d, device_path, args.signals)
        print('Start recording.  Press CTRL-C to stop.')
        wr.open(args.filename)
        t_stop = None if args.duration is None else time.time() + args.duration
        try:
            while t_stop is None or t_stop > time.time():
                time.sleep(0.010)
        except KeyboardInterrupt:
            pass
        finally:
            wr.close()
            d.close(device_path)
