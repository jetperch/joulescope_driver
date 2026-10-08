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

**Status**: completed 2026-10-08
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

## Proposal (original)

1. Reproduce each finding with a focused C tool (based on `fuzz.c`)
   that reports a failure rate.
2. Finding 1: arm the drop window on every dwnN or mode change, or have
   the firmware discard its buffered frames when streaming stops.
   Decide which side owns the fix.
3. Finding 2: capture the frame with the driver's stream log to find
   whether it leaves the device stale.
4. Finding 3: publish the effective `h/*` values at open in both modes.
5. Run `fuzz` on the JS320 for 30 minutes per seed with no failures.

## Progress (2026-10-08)

**Repro tool.**  `jsdrv stream_watch --cycles N` stops and restarts the
streams N times, with `--on-ms`, `--off-ms`, `--fs-alt` (alternate `h/fs`
while stopped), `--fs-delay-ms` (delay from stop to the `h/fs` change)
and `--signals`.  Each cycle reports per-channel skips and the first
frame's age as one JSON line.

**Finding 1: fixed (host).**  The root cause was a race, not stale
frames: fuzz stops, changes `h/fs` and restarts within about 1 ms.  The
~3 ms of frames already committed in the device pipeline arrive after
the host switched to the new decimation, and `js320_ack_begin` armed the
drop-until-ack window only while streaming.  It now also arms it when
the family's last frame arrived within `JS320_DWNN_DRAIN_TIME` (500 ms).
A settings replay before streaming (UI hot-plug) still skips the window.
An acked window with no later frames now times out without a warning.
`stream_watch --fs 1000000 --fs-alt 2000 --cycles 20`: 3 of 6 cycles
failed before, 0 of 20 after.

**Finding 2: fixed (gateware).**  `comm_wr.v` set `channel_discard` only
when a word for a disabled channel arrived.  At 1 kHz, si_fwd writes one
word per millisecond, so a disable and enable between two words skipped
the discard.  Maintenance returned the buffer and allocated a new one,
and the rest of the frame committed without its header words, carrying
the buffer's previous `sample_id`.  Maintenance now sets
`channel_discard` when it returns a buffer, so the rest of the frame is
dropped.  `test_disable_enable_between_words` in
`gateware/test/comm_wr` covers it.  Flashed to 8W2A (js320 1.1.11 dev);
fuzz seed 2 now passes this point.

**Finding 3: fixed (host).**  The JS320 driver publishes the effective
`h/fs`, `h/fp`, `h/i_scale` and `h/v_scale` when the `h` replay
completes.  `jsdrvp_mb_dev_host_replay()` now ends on the subscribe
completion and calls the existing `on_instance_synced` hook with `'h'`,
which also completes the open.  Publishing earlier would overwrite host values
before the replay, and an asynchronous replay applied the device's
`s/dwnN/N` only after the publish.  The JS220 publishes `h/fs` and
`h/fp` at connect.  Its `h/fs` default is 2 MHz while i, v and p cap at
1 MHz, so fuzz allows that pair.

