<!--
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
-->

# Linux host sleep/resume failure reproduction (Fedora 44 customer report)

Customer (Fedora 44, UI 1.6.3 / jsdrv 2.3.2, JS320 fw 1.1.5): after host
sleep/resume the UI never recovers the JS320 (JS220 recovers).  The UI must
be restarted twice: the first restart shows only the power trace (i and v
blank); the second restart restores all three.  The JS320 LED stays green
through the whole sequence (no watchdog reset), the JS220 blinks red->green
(watchdog reset + re-enumeration).

This plan reproduces that failure on the local dev station, which resumes
cleanly by itself (validated manually 2026-07-16).  The difference between
"works here" and "fails there" is the kernel's resume path for the device:

- Clean resume (this machine): device keeps state, bulk-IN re-arm recovers.
  This is the path the 2026-07-10 Linux fix validated (device-level
  ALLOW_SUSPEND runtime suspend; never a whole-system sleep).
- Reset-resume (suspected on the customer host): xHCI loses context across
  sleep, so on wake the kernel issues a port reset + descriptor re-read
  (no hotplug event).  usbfs has no reset_resume handler, so the kernel
  force-unbinds it: the driver's fd dies (ENODEV -> LIBUSB_ERROR_NO_DEVICE)
  and, because no hotplug ARRIVED ever fires, jsdrv never re-adds the
  device.  Independently, the bus reset wipes the firmware usbd link back
  to AWAIT_REQ, and nothing on Linux replays the handshake (the revalidate
  and replay machinery is winusb-only).

The reproduction forces the reset-resume path on this machine using two
stock kernel facilities, so no customer hardware is required.

## Test hardware and preconditions

- JS320 fw 1.1.5, serial X2VJ, usb 16d0:135a, sysfs 3-5.3, on USB Insight
  Hub CH3.  CH3 power cycling recovers a wedged device between iterations
  (do not touch CH1/CH2: NUCLEO boards for another project).
