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

# libusb backend: blocking device open starves other streams

Opening one device stalls the bulk IN streams of every other open device.  The libusb backend
(macOS and Linux) runs the whole device open on its single backend thread, and that same thread
resubmits every device's bulk IN transfers from `on_bulk_in_done()`.  While an open step blocks,
no transfer is resubmitted, so each streaming device can only absorb the delay with its queued
transfers (`BULK_IN_TRANSFER_OUTSTANDING`) plus its own buffer.

The winusb backend does not have this problem: each device has its own thread.


## Evidence

Measured on ci_mac (macOS 27.2.1, Apple silicon) on 2026-11-20 with timing around each
`device_open_inner()` step, while a JS220 and a JS320 streamed i, v and p at full rate:

| Device | Step                                | Time      |
|--------|-------------------------------------|-----------|
| JS220  | `libusb_set_interface_alt_setting`  | ~1 ms     |
| JS320  | `libusb_set_interface_alt_setting`  | ~1.7 ms   |
| JS110  | `libusb_set_interface_alt_setting`  | 52 ms     |
| JS110  | same, first open after enumeration  | 630 ms    |

The first JS110 open after a power cycle dropped 550-570 ms of data on both the JS220 and the
JS320, every time.  With 16 queued transfers (about 64 ms), the 52 ms case leaves little margin;
with the earlier 4 (about 16 ms), every JS110 open during streaming dropped data.  The 630 ms
case also opens the device after `libusb_set_configuration()`, which itself is fast.

Skipping `set_interface_alt_setting` after `set_configuration` is not an option: the JS220 then
failed to connect (`OP_CONNECT timed out`), so the firmware relies on SET_INTERFACE.

Not yet measured on Linux.  The time is probably in the JS110 (FX2) itself, so expect the same.


## Plan

1. **Measure on Linux.**  Repeat the step timing with a JS110 on a Linux host.  Done when the
   table above has Linux rows.
2. **Run blocking open and close steps off the backend thread.**  Move `libusb_open()` through
   `set_interface_alt_setting()` (and the matching close steps) to a worker thread, while the
   backend thread keeps calling `libusb_handle_events*()` and resubmitting transfers.  libusb
   allows synchronous calls on one thread while another handles events.  The device stays in an
   OPENING state until the worker reports back, and the open response is sent from the backend
   thread.  Add a unit test for the state handling with the libusb test harness
   (`libusb_backend_test_harness.md`).
3. **HIL check.**  With a JS220 and a JS320 streaming, power-cycle and open a JS110 10 times:
   no sample gaps on the streaming devices.  ci_mac reproduces this in about 20 s.
