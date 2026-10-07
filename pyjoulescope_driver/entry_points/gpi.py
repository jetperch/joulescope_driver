# SPDX-FileCopyrightText: Copyright 2022-2023 Jetperch LLC
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
from .device_arg import add_device_argument


def parser_config(p):
    """Display Joulescope general-purpose input values."""
    p.add_argument('--verbose', '-v',
                   action='store_true',
                   help='Display verbose information.')
    add_device_argument(p, 'Defaults to all connected Joulescopes.')
    return on_cmd


def _query_gpi_value(d, device):
    return d.publish_and_wait(
        f'{device}/s/gpi/+/!req', 0,
        f'{device}/s/gpi/+/!value',
        timeout=1.0,
    )


def on_cmd(args):
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        for device in d.find_devices(args.device, brand='joulescope'):
            try:
                d.open(device, 'restore')
            except Exception:
                print(f'{device} unavailable')
                continue
            try:
                gpi = _query_gpi_value(d, device)
                print(f'{device}: 0x{gpi:08x}')
            except Exception:
                print(f'{device} unavailable')
            finally:
                d.close(device)
