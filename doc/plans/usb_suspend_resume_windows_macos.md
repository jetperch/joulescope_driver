<!--
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
-->

# USB suspend/resume: Windows and macOS validation

Host-side recovery of an open, streaming device across a host sleep/resume (and clean handling of
real removal) is fixed and validated on Linux.  This plan covers carrying that to the other two
platforms.  It must be run on real hardware on each target OS; none of it can be validated on the
Linux dev host.

## Status

| Platform | Backend | Code change | Validated |
|---|---|---|---|
| Linux | `libusb/backend.c` + `linux_usbfs.c` | landed | yes, `stream-live` 6/6 |
| macOS | `libusb/backend.c` + `darwin_usb.c` | none needed (see below) | no |
| Windows | `winusb/backend.c` (native, not libusb) | landed, built | tested 2026-07-13: **insufficient**, see measured behavior |

## The model, from Linux (what "works well" means)

An open streaming client survives a host sleep/resume with **no close/reopen**: the stream stops at
suspend and resumes at full rate on wake, same `devnum`, no device reset.  A real removal tears the
device down promptly.  The mechanism: a suspend fails the in-flight bulk IN transfers, and the pipe
is re-armed on a delay with exponential backoff until it either completes (device back) or is found
gone.  The measured Linux facts are recorded in the `reference_libusb_suspend_vs_removal` memory and
in the `backend.c` block comment above `BULK_IN_RETRY_DELAY_MS`.

## macOS — expected no code change; validate the assumption

macOS uses the **same** `src/backend/libusb/backend.c` that was fixed for Linux, over the
`darwin_usb.c` OS layer.  Inspection of `darwin_usb.c` shows the retry classification already fits:

- Completion side: suspend and removal both map to `LIBUSB_TRANSFER_ERROR`
  (`darwin_transfer_status` default case), same as Linux.  Our completion handler schedules a retry.
- Submit side (the discriminator): a removed device -> `kIOReturnNoDevice` -> `LIBUSB_ERROR_NO_DEVICE`
  (`darwin_to_libusb`), which our retry treats as "gone -> tear down".  A suspended/unresponsive
  device -> `LIBUSB_ERROR_OTHER`, which is not `NO_DEVICE`, so our retry keeps trying.  The
  classification is deliberately "NO_DEVICE means gone, anything else keep retrying", so it does not
  depend on the platform-specific suspend code (`EHOSTUNREACH`/`IO` on Linux vs `OTHER` on macOS).
- macOS also delivers removal out-of-band via the IOKit `kIOTerminatedNotification` ->
  `usbi_disconnect_device` hotplug path, the analog of Linux hotplug LEFT.

So the fix should already work on macOS.  Validate:

1. Build pyjoulescope_driver on macOS (`python setup.py build_ext --inplace`).
2. With a Joulescope streaming and a client open, put the Mac to sleep and wake it.  Induce sleep
   with `pmset sleepnow` (or the real lid/idle path); there is no macOS equivalent of Linux
   usbfs `ALLOW_SUSPEND`, so this is a whole-system sleep, and the process is frozen during it.
3. Confirm from `Driver().log_level = 'ALL'` that after wake the bulk IN completions resume with data
   and the stream returns to full rate **without reopening**, same session.
4. Confirm a physical unplug (or a powered-hub port off) tears the device down: hotplug disconnect
   fires and `bulk_in` submit returns `LIBUSB_ERROR_NO_DEVICE`.
5. Watch for a retry storm during the sleep transition (unlikely, since the process is frozen while
   asleep) and for any wake-latency samples loss beyond the ~100 ms backoff cap.

If any of 3-5 fails, the darwin OS layer behaves differently than inspected; capture the actual
`IOReturn`/`libusb_transfer_status` and submit rc and re-tune, but prefer changing only the shared
retry constants, not per-platform branches.

## Windows — code landed, must be built and validated

