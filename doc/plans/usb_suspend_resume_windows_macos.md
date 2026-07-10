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
| Windows | `winusb/backend.c` (native, not libusb) | landed, **unbuilt** | no |

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

**This code has not been compiled.** The Linux dev host has no MSVC/WinUSB toolchain.  First step is
a Windows build.

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

## Unit-testable logic

The retry state machine (schedule, backoff, clear-on-complete, tear-down-on-NO_DEVICE, top-up on
synchronous submit failure) is pure logic once the USB calls are behind a seam.
`doc/plans/libusb_backend_test_harness.md` describes that seam for the libusb backend; because macOS
shares `backend.c`, those tests cover the macOS classification too.  The WinUSB backend would need
its own equivalent seam over `WinUsb_ReadPipe` / `WinUsb_GetOverlappedResult`; the retry helpers
(`bulk_in_retry_schedule` / `bulk_in_retry`) are already isolated enough to test that way.
