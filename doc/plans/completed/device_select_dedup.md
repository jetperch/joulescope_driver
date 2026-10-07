<!--
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
-->
# Replace the entry point device selection helpers with device_filter

**Status**: completed 2026-10-07
**Created**: 2026-10-07

## Context

Found while adding `DevicePath` and `device_filter.find_one`.  Two entry
points still select a device with their own logic:

- `entry_points/metadata.py:device_select` requires an exact device path,
  raises `ValueError`, and accepts non-Joulescope devices such as
  `u/mb/...`.  `test_metadata.py` tests it directly.
- `entry_points/threads.py:_device_get` filters to the JS220 and picks the
  first device when there are several.

Both overlap `device_filter.find_one`, but switching changes behavior:
`find_one` accepts device specifications, such as `js320` or `31NB`,
and ignores non-Joulescope devices.

## Plan

1. Decide if `metadata` must support MiniBitty devices.  If so, add an
   option to `find` and `find_one` to include non-Joulescope devices, such
   as `joulescope_only=True`, with tests.
2. Replace `metadata.device_select` with `device_filter.find_one`, and
   update `test_metadata.py` to test the entry point behavior.
3. Replace `threads._device_get` with `device_filter.find(paths, 'js220')`,
   keeping the select-first behavior, or switch to `find_one`.

Each step builds and passes the unit tests on its own.

## Outcome

`device_filter.find` and `find_one` are now generic, with an optional
`brand` argument, so step 1 needed no `joulescope_only` option.
`Driver.device_paths(specs, brand)` and `Driver.find_one_device(specs,
brand)` provide the selection directly.

- `metadata.device_select` is removed.  `metadata` uses
  `find_one_device(args.device)`, so `--device` now also accepts a model
  or serial number.  `test_device_filter.py` and
  `test_driver_device_paths.py` cover the selection.
- `threads._device_get` uses `device_paths('js220')` and keeps the
  select-first behavior.