`winusb/backend.c` is a native WinUSB backend, independent of libusb.  The same dead-pipe bug was
present: a failed overlapped `WinUsb_ReadPipe` freed the transfer and returned without re-pending,
so a host sleep/resume left the pipe dead until close/reopen.  The change mirrors Linux: on any read
failure other than `ERROR_IO_PENDING`/`ERROR_IO_INCOMPLETE`/`ERROR_SEM_TIMEOUT`, schedule a throttled
re-arm (`bulk_in_retry`, backoff `BULK_IN_RETRY_DELAY_MS` -> `..._MAX_MS`) driven by the device
thread's timed wakeup; a completed read clears the backoff; a real removal is retired out-of-band by
the existing `WM_DEVICECHANGE` -> `device_scan` sweep.

This code was built and run on-target 2026-07-13 (Joulescope UI, JS320).  The re-arm behaves as
designed but is **not sufficient** on Windows; see the measured behavior and revised design below.

### The one assumption that must be measured first

The design assumes host sleep/resume surfaces as a *read completion failure* that reaches
`bulk_in_process`.  It does **not** assume any specific `GetLastError` code -- every failure except
the three benign codes above is retried, including `ERROR_OPERATION_ABORTED`, because our own close
drains its aborts inside `bulk_in_finalize` with the endpoint already off the active list and so
never reaches `bulk_in_process`.  Still, capture the actual code:

1. Instrument the `else` branch of `bulk_in_process` to log `ec` (already logged at `JSDRV_LOGD1`;
   raise to a visible level for the test).
2. Stream with a client open, sleep the machine (Start > Sleep, or `rundll32
   powrprof.dll,SetSuspendState 0,1,0`), wake it, and record the `ec` values seen at wake.
3. Sanity-check the descriptors that are supposed to prevent *selective* suspend while open
   (`DeviceIdleEnabled = false`, `DefaultIdleState = false`, `UserSetDeviceIdleEnabled = true`), so
   the only suspend under test is whole-system sleep.

### Validation

4. Confirm the stream resumes at full rate after wake with **no reopen**, same device handle.
5. Confirm removal (unplug / hub port off) still tears down: `WM_DEVICECHANGE` sweeps the device and
   its thread exits; verify no permanent retry spin.
6. Watch device-thread CPU during the sleep/resume window.  If the re-arm spins (reads failing
   immediately rather than pending), the backoff should hold it to a submit every 5->100 ms; if the
   OS instead cancels-then-refuses submits at a high rate, retune `BULK_IN_RETRY_DELAY_MS` upward
   using the Linux methodology (the 1-2 ms Linux race is Linux-specific; measure the Windows cliff).
7. Confirm no regression to normal streaming, close, and hot-unplug when no sleep occurs.

## Windows — measured behavior (2026-07-13, on-target)

Two machines, Joulescope UI streaming a JS320, Beagle USB 5000 on the wire.  The bulk IN re-arm
alone recovers neither.

### Desktop (traditional S3 sleep)

- Sleep: the device sees a normal USB suspend.  Firmware unregisters its link watchdog
  (minibitty `usbd.c` `USB_EV_SUSPEND`) and waits; control LED stays green.
- Wake: Windows issues a **full USB bus reset**, not resume signaling, and re-enumerates the
  device.  The control LED goes blue: the usbd state machine left CONNECTED and only returns on
  `MB_COMM_LINK_SM_EV_IDENTITY_RECEIVED`, i.e. the host replaying the link handshake, which the
  host never does.  The sensor LED stays green: no firmware reset.
- The firmware cannot have initiated this.  Its only soft-disconnect (`DCTL` SDIS, stm32h7rs
  `usbd_ll.c`) runs during ll init, which only executes after an MCU reset, and no reset
  occurred.  The reset is host-initiated and is normal Windows behavior when the xHCI controller
  loses power/context across S3: the hub driver performs *reset-recovery* -- port reset, re-read
  descriptors, and (descriptors matching) it keeps the same devnode and open handles, so no
  `WM_DEVICECHANGE` fires.  USB 2.0 permits reset in place of resume; devices must tolerate it.
- Consequence: the WinUSB handle stays valid and the re-armed reads pend fine, but the firmware
  link layer is back at AWAIT_REQ and will never transmit.  The session is dead until the UI
  closes and reopens (which replays the handshake).  Pipe re-arm cannot recover this.

