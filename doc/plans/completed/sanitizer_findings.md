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
# Fix the C unit test sanitizer findings

**Status**: complete
**Created**: 2026-10-07
**Completed**: 2026-10-07

## Context

Found while adding the pubsub rejected-publish tests.  A Debug build with
`-fsanitize=address,undefined` passes 34 of the 37 C unit tests.  The three
failures also occur at `962ca8c`, before that change:

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    cmake --build build-asan && ctest --test-dir build-asan

* `os_test`: LeakSanitizer reports 16 bytes from `jsdrv_alloc` in
  `jsdrv_thread_join` (`src/backend/posix.c:269`).
* `js320_drv_test`: LeakSanitizer reports allocations from `jsdrv_alloc`
  that the test does not free.
* `frontend_test`:
  * UBSan: `src/jsdrv.c:204` passes a null pointer to `memcpy` for an
    empty binary value (size 0).
  * UBSan: `src/devices/js220/js220_usb.c:793` and `:805` left-shift a
    signed `int` out of range (`65536 << 15`, `1 << 31`).
  * LeakSanitizer reports 288 bytes, including the mutex from
    `jsdrv_platform_initialize` (`src/backend/posix.c:429`).

## Plan

1. Fix the UBSan findings: skip the `memcpy` for a zero size, and use
   unsigned shifts in `js220_usb.c`.  Add unit tests where a behavior
   changes.
2. Fix or attribute each leak: library leaks (thread join, platform mutex)
   versus test fixtures that do not free.
3. Add a CI job that builds the C unit tests with the sanitizers on Linux
   and runs ctest, so new findings fail the build.

Each step builds and passes the unit tests on its own.


## Outcome

With `-fno-sanitize-recover=all`, which the CI job uses so that UBSan
findings fail, and a rebuild, the baseline had more findings than listed
above.  All 37 tests now pass with both sanitizers.

Library fixes:

* `jsdrv_thread_join`: the joiner and the helper thread exchange an atomic
  state, and whichever finishes last frees the helper.
* `jsdrv_platform_initialize` allocates the heap mutex once.  Each
  `jsdrv_initialize` previously leaked the old mutex and replaced it while
  other threads could hold it.
* `js320_finalize` releases in-flight port messages, using the context
  saved at open.  A forced removal (`LL_TERMINATED`) closes the device
  without `on_close`, and stream data can still arrive after `on_close`.
* The libusb backend frees its pooled transfers (`transfers_free`) in
  `device_close_all`.
* `jsdrvp_msg_alloc_value` skips the `memcpy` for a zero size.
* `js220_usb.c` stream suspend and resume use unsigned masks.

Test fixture fixes: `buffer_test` (publish now owns and frees the message,
like `jsdrv_pubsub_publish`), `buffer_signal_test` (free the response tmap
copy), `tmap_test`, `js220_stats_test` (signed shifts) and
`js320_fwup_test` (null `memcpy`).

CI: the `sanitizers` job in `.github/workflows/packaging.yml` runs on
ubuntu-latest and gates `publish_python`.

Dynamic analysis on hardware: `cmake-build/example/jsdrv capture` with a
JS320 (8W2A), built with both sanitizers, runs clean for 3 to 15 second
captures at the default rate and at 1 kHz, a current-only capture, and
CTRL-C.  The first run found the two items above.
