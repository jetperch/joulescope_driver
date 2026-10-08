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
# Upper-level device thread loop deduplication

**Status**: proposed
**Created**: 2026-10-08

## Context

`js110_usb.c`, `js220_usb.c` and `mb_device.c` each implement the same
upper-level device thread: wait on `ul.cmd_q` and `ll.rsp_q`, drain
`ul.cmd_q`, drain `ll.rsp_q`, and exit on FINALIZE.  Each also has a
`join()` that sends FINALIZE, joins with a 10 s cap and frees the device.

The copies drifted, and both differences caused a crash:

* The `ll.rsp_q` drain was unbounded in all three.  A stream that
  outpaced processing (the JS110 under ASan, likely a slow Windows host)
  never let the thread see FINALIZE.  Fixed 2026-10-08 with
  `JSDRVP_UL_RSP_DRAIN_MAX` in all three.
* Only mb_device refused to free the device when the join timed out.
  The JS110 and JS220 freed it under the running thread, a
  heap-use-after-free in `jsdrv_downsample_add_i64q30`.  Fixed
  2026-10-08 by copying the mb_device guard.

## Proposal

Extract a shared helper in `src/devices/` (for example
`jsdrvp_ul_thread_run(d, handle_cmd, handle_rsp, timeout_fn)` and
`jsdrvp_ul_join(thread, cmd_q, prefix)`), and use it in all three
drivers.  Keep each driver's timed work (JS110 status polling, mb_device
keepalive) as a callback.  Unit test the bounded drain and the
leak-on-timeout join once, in the helper.
