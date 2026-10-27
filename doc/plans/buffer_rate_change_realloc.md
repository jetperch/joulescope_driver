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
# Resize the stream buffer on a sample rate change

**Status**: proposed (needs a design decision)
**Created**: 2026-10-26

## Problem

`buffer_alloc()` (`src/buffer.c`) sizes each signal's sample count `N` from
the signal's sample rate when streaming starts: the buffer's byte budget is
split across all active signals by their sample rates.  A later sample rate
change keeps `N`, so the buffer's time span changes by the rate ratio:

- Start at 1 MHz, switch to 10 kHz: the span grows 100x.  Harmless.
- Start at 10 kHz, switch to 1 MHz: the span shrinks 100x.  On a JS220,
  `N = 131072` holds 13 s at 10 kHz but only 0.13 s at 1 MHz, so the
  waveform and any export from the live view see 0.13 s of data.

Found by the UI release tests (pyjoulescope_ui
`docs/plans/expand_test_coverage_202610.md`, Stage 1).  The related defect,
where a rate decrease dropped every new sample, is fixed in
`jsdrv_bufsig_recv_data()` by restarting the signal buffer on a rate change.

## Options

1. **Re-run `buffer_alloc()` on any signal rate change.**  Correct sizing,
   but the buffer is shared by every device, so one device's rate change
   discards all devices' buffered data.  Each of a device's signals
   (i, v, p, ...) changes rate in a separate message, so this must wait for
   all of them before allocating, as `ST_AWAIT` does at start.
2. **Re-allocate only the changed signal**, within its share of the byte
   budget at the new rate.  Keeps other devices' data, but the budget split
   then differs from a fresh start.
3. **Leave the driver as is; the UI restarts streaming** after a signal
   frequency change.  Simplest, but every client of the buffer must do it.

Recommendation: option 2, so a rate change on one device never discards
another device's data.

## Tests

Extend `test/buffer_signal_test.c` / `test/buffer_test.c`: start at a low
rate, switch to a high rate, and check the buffer span in seconds matches a
fresh start at the high rate.
