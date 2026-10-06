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

"""Select Joulescope instruments from the driver's device paths.

Device paths have the form "{backend}/{model}/{serial_number}", such as
"u/js320/31NB".  The driver also reports other devices, such as a
MiniBitty at "u/mb/{serial_number}", which the Joulescope tools ignore.
"""

JOULESCOPE_MODELS = ('js110', 'js220', 'js320')


def _parts(device_path):
    parts = device_path.lower().split('/')
    if len(parts) != 3:
        return None
    return parts


def device_model(device_path):
    """Get the model for a device path.

    :param device_path: The device path, such as "u/js320/31NB".
    :return: The lowercase model, such as "js320", or None.
    """
    parts = _parts(device_path)
    return None if parts is None else parts[1]


def is_joulescope(device_path):
    """Check if a device path is a Joulescope instrument."""
    return device_model(device_path) in JOULESCOPE_MODELS


def device_match(device_path, spec):
    """Check if a device path matches a user-provided device specification.

    :param device_path: The device path, such as "u/js320/31NB".
    :param spec: The device specification, which is one of:
        * the full device path, such as "u/js320/31NB".
        * the model and serial number, such as "js320/31NB".
        * the model, such as "js320".
        * the serial number, such as "31NB".
        Matching is case-insensitive, and serial numbers must match exactly.
    :return: True on a match, False otherwise.
    """
    parts = _parts(device_path)
    if parts is None or spec is None:
        return False
    spec = spec.lower().strip('/')
    _, model, serial_number = parts
    if spec in (device_path.lower(), f'{model}/{serial_number}', serial_number):
        return True
    return spec == model


def device_filter(device_paths, specs=None):
    """Filter device paths to the matching Joulescope instruments.

    :param device_paths: The list of device paths from Driver.device_paths().
    :param specs: The device specification string, a list of device
        specification strings, or None to match all Joulescopes.
        See :func:`device_match`.
    :return: The list of matching Joulescope device paths, in order.
    """
    paths = [p for p in device_paths if is_joulescope(p)]
    if specs is None:
        return paths
    if isinstance(specs, str):
        specs = [specs]
    if not len(specs):
        return paths
    return [p for p in paths if any(device_match(p, spec) for spec in specs)]
