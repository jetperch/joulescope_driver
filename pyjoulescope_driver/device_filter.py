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

"""Select devices from the driver's device paths.

The driver reports all devices, including Joulescope instruments and
other devices, such as a MiniBitty at "u/mb/{serial_number}".  Use the
brand argument to select only Joulescope instruments.  See
:class:`DevicePath` for the device path and specification formats.

Use this module as a namespace::

    from pyjoulescope_driver import device_filter
    device_path = device_filter.find_one(paths, '31NB', brand='joulescope')

Driver.device_paths() and Driver.find_one_device() provide the same
selection directly.
"""

from .device_path import DevicePath, brand_validate


class DeviceFilterError(ValueError):
    """The device specifications did not match exactly one device.

    :ivar specs: The list of device specifications, or None for all.
    :ivar brand: The brand, such as "Joulescope", or None for all.
    :ivar matches: The list of matching device paths.
    :ivar available: The list of all device paths for the brand.
    """

    def __init__(self, specs, brand, matches, available):
        self.specs = specs
        self.brand = brand
        self.matches = matches
        self.available = available
        noun = brand if brand else 'device'
        name = f'Device "{", ".join(specs)}"' if specs else None
        if not matches:
            msg = f'{name} not found in {available}' if name else f'No {noun} found'
        elif name:
            msg = f'{name} matched multiple {noun}s: {matches}'
        else:
            msg = f'Multiple {noun}s found: {matches}'
        super().__init__(msg)


def _specs_normalize(specs):
    if specs is None:
        return None
    if isinstance(specs, str):
        specs = [spec.strip() for spec in specs.split(',')]
        specs = [spec for spec in specs if spec]
    elif not isinstance(specs, (list, tuple)):
        raise TypeError(f'specs must be str or a list of str, not {type(specs).__name__}')
    elif not all(isinstance(spec, str) for spec in specs):
        raise TypeError('specs must be str or a list of str')
    return list(specs) or None


def _brand_normalize(brand):
    return None if brand is None else brand_validate(brand)


def find(device_paths, specs=None, brand=None):
    """Find the device paths for the matching devices.

    :param device_paths: The list of device path strings, such as
        from Driver.device_paths().
    :param specs: The device specifications, which is one of:

        * None to match all devices.
        * a string containing one or more comma-separated device
          specifications, such as "js320" or "31NB, u/js220/000415".
          Whitespace around each specification is ignored, and an
          empty string matches all devices.
        * a list of device specification strings.

        A device matches if it matches any specification.
        See :meth:`DevicePath.match` for the device specification format.
    :param brand: The case-insensitive brand, such as "Joulescope",
        to only match devices of that brand.  None (default) matches
        all brands.
    :return: The list of matching :class:`DevicePath` instances, in order.
    :raise TypeError: If specs or brand has an invalid type.
    :raise ValueError: If brand is not supported.
    """
    specs = _specs_normalize(specs)
    brand = _brand_normalize(brand)
    paths = [DevicePath(p) for p in device_paths]
    if brand is not None:
        paths = [p for p in paths if p.brand == brand]
    if specs is None:
        return paths
    return [p for p in paths if any(p.match(spec) for spec in specs)]


def find_one(device_paths, specs=None, brand=None):
    """Find the device path for exactly one matching device.

    :param device_paths: The list of device path strings, such as
        from Driver.device_paths().
    :param specs: The device specifications.  See :func:`find`.
    :param brand: The brand.  See :func:`find`.
    :return: The matching :class:`DevicePath`.
    :raise DeviceFilterError: If zero or multiple devices match.
    """
    matches = find(device_paths, specs, brand)
    if len(matches) != 1:
        raise DeviceFilterError(_specs_normalize(specs), _brand_normalize(brand),
                                matches, find(device_paths, brand=brand))
    return matches[0]
