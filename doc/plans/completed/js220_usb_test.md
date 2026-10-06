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

# Unit tests for the JS220 USB device

**Status**: complete
**Created**: 2026-10-06
**Completed**: 2026-10-06

## Context

Found while fixing a `completed/js320_fix.md` low-priority item.  In
`src/devices/js220/js220_usb.c`, `mem_complete` ran
`memset(&d->mem_hdr, 0, sizeof(d->mem_topic))`.  `mem_topic` is larger
than `mem_hdr`, so the memset also zeroed `mem_offset_valid`,
`mem_offset_sent` and the `mem_data` pointer.  The `NULL != d->mem_data`
check that follows never fired, so every memory read or write leaked its
buffer.  It is now `sizeof(d->mem_hdr)`.

No unit test covers `js220_usb.c` on its own.  `frontend_test` links it,
but only to build the device table.  The fix was checked on hardware
with `jsdrv mem_read --device js220 c/pers` (two identical 256-byte
reads), which shows no regression but cannot show the leak.

## Plan

Follow the `js320_drv_test` and `mb_device_test` pattern: include the
device source directly in the test, and stub the backend, message
allocation and USB services.  Each stage touches at most three files and
passes ctest.

1. [x] Add `test/devices/js220/js220_usb_test.c` and register it in
   `test/CMakeLists.txt`.  Stub `jsdrvp_msg_alloc`, `jsdrvp_backend_send`
   and the USB bulk transfer calls.  Count `jsdrv_alloc` and
   `jsdrv_free` to detect leaks.
2. [x] Memory operations (`h/mem/{region}/!read`, `!write`, `!erase`):
   - each completed operation frees `mem_data`, and the alloc and free
     counts match.  This test fails with the old memset.
   - `mem_complete` clears `mem_hdr`, the offsets and `mem_topic`, and
     publishes the return code to `{topic}#`.
   - a read publishes `!rdata` with the received bytes.
   - a new command while one is in progress aborts the active one.
3. [x] Parameter handling already reviewed in `design_review_2026-07.md`
   P2: `h/fs`, `h/filter` and `h/fp` validation, so those fixes stay
   fixed.

## Results

- The test uses the real `msg_queue`: frames to the instrument are popped
  from `d->ll.cmd_q`, and instrument frames are injected with
  `handle_stream_in_frame()`.  `jsdrv_alloc` and `jsdrv_free` are renamed
  only inside the included source, so the counts cover `mem_data` alone.
  `js220_params.c` is linked for the parameter table.
- With the old memset, 9 of the 20 tests fail on the alloc and free
  counts.
- The plan said a new command while one is in progress "is rejected".
  The driver aborts the active operation instead: it publishes
  `JSDRV_ERROR_ABORTED` to the old topic, frees its buffer and starts the
  new one.  The test pins down that behavior.
- New bug, fixed: a `!write` larger than `MEM_SIZE_MAX` called
  `mem_complete()` before any operation started, which returns early, so
  no return code was published and the caller waited for its timeout.
  It now clears `mem_topic` and publishes `JSDRV_ERROR_PARAMETER_INVALID`.
- `test_scale` also covers P2.1: the `h/i_scale$` and `h/v_scale$`
  metadata, and the converted `h/i_scale` and `h/v_scale` values.
- Also fixed: `test/frontend_test.c` used `JSDRV_LOGI` without
  `jsdrv_prv/log.h`, which broke the Linux build (MSVC only warns).
- **HW** (JS220 000043): `jsdrv mem_read` of `c/pers`, `s/cal_a` and
  `s/pers` with `--size 256`, and `pyjoulescope_driver statistics`.
  Without `--size`, `jsdrv mem_read` of a sensor region times out: it
  requests the 512 KB maximum, which takes longer than the example
  timeout.  This happens before this change too.