### Laptop (Modern Standby, S0 Low Power Idle)

- Sleep: the device **never sees USB suspend** (Beagle-confirmed).  Modern Standby has no global
  bus suspend; devices sleep only via USB selective suspend, and our MS OS descriptors disable it
  (`DeviceIdleEnabled = false`, minibitty `usbd_descriptors.c`).  The bus stays active (SOFs
  continue) while the Desktop Activity Moderator freezes the UI process.
- The firmware therefore sees exactly what a host-app crash looks like: an active bus with a
  silent host.  The link watchdog fires and the device resets -- correct for the CE recovery
  requirement, wrong for sleep.  The information needed to tell them apart is not on the bus.
- Confirm the sleep-state split per machine with `powercfg /a` (desktop: S3; laptop:
  "Standby (S0 Low Power Idle)").

### Diagnosis

The Linux model ("suspend fails the reads; re-arm until they complete") does not transfer:

1. Windows S3 wake resets and re-enumerates the device, wiping firmware link state while keeping
   host handles alive.  Recovery requires replaying the link connect/identity/open sequence and
   re-applying device state, not just re-arming pipes.
2. Windows Modern Standby never suspends the device, so the firmware cannot distinguish host
   sleep from host crash from the bus alone.  The host must announce sleep explicitly.

## Windows — revised design

Keep the bulk IN re-arm; it is still needed to survive the failed reads at the sleep/wake edges.
Add three pieces:

1. **Host sleep/resume notifications** (`winusb/backend.c`).  The backend already runs a message
   window for `WM_DEVICECHANGE`; also handle `WM_POWERBROADCAST`:
   - `PBT_APMSUSPEND` (sent on both S3 and Modern Standby entry): quiesce streaming and send the
     firmware a "host sleeping" link message for each open device.  Measure the time budget --
     the handler must finish before the process is frozen (expect < 2 s; keep it to one small
     transfer per device).
   - `PBT_APMRESUMEAUTOMATIC`: trigger link revalidation for each open device.
2. **Firmware "host sleeping" link message** (minibitty).  A link control frame that mirrors
   `USB_EV_SUSPEND`: `mb_task_watchdog_unregister()` until the next RX activity (or USB
   suspend/reset).  A crashed host never sends it, so the CE watchdog-recovery requirement is
   preserved.  Fixes the laptop watchdog reset.
3. **Link revalidate/re-establish** (`mb_device.c`).  On resume notification, ping the link; if
   the device re-enumerated (desktop reset-on-resume leaves it at AWAIT_REQ), replay the
   connect -> identity -> open sequence on the same client-facing device and re-apply streaming
   state.  Fixes the desktop dead session.  The Linux ideal of "no reopen" holds at the client
   API, not on the wire: after a bus reset the wire session is necessarily new.

### Alternative considered: enable selective suspend

Setting `DeviceIdleEnabled = 1` plus WinUSB AUTO_SUSPEND power policy would let Modern Standby
actually suspend the device, making the laptop look like the desktop.  Rejected for now:

- WinUSB treats pending bulk IN reads as idle, so the device could selective-suspend during any
  quiet moment of an open session -- a much larger behavior change than sleep handling.
- Windows caches the MS OS registry properties per device (usbflags); changing them needs a
  `bcdDevice` bump or devnode deletion to take effect, which is painful to roll out.
- It does not address the desktop case: reset-on-resume still wipes the link.

### Validation (revised)

1. Laptop, Modern Standby: sleep/wake while streaming.  Device must not watchdog-reset (both
   LEDs stay green through sleep) and the stream resumes without user action.
2. Desktop, S3: sleep/wake while streaming.  Control LED returns to green after wake without
   close/reopen; stream resumes at full rate.
3. Host crash (kill the UI process): device still watchdog-resets and re-enumerates within its
   timeout (CE requirement intact).
4. Unplug while asleep: on wake, revalidation fails and the `WM_DEVICECHANGE` sweep (or a
   revalidate timeout) tears the device down cleanly; no permanent retry spin.