4. **Open: the previous stream's tail after a same-rate restart.**  Fuzz
   seed 2 on 8W2A, after about 50 streams: a 1 MHz stop and immediate
   restart (no rate change) delivers the previous stream's in-flight
   tail as the new stream's first message, then a forward gap of about
   0.7 ms.  The frames are correctly labeled, so this is extra old data,
   not a divergence.  `stream_watch --off-ms 1` did not reproduce it in
   20 cycles.  Dropping the tail needs a device boundary for enable,
   such as an `!ack` with the sample_id after the channel_select update
   in fpga_mcu `app.c`, like `dwnN/!ack`.

   MiniBitty confirmed delivery (publish `tracking_id`, response on
   `h/!rsp`) does not provide this boundary.  The response carries only
   the topic, tracking id and return code, with no device time.  The
   fpga_mcu pubsub task (task_id 6) also sends it before the lower
   priority app task (12) writes `channel_select`.  js320_drv forwards
   the stream ctrl topics unconfirmed today.

   **Fix (2026-10-08).**  The fpga_mcu publishes `./{i,v,p}/!ack` with
   `mb_time_counter_u64()` after each `channel_select` update, enable or
   disable.  js320_drv keeps one drop window per channel (`ctrl_ack`).
   It arms the window on an enable that follows a known stop
   (`last_sent_ctrl` 0), and drops frames that end at or before the ack.
   Older firmware lacks these topics, so the host arms only when the
   open metadata declared `s/{i,v,p}/!ack` (mb_device now passes each
   metadata entry to `handle_publish` as `{topic}$`), or an ack arrived
   during the open.  Learning
   from the first ack alone was too late: a start, stop and restart
   within 1 ms of an open arrived before any ack.  Otherwise the host
   behaves as before, with no wait and no timeout.  Lost acks time out
   after `JS320_DWNN_ACK_TIMEOUT` with a warning.

   The soak also found a stop gap: at 5 kHz the stop discarded the
   partial message (up to 2 frames), then delivered the in-flight tail.
   The host now drops i/v/p frames for a disabled port, since the device
   streams only enabled channels.

   The device acks every change, stops too, so the host counts every
   forwarded change in the known state.  Counting only enables let an
   earlier start's ack close a later window during quick 0 ms cycles.
   Changes from the unknown state after open stay uncounted, since the
   device ignores a repeated value.

   Only frames for enabled ports refresh the dwnN drain timer.  A tail
   after a close had armed the dwnN window for the next open's replayed
   values.  On a restore open the device already held those values, so
   no ack came and the first stream lost up to 2 s.

   Validation found that the dwnN window also waited on acks that never
   come.  The device ignores a repeated retained value, so a change of
   only the host factor (`h/fs` 5 to 10 Hz, both dwnN 1000) produced no
   `dwnN/!ack`.  The host then dropped frames until the 2 s timeout,
   while streaming (before 2026-10-08 too) or within 500 ms of a stop.
   The dwnN and gpi dwnN windows now arm only when the device value
   changes.

Hardware after the fixes: `test/hw/test_open_state.py` passes on both
models.  Fuzz for 5 minutes: JS220 clean (43 opens, 144 streams); JS320
fails only on finding 4.

## JS110 (2026-10-08)

Added to this investigation for a Windows report: an access violation in
`Driver.finalize()` after a JS110 streamed (`monitor.py`, Ctrl-C, 7 of
20 runs).  On Linux, `monitor.py` and `measure.py` did not crash in 20
runs each, including under AddressSanitizer.  `fuzz --device
u/js110/001612` under ASan reproduced a heap-use-after-free on every
SIGINT while streaming:

* The JS110 driver thread drained `ll.rsp_q` without a bound, so while
  the stream outpaced processing it never saw FINALIZE.
* `join()` timed out after 10 s and freed the device under the running
  thread, which then used a freed downsampler.

Fixed in all three upper-level drivers: the drain stops every
`JSDRVP_UL_RSP_DRAIN_MAX` messages to check `ul.cmd_q`, and the JS110
and JS220 joins now leak instead of freeing on timeout, like mb_device.
ASan fuzz then passed 8 SIGINT-while-streaming runs (joins return 0).
This is the likely cause of the Windows crash.  It needs confirmation on
Windows.  See `ul_thread_loop_dedup.md`.

The JS110 fuzz also found that an open republished the `h/fs` default
(2 MHz) while the driver kept the last rate, so `h/fs` disagreed with
the stream.  `on_sampling_frequency` now stores the rate.  Release fuzz
on the JS110 then ran 240 s clean (36 opens, 113 streams).  Under ASan,
the publish in progress when SIGINT arrives can time out (rc 11).

## Outcome

30-minute fuzz soaks with `--log-level warning`, release builds:

* JS320 8W2A, with the comm_wr and `!ack` firmware: 185 opens, 842
  streams, no failures.  The only warnings are the expected
  `h/fs` 500000 rejections.
* JS220+ 002122: 215 opens, 889 streams, no failures.
* JS110 001612: 201 opens, 871 streams, no failures.

`doc/js320.json` was regenerated from 8W2A: it adds the three `!ack`
topics and drops the defaults from 12 `!` topics.  The fuzz buffer
requests now use the `m/mem/001/!rsp` response topic, which removed the
`r/t` device_lookup warnings.