- jsdrv 2.3.2 (matches the customer's UI 1.6.3) from this repo's
  cmake-build; pyjoulescope_driver in the agent venv for orchestration.
- Root, needed once per configuration change (interactive sudo):
  - Force reset-resume quirk: `sudo sh -c 'echo 0x16d0:0x135a:b >
    /sys/module/usbcore/parameters/quirks'` (`b` = USB_QUIRK_RESET_RESUME).
    Quirks latch at enumeration: power cycle Insight Hub CH3 afterwards.
    Clear with an empty write + another CH3 power cycle.
  - Scripted real sleep: `sudo rtcwake -m mem -s 20` (auto-wake after 20 s).
  - Fallback if real sleep is disruptive: `sudo sh -c 'echo devices >
    /sys/power/pm_test'` then `echo mem > /sys/power/state` runs the full
    suspend path (process freeze + device/bus suspend) and auto-resumes
    after 5 s without platform power-down.  Restore with `echo none`.
    Note: pm_test may NOT produce xHCI context loss, so the quirk is what
    guarantees reset-resume in either mode.

## Instrumentation (evidence to files only)

The host freezes during each sleep, so nothing interactive can observe the
transition.  Every scenario runs as a detached script writing to an
evidence directory; pass/fail comes from files and exit codes, never from
watching output.

1. `stream_watch` client (C, jsdrv API, extend `example/minibitty/` or a
   new example): open `u/js320/X2VJ`, enable s/i, s/v, s/p, and append a
   JSON line per second to the evidence log: wall time, per-channel sample
   count and last sample_id, device add/remove events, publish errors.
   Exits 0 only if the final window meets the expected rate.  C, not
   Python, per HIL tooling convention.
2. Kernel log: `journalctl -k --since <t0>` captured after each iteration.
   The reset-resume signature is `reset high-speed USB device number N
   using xhci_hcd`; the unbind shows as usbfs disconnect.
3. Device reset accounting: read the firmware boot/reset-reason topic
   before and after each iteration (expect no watchdog reset in sleep
   scenarios; expect exactly one in the CE scenario).
4. `lsusb`/sysfs snapshot before/after (devnum change proves re-enumerate;
   same devnum + kernel reset line proves reset-resume).

## Scenarios

Run each 5 times (failure-rate metric, not single-shot).  Power cycle CH3
between iterations to restore a known-good device state.

### S0 — baseline

Stream i/v/p at 1 MSPS for 10 s, clean close.  PASS: all three channels at
full rate, exit 0.  Guards against blaming sleep for an unrelated wedge.

### S1 — control: clean sleep, no quirk

Start stream_watch, `rtcwake -m mem -s 20`, observe 30 s after wake.
Expected on this machine: PASS — stream resumes with no reopen (this is
the validated bulk-IN re-arm path).  If S1 fails, this machine also
reset-resumes and S2's quirk is unnecessary; note and continue.

### S2 — reproduction target: forced reset-resume sleep

Set the `b` quirk, CH3 power cycle, start stream_watch, sleep as in S1.
Expected (duplicates customer symptom 1): after wake the stream never
resumes; jsdrv either holds a dead fd (silent forever) or tears the device
down on NO_DEVICE and never re-adds it (no hotplug ARRIVED); kernel log
shows the xhci reset line; device LED stays green (no watchdog reset,
link parked at AWAIT_REQ).  FAIL(expected) = no samples for 60 s post-wake
while the device is still enumerated with the same devnum.

### S3 — customer symptom 2: the two-restart trace asymmetry

Immediately after an S2 failure, without power cycling the device:
1. kill -9 the dead stream_watch (unclean close, like the customer's UI).
2. Start a fresh stream_watch (restart #1): record per-channel counts for
   10 s.  Customer prediction: p flows, i and v deliver nothing.  Then
   clean close.
3. Start again (restart #2): expect all three channels at full rate.
Also run the sequence at a supported fs below 1 MSPS to exercise both
smart-power reconcile modes (host-computed p vs device-streamed p).
Capture the driver log (ctrl forwards from js320_reconcile_power and any
"sample_id skip" lines) — this pins the still-unconfirmed mechanism.
S3 may also reproduce with no sleep at all (kill -9 mid-stream while the
link is healthy); run that variant first since it is the cheapest.

### S4 — CE regression: unattended recovery must survive any fix

With the bus active and no sleep involved:
1. kill -9 the streaming client.  Expected: firmware comm watchdog resets
   the device within its timeout; re-enumeration (new devnum, hotplug
   fires); boot history shows reset=WATCHDOG; a fresh open then streams.
2. SIGSTOP the client for 30 s, then SIGCONT.  On Linux today this is
   indistinguishable from a crash (bus active, host silent, no SLEEP_REQ):
   expected watchdog reset.  Record as the documented current behavior —
   any future Linux sleep fix (logind PrepareForSleep -> SLEEP_REQ) must
   change the sleep path without changing the kill path.

### S5 — optional: reset-resume without process freeze

Re-run the #197 ALLOW_SUSPEND runtime-suspend harness with the `b` quirk
set: runtime resume then also takes the reset_resume path, while the
client stays schedulable throughout.  Faster to iterate than S1/S2 when
developing the fix; validate the fix against S2 (real sleep) last.

## Success criteria

The failure is duplicated when S1 passes and S2 fails 5/5 with the
customer's signature (dead session, device still enumerated, no watchdog
reset, restart required), and S3 restart #1 shows a per-channel asymmetry
while restart #2 is clean.  If S2 passes with the quirk, the reset-resume
theory is wrong for this failure and the next suspect is the clean-resume
link-desync variant (instrument frame ids around S1).

## Cleanup

Clear `usbcore.quirks`, restore `pm_test` to `none`, power cycle CH3, and
re-run S0 to leave the station in a known-good state.

## Results (2026-07-16, JS320 X2VJ fw 1.1.5, jsdrv 2.3.2)

Tooling: `jsdrv stream_watch` (example/jsdrv/stream_watch.c) + scenario
runner (scratchpad hil/runner.py, evidence dirs with JSON verdicts).

- S0 baseline: PASS.  i/v/p all ~1 MSPS.
- S3 kill -9 + restart at 1 s (inside the watchdog window, emulating the
  post-sleep watchdog-released state): **customer symptom 2 reproduced
  4/4, no sleep involved.**  Restart #1: i and v deliver ~11-32k samples
  (~16 ms after enable) then stop at the SAME sample_id; p streams at
  full rate for the whole session.  Restart #2: all three clean.
- Root cause (instrumented 2026-07-16, host-side, three interacting bugs):
  1. Precondition: the device preserves stream enables across USB
     sessions (fpga_mcu `channel_select`; nothing clears it on
     CONNECT_REQ) and the suspend path releases the watchdog, so an
     unclean death leaves `i/ctrl=1, v/ctrl=1, p/ctrl=0` live into the
     next session.
  2. The mb_device open state-restore, which exists to push defaults
     over exactly such leaked state, is broken: `state_set_send_chunk`
     packs `!state` SET_CMD chunks to the full PAYLOAD_SIZE_MAX_U8, the
     publish wrapper adds topic/header overhead, and every full chunk
     then exceeds PAYLOAD_SIZE_MAX_U32 and is SILENTLY dropped by
     `msg_alloc_send_to_device` ("send_to_device: invalid length 134",
     mb_device.c:1567).  Only the final partial chunk reaches the
     device, so the leaked enables survive open.
  3. The open read-back then seeds the frontend retained values with the
     leaked device state (s/i/ctrl=1, s/v/ctrl=1, s/p/ctrl=0).  When the
     client publishes i=1/v=1/p=1, pubsub.c:578 DEDUPS i and v against
     the retained 1s (returns success, never dispatches to the driver),
     so `ports[5/6].enabled` stay false; only p=1 arrives.
     `js320_reconcile_power` then faithfully sends `i=0, v=0, p=1` to
     the device: the driver itself disables current and voltage and
     enables device-streamed power.  Captured verbatim:
     `DBG handle_stream_ctrl ch=7 enable=1` (ch5/6 never called) then
     `DBG reconcile send s/i/ctrl=0 ... en=001`.
  The brief initial i/v delivery is the leaked stream during the window
  before reconcile's zeros land (varies ~16-320 ms between runs).
  Restart #1's clean close publishes ctrl=0s (which DO differ from the
  retained 1s, so they get through) leaving the device fully zeroed;
  restart #2 therefore sees clean state and works.  Exactly two
  restarts, always.
- S4 CE recovery: PASS.  kill -9 with bus active -> comm watchdog reset,
  re-enumeration (devnum bump) in ~4.5 s, fresh open streams cleanly.
  A restart delayed past the watchdog never sees the S3 wedge (the reset
  cleans it) -- which is why only the sleep path exposes this in the
  field: suspend releases the watchdog, so the leaked state persists.
- S1/S2 (sleep, quirk reset-resume): pending -- needs interactive sudo
  for `rtcwake -m mem -s 20`.  Quirk `0x16d0:0x135a:b` is set and
  latched (device re-enumerated after the write).

## Fix results (2026-07-16, jsdrv 2.3.3 work tree)

All host-side; no firmware change.  Fixes landed:

1. mb_device `!state` SET chunking budgeted against the post-wrap frame
   limit (`PUBLISH_VALUE_SIZE_MAX_U8`); oversized publishes now LOGE.
2. libusb backend: silent (no-hotplug) device death -> proper teardown +
   bounded timed rescan (500 ms x 60) -> re-announce via device_add.
3. mb_device link-silence supervision: >3 s RX silence with keep-alive
   active -> existing revalidate -> handshake replay (OS-agnostic).

Measured after the fixes (same runner, JS320 X2VJ fw 1.1.5):

- S3 kill -9 + 1 s restart: restart #1 streams all three channels at
  full rate, 6/6 runs (symptom 2 GONE).  Open-log check: 59 entries in
  7 chunks, zero "invalid length" drops.
- usbreset (Case C emulation, USBDEVFS_RESET mid-stream): driver logs
  "silent device death (no hotplug)", removes the device, rescan
  re-adds it 500 ms later; fresh open streams (validates fix 2
  end-to-end).  Unplug regression (CH3 power off): hotplug LEFT path
  unchanged, no rescan armed, re-plug re-adds normally.
- S4 CE recovery: kill -9 with active bus still watchdog-resets and
  re-enumerates in ~4.5 s; post-reset open streams.  SIGSTOP 2.5 s
  (below thresholds): no false silence trigger, stream continues.
- Unit: new test/devices/mb_device/mb_device_test.c (5 tests: SET
  chunking incl. pre-fix regression catch, silence->revalidate->pong,
  silence->replay with frame-id reset, keep=0 disable, ST_OPEN-only);
  full ctest 30/30 pass.
- S2 real sleep (rtcwake 20 s, quirk latched, 2026-07-16): PASS.
  Kernel reset-resumed the JS320 on wake ("reset high-speed USB device
  number 114 using xhci_hcd"); the usbfs fd survived (Case B).  Stream
  timeline: sleep window, 2 s of zero samples, then full-rate recovery
  in the SAME session -- the silence supervision fired on the first
  post-thaw tick, revalidated, replayed the handshake, and the fixed
  state restore re-enabled the streams.  ~3 s total outage after wake,
  no client action.  Post-wake reopen also clean.
- S1 control (quirk cleared, fresh enumeration via watchdog reset):
  PASS -- and notably the kernel reset-resumed the JS320 anyway (this
  bench exhibits the customer's resume path natively, quirk or not; a
  brief post-wake buffered-data burst preceded the link death, so the
  silence clock restarted and recovery took ~4-5 s).  Recovery again
  in-session via the supervision ladder; reopen clean.  Implication:
  before these fixes, ANY streaming session on this machine would have
  died across sleep, matching the customer report exactly.
- Bench note: the Aerio Insight Hub controller's CDC wedges across a
  host sleep (serial open/IO hangs; USBDEVFS_RESET on its devnode or a
  replug revives it).  The JS320 tests do not need it: a comm-watchdog
  reset (kill -9 an open session) re-enumerates the JS320 on demand.
- Still pending: customer confirmation on Fedora 44 and macOS
  validation.

Additional driver bug found while instrumenting (affects every open):
`state_set_send_chunk` packs `s/./!state` SET_CMD chunks to the full
PAYLOAD_SIZE_MAX_U8, but the publish wrapper adds topic/header overhead,
so every full chunk exceeds PAYLOAD_SIZE_MAX_U32 and is silently dropped
by `msg_alloc_send_to_device` ("send_to_device: invalid length 134").
Only the final partial chunk of the 59-entry sensor state restore (and
the 14-entry core restore) actually reaches the device.  Harmless on a
clean boot (device already at defaults) but the restore is dead weight
today and will bite anything that relies on it.  Also: a leaked session
adds ~750 ms of CONNECT_REQ retries to open, which can exceed a 1000 ms
open timeout (observed as jsdrv_open rc=11).