5. Regression: normal open/stream/close and hot-unplug with no sleep are unaffected.

### Implementation and validation results (2026-07-13)

Implemented across three repos and validated on the S3 desktop with a JS320 (fw 1.1.5):

- minibitty: `MB_FRAME_CTRL_SLEEP_REQ`/`SLEEP_ACK` (frame.h), usbd.c watchdog release on
  SLEEP_REQ, re-register on next RX; unit-tested (usbd_test.c scope 9, 41/41 pass).
- joulescope_driver: `device_change_notifier` WM_POWERBROADCAST -> backend power event ->
  `JSDRV_USBBK_MSG_POWER` into each mb-protocol device rsp_q; mb_device sends SLEEP_REQ on
  suspend, revalidates on resume (4 pings @ 500 ms), replays the handshake via new
  `EV_LINK_REVALIDATE_FAILED` -> ST_LINK_REQUEST on silence.  `on_link_request` now resets
  host frame ids to match the device's `reset_buffers`.
- js320: firmware 1.1.5 built and flashed.

Measured, desktop S3 (`minibitty stream` with `--timeout 30` across a scripted sleep/wake):

- PBT_APMSUSPEND -> SLEEP_REQ -> SLEEP_ACK round trip: **3.5 ms** -- comfortably inside the
  pre-freeze window (process froze ~24 s after broadcast on this machine; USB streaming stopped
  ~5 s before actual S3 entry, which is Windows suspending the USB stack early -- unavoidable).
- Wake: PBT_APMRESUMEAUTOMATIC at t+0; 4 unanswered pings -> replay at t+2.0 s; CONNECT_REQ
  retried every 250 ms until the re-enumeration completed at t+7.0 s; handshake + open restore
  completed and **streaming resumed with no close/reopen**.  No firmware reset (boot history
  clean).  Bulk OUT writes queued while the device was gone all flush at once when the pipe
  returns, so the device sees a CONNECT_REQ burst; OUT FIFO ordering guarantees the host
  IDENTITY still lands after the last queued CONNECT_REQ, so the handshake converges.
- Host crash: `Stop-Process` on the streaming app -> device watchdog reset (boot history
  `reset=WATCHDOG`) and re-enumeration.  CE recovery intact.

Still to validate (needs other hardware / hands):

- Laptop, Modern Standby (item 1): the SLEEP_REQ path is the fix; confirm PBT_APMSUSPEND is
  delivered before the DAM freeze and the device rides through host sleep without reset.
- Unplug while asleep (item 4) and physical hot-unplug regression (item 5 unplug half).
- macOS (unchanged plan above).

The mb_device revalidate/replay logic has no host unit-test seam yet; see
`doc/plans/mb_device_test_harness.md`.

## Windows selective suspend (2026-07-14)

The laptop (Modern Standby) still watchdog-reset with the SLEEP_REQ design: the
`PBT_APMSUSPEND` broadcast can lose the race against the process freeze, and no
host-side signal reliably fires last-before-freeze.  Revised approach: enable USB
selective suspend so Windows itself translates "host stopped using the device"
into a bus suspend, which the firmware's tested suspend path already handles.

Implemented (minibitty + js320 fw 1.1.5 bcdDevice 0x0001 + jsdrv):

- MS OS descriptors (both 1.0 ext-props and 2.0 sets): `DeviceIdleEnabled=1`,
  `DefaultIdleState=1` (AUTO_SUSPEND default on), new `DefaultIdleTimeout=2000` ms.
  Descriptor-driven; no host `WinUsb_SetPowerPolicy` call.  bcdDevice bumped so
  Windows re-reads the cached MSOS values (usbflags is per VID/PID/bcdDevice).
  Descriptor layout locked by new unit tests (usbd_test scope 11).
