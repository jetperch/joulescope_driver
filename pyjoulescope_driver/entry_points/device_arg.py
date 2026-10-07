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

"""The shared --device and --brand options for the entry points."""

import argparse
from pyjoulescope_driver import device_filter
from pyjoulescope_driver.device_path import BRANDS_TO_MODELS, BRAND_ALIASES, brand_validate


DEVICE_HELP = ('The device specification: a comma-separated list of device paths, '
               'backend/model, models, model-serial_number or serial numbers, '
               'such as "js320", "u/js320/" or "31NB".')


def add_device_argument(p, help=None, aliases=None):
    """Add the --device option, which sets args.device.

    :param p: The argparse parser.
    :param help: The help text suffix.
    :param aliases: The list of deprecated option names, which remain
        supported but are not shown in the help.
    """
    text = DEVICE_HELP if help is None else f'{DEVICE_HELP}  {help}'
    p.add_argument('--device', '-d', dest='device', help=text)
    for alias in aliases or []:
        p.add_argument(alias, dest='device', help=argparse.SUPPRESS)


def _brand_type(value):
    try:
        return brand_validate(value)
    except ValueError as ex:
        raise argparse.ArgumentTypeError(str(ex)) from None


def add_brand_argument(p):
    """Add the optional --brand option, which sets args.brand.

    Use this option for entry points that support all devices.  args.brand
    defaults to None, which matches all brands.

    :param p: The argparse parser.
    """
    brands = ', '.join([*BRANDS_TO_MODELS, *BRAND_ALIASES])
    p.add_argument('--brand',
                   type=_brand_type,
                   help=f'Only match devices of this case-insensitive brand: {brands}.  '
                        'Defaults to all brands.')


def device_select(driver, specs, brand=None):
    """Select exactly one device, and print the error on failure.

    :param driver: The Driver instance.
    :param specs: The device specifications, usually args.device.
    :param brand: The brand, such as "joulescope", or None for all.
    :return: The DevicePath, or None on failure.
    """
    try:
        return driver.find_one_device(specs, brand=brand)
    except device_filter.DeviceFilterError as ex:
        print(ex)
        if len(ex.matches) > 1:
            print('Use "--device" to specify the desired device.')
        return None
