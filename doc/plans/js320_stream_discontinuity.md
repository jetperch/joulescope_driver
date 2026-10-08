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
# JS320 stream sample_id discontinuities

**Status**: proposed
**Created**: 2026-10-08

## Context

Found by the open-mode fuzz checks in `example/fuzz.c` (see
`completed/open_state_management.md`).  The fuzz now fails a stream when
`sample_id` is not continuous, or when the delivered rate
(`sample_rate / decimate_factor`) differs from the host `h/fs`.  The
JS220+ 002122 passes over 200 streams.  JS320 8W2A (fw 1.1.11) fails
within a few minutes, in both open modes.  The driver logs no
`sample_id skip`, so the frames arrive at the host already
discontinuous.

## Findings

1. **Stale frames after an `h/fs` change while stopped.**  Repro:
   `fuzz --device u/js320/8W2A --random 2` (fails in about 1 minute,
   every run).  Stream at 1 MHz, stop, publish `h/fs` = 2000, then
   start.  The first ~26 frames of each channel step by 1968 ticks per
   123 samples, which is the 1 MHz step (16 ticks per sample).  The host
   labels them with `decimate_factor` 8000 (2 kHz), so about 1.6 s of
   2 kHz time overlaps.  `js320_apply_signal_dwn_n()` arms the
   drop-until-ack window only while streaming, so frames still buffered
   at the old rate pass through when streaming restarts.
2. **A stale s/v frame at stream start.**  Seen at 1 kHz after a 204 ms
   stream: the first s/v frame carried sample_id 49174085642, while the
   device counter was at 850053013775 (about 14 hours later).  The next
   frame was correct, and s/i was unaffected.  Either the device or the
   host emitted a frame from an old buffer.
3. **`h/fs` has no host value after open.**  On both models, a query of
   `h/fs` fails after open until the application publishes it, although
   the device streams at its default rate.  This conflicts with the
   open goal that the host cache reflects the device state.  The fuzz
   skips the rate check in this case.

## Proposal

1. Reproduce each finding with a focused C tool (based on `fuzz.c`)
   that reports a failure rate.
2. Finding 1: arm the drop window on every dwnN or mode change, or have
   the firmware discard its buffered frames when streaming stops.
   Decide which side owns the fix.
3. Finding 2: capture the frame with the driver's stream log to find
   whether it leaves the device stale.
4. Finding 3: publish the effective `h/*` values at open in both modes.
5. Run `fuzz` on the JS320 for 30 minutes per seed with no failures.
