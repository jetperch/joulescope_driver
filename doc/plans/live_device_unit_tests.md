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
# Move the live device tests out of the Python unit tests

`pyjoulescope_driver/test/test_driver_context.py::TestLiveDevice` opens the
first connected Joulescope whenever one is connected.  The unit tests
therefore depend on the bench: when another program holds that instrument,
the four tests fail with `IN_USE`, and they briefly claim an instrument that
a long-running test may be using.  `test/hw/` holds the tests that need a
device.

Found 2026 Oct 9, while a soak test held every instrument.  Before then, a
failed open in `test_finalize_closes_opened` also leaked its Driver, so the
rest of the run failed with `jsdrv_initialize failed 21 IN_USE` (only one
Driver per process).  That leak is fixed with `addCleanup(d.finalize)`.

## Plan

1. Move `TestLiveDevice` to `test/hw/`, or skip it unless an environment
   variable, such as `JSDRV_TEST_DEVICE`, names the instrument to use.
2. Keep the device-free tests of the same behavior, with `ListDriver` and
   `CloseRecordDriver`, in the unit tests.
