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
from .device_arg import add_brand_argument, add_device_argument, device_select
from .info import format_value
from .metadata import metadata_load


def parser_config(p):
    """Display the current values for a device."""
    add_device_argument(p, 'Optional when only one device is connected.')
    add_brand_argument(p)
    return on_cmd


def values_load(driver, device_path):
    """Load the retained values for an open device.

    :param driver: The active Driver instance.
    :param device_path: The open device path.
    :return: The dict mapping device-relative subtopic to value.
    """
    values = {}
    prefix_len = len(device_path) + 1

    def on_value(topic, value):
        values[topic[prefix_len:]] = value

    driver.subscribe(device_path, 'pub_retain', on_value).unsubscribe()
    return values


def on_cmd(args):
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        device_path = device_select(d, args.device, args.brand)
        if device_path is None:
            return 1
        with d.open(device_path, mode='restore'):
            meta = metadata_load(d, device_path)
            values = values_load(d, device_path)
    print(f'{device_path} values:')
    for subtopic, value in values.items():
        print(f'  {subtopic} = {format_value(meta.get(subtopic), value)}')
    return 0