- Firmware `wd_en` topic (`c/comm/usbd/wd_en`, u8, default 1): user-accessible
  watchdog inhibit.  0 releases the comm watchdog and blocks all re-registers;
  1 restores (register only while link CONNECTED).  Unit tested (scope 10).
  Found on-target: host publishes enter the pubsub THROUGH the usbd task, so the
  usbd subscription needs `MB_PUBSUB_SFLAG_ALLOW_SAME_TASK` or the no-echo rule
  silently drops delivery.
- jsdrv: 1 Hz keep-alive (link PING) while a session is open; `h/link/keep`
  on/off knob; `WinUsb_GetPowerPolicy` logged at device open (verifies descriptor
  uptake); new `minibitty publish` and `minibitty suspend_test` commands.

### Measured on the S3 desktop (Windows 11): two vetoes, then success

Selective suspend initially never fired under ANY condition (live-idle, frozen
process, killed process, no handles) despite `AUTO_SUSPEND=1, SUSPEND_DELAY=2000`
reading back at open.  Two independent vetoes had to fall:

1. **The power plan**: "USB settings > USB selective suspend setting" was
   Disabled on this machine, which globally blocks selective suspend no matter
   what the device requests.  This is a per-host system requirement; "High
   performance" power plans commonly ship with it Disabled.
2. **Remote wakeup**: the JS320 does not advertise remote wakeup (config
   descriptor bmAttributes D5 = 0), and winusb.sys refuses to selectively
   suspend such a device unless the registry property
   **`DeviceIdleIgnoreWakeEnable = 1`** is set.  Added to both MS OS
   descriptor sets (fw 1.1.5, bcdDevice bumped again to 0x0002 for the MSOS
   cache).  Ignoring wake is correct here: the host resumes the device with
   its own OUT traffic (keep-alive), so device-initiated wake is unnecessary.

With both fixed, all measured behaviors are exactly the design intent:

- LIVE session (host running): never suspends, even with the keep-alive off --
  bulk-IN read resubmissions count as WinUSB activity (suspend_test asserts
  this).  Old keep-alive-less hosts cannot suspend mid-session.
- FROZEN process (NtSuspendProcess, 20 s, `wd_en=1` fully armed): Windows
  suspends the device within the 2 s idle timeout, **beating the 3 s IWDG**;
  the firmware suspend path releases the watchdog; on thaw the session
  auto-resumes and streaming continues.  **No reset, no re-enumeration, no
  user action.**  This is the Modern-Standby-freeze emulation passing.
- KILLED host (`wd_en=1`): handles close -> device suspends ~2 s later ->
  watchdog released -> device parks suspended and recovers on the next open
  (resume + CONNECT_REQ).  **Semantic change: a host crash no longer causes a
  device watchdog reset when selective suspend is operational** -- the device
  parks cleanly instead.  The watchdog reset remains for device-side wedges
  (CE) and for hosts where selective suspend is unavailable (power plan
  Disabled), where crash/freeze behavior is unchanged from before.
- S3 sleep/wake regression: passes unchanged (SLEEP_REQ ack ~3 ms, resume
  revalidate -> handshake replay -> stream recovery).

### Laptop acceptance test (remaining)

With fw >= 1.1.5 (bcd 0x0002) and the new jsdrv, and the laptop's power plan
"USB selective suspend setting" **Enabled**: stream, sleep (lid close), wake.
Expect: device suspends ~2 s after the DAM freezes the UI (no watchdog reset,
LEDs steady), and streaming resumes on wake without user action.  If a host
has selective suspend Disabled in its power plan, sleep still causes a
watchdog reset; `c/comm/usbd/wd_en=0` is the manual mitigation on such hosts.

## Unit-testable logic

The retry state machine (schedule, backoff, clear-on-complete, tear-down-on-NO_DEVICE, top-up on
synchronous submit failure) is pure logic once the USB calls are behind a seam.
`doc/plans/libusb_backend_test_harness.md` describes that seam for the libusb backend; because macOS
shares `backend.c`, those tests cover the macOS classification too.  The WinUSB backend would need
its own equivalent seam over `WinUsb_ReadPipe` / `WinUsb_GetOverlappedResult`; the retry helpers
(`bulk_in_retry_schedule` / `bulk_in_retry`) are already isolated enough to test that way.
