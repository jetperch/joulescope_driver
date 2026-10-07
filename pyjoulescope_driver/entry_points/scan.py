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


def parser_config(p):
    """Scan for available devices."""
    add_device_argument(p, 'Defaults to all devices.')
    add_brand_argument(p)
    return on_cmd


def on_cmd(args):
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        for device in d.find_devices(args.device, args.brand):
            print(device)
    return 0
