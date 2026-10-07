..
   SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
   SPDX-License-Identifier: Apache-2.0

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

.. _device_selection:

Device selection
================

Driver.device_paths(), Driver.find_devices() and Driver.find_one_device()
return :class:`DevicePath` instances, which are str instances that also
parse the device path fields.  The device specifications select devices by
device path, model, model-serial_number or serial number, such as
"u/js320/8W2A", "js320", "js320-8W2A" or "8W2A".  The optional brand,
such as "Joulescope", restricts the selection to that brand's devices.

.. autoclass:: pyjoulescope_driver.device_path.DevicePath
    :members:

.. automodule:: pyjoulescope_driver.device_path
    :members: BRANDS_TO_MODELS, BRAND_ALIASES, brand_validate

.. automodule:: pyjoulescope_driver.device_filter
    :members:
