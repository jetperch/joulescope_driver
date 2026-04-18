# Guide: investigate the Windows fuzz crash

## Why this guide exists

`example/fuzz.c` was recently extended with a `--device <prefix>`
option so it can target a specific device on a station with multiple
units attached. It is now used for close/open race-condition testing
on the JS320.

On Linux we hit and fixed a libusb-backend assertion
(`libusb_submit_transfer: transfer->dev_handle` null) when the
frontend queued USB messages against a device whose handle had just
been cleared by the close path. See commit `88a3a93` on branch
`feature/js320` in `joulescope_driver` — helper
`device_closed_reply()` in `src/backend/libusb/backend.c` guards
`bulk_out_send`, `ctrl_in_start`, `ctrl_out_start`, and `bulk_in_open`
against `d->handle == NULL` / `mode != DEVICE_MODE_OPEN`, replying
`JSDRV_ERROR_CLOSED` instead of submitting.

The user then ran fuzz on Windows and it crashed. The Windows backend
(`src/backend/winusb/backend.c`) has the same class of entry points
but has not been audited for the same race. This guide drives that
investigation from a fresh Claude Code session on Windows.

## Starting context

- Repo: `C:\repos\Jetperch\joulescope_driver` (or wherever the user
  has it). Branch: `feature/js320`. Verify with
  `git log --oneline -5`; commit `88a3a93` ("Fix libusb backend
  assert on close/open race, expand fuzz tool") should be present.
- Tools in the same repo: `example/fuzz.c`, `example/jsdrv/`.
- Power-cycle helper (for when the device wedges after a crash):
  `.\cmake-build\example\minibitty.exe power_cycle --power u/js220/002557 --target u/js320/<SN> --count 0`
  (adjust JS220 power-source serial and JS320 target serial to what
  is attached to the station).

Related recent fixes to be aware of (so you do not rediscover them):
- Host UI hot-plug assertion on the fpga_mcu sensor side — fixed in
  `pyjoulescope_ui` commit `eb80d8b` and `joulescope_driver` commit
  `3a0530e`.
- USBD firmware state-machine was missing a `CONNECT_REQ` self-
  transition in `AWAIT_IDENTITY`, causing intermittent `jsdrv info`
  timeouts after rapid close/open — fixed in `minibitty` commit
  `32f4036`. The JS320 on the Windows station may still be running
  older ctrl firmware; confirm before blaming the driver.

## Step 1 — Reproduce

Build first. From an x64 Developer Command Prompt or PowerShell:

```
cd C:\repos\Jetperch\joulescope_driver
cmake --preset debug    (or whatever preset is in use on the station)
cmake --build --preset debug
```

Enumerate devices and run fuzz against the JS320:

```
.\cmake-build\example\jsdrv.exe scan
.\cmake-build\example\fuzz.exe --device u/js320/<SN> --log-level warning
```

If it does not reproduce immediately, try seeds that hit it on Linux:
`--random 42` reproduced the libusb assertion within ~2 s on Linux.

Capture:
- exact command line,
- stdout / stderr (redirect to a file: `> fuzz.log 2>&1`),
- any WER / crash dialog. On Windows, crash dumps typically land in
  `%LOCALAPPDATA%\CrashDumps` if enabled via
  `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps`.
- the exit code.

Record these verbatim before changing anything.

## Step 2 — Get a stack trace

On Linux we used `gdb -batch` to run fuzz and dump `thread apply all
bt` after the SIGABRT. On Windows, the equivalent options are:

1. **Visual Studio**: File → Open → Project/Solution → pick
   `build\joulescope_driver.sln` (or open the .exe with Debug →
   Attach to Process). Run under the debugger; when it faults,
   Debug → Windows → Call Stack. Do this for every thread
   (Debug → Windows → Threads, right-click → Switch To Frame).
2. **WinDbg / cdb** if VS is not available:
   `cdb -g -G -o .\cmake-build\example\fuzz.exe --device ...`
   then `!analyze -v; ~*kb` at the crash.
3. **procdump** to just grab a dump you can open later:
   `procdump -ma -e -x .\cmake-build\example fuzz.exe --device ...`
   The `.dmp` opens in Visual Studio.

The single most valuable artifact is the call stack of the crashing
thread and of the `backend_thread` (the winusb event-pump thread).
If the pattern matches Linux, you will see something like:
`WinUsb_* → ctrl_start_next / bulk_out_send_next → device_handle_msg
→ process_devices → backend_thread`, with `d->winusb == NULL` or
`d->file == INVALID_HANDLE_VALUE`.

## Step 3 — Read the likely-suspect code

`src/backend/winusb/backend.c`. The parallels to the Linux fix are:

- `bulk_out_send` (line ~383): submits `WinUsb_WritePipe` without
  checking that `d->winusb` is still valid.
- `ctrl_start_next` / `ctrl_add` (~474 / ~529): control transfer
  dispatch.
- Wherever bulk-IN stream opening happens.
- `device_open` / `device_close` paths (device_open starts ~408).

What to look for, by analogy with `device_closed_reply()` in the
libusb backend (commit `88a3a93`):

- Is there any guard against `d->winusb == NULL`, `d->file ==
  INVALID_HANDLE_VALUE`, or `d->mode != DEVICE_MODE_OPEN` before
  each submission?
- What does the close path do — does it `CloseHandle` / deinit
  winusb, clear the handle, and then return? Can a queued
  `JSDRV_USBBK_MSG_BULK_OUT_DATA` reach the backend after that?
- Does the frontend or upper driver (`mb_device.c`, `js320_drv.c`)
  fan out publishes to the device after close has been requested?
  (Yes — on Linux, that is exactly the race we fixed.)

If the analogous guard is missing, the fix is the same shape as the
libusb fix: a `device_closed_reply()` helper that drops the message
with `JSDRV_ERROR_CLOSED` through the correct response channel
(bulk: `msg->value = jsdrv_union_i32(JSDRV_ERROR_CLOSED);`, ctrl:
`msg->extra.bkusb_ctrl.status = JSDRV_ERROR_CLOSED;`) before touching
WinUsb.

## Step 4 — Confirm the cause, then fix

Do not skip confirmation. Once you have a stack trace:

1. Annotate which thread crashed and which function tried to use a
   freed/null handle.
2. Reproduce in a minimal way (one open + one close, or a small
   loop) before adding guards.
3. Apply the smallest fix that prevents the crash. Match the style
   of the Linux fix so reviewers have a clear parallel.
4. Rebuild and rerun fuzz with the same seed; also run a broad
   sweep (seeds 0..99 for ~10 s each) to surface secondary issues.

## What NOT to do

- Do not edit `example/fuzz.c` unless the crash is in it. The recent
  `--device` / sample-rate changes are landed and working on Linux;
  the Windows crash is almost certainly in the backend.
- Do not touch `src/backend/libusb/backend.c` from Windows.
- Do not flash firmware. The Windows station runs whatever ctrl
  firmware it has; firmware-level issues (if any) belong in the
  `minibitty` repo and need a JS320-side rebuild per
  `README.md` → "Build and update ctrl firmware".
- Do not commit until the user has seen the stack trace and agreed
  with your diagnosis.

## Deliverable

A short report (1 page) containing:
- exact repro command + seed,
- crash signature (top ~10 frames of the faulting thread, plus the
  backend thread if different),
- the diagnosed root cause tied to specific file:line,
- a proposed fix (diff or described edits), with an explicit
  comparison to the Linux fix in commit `88a3a93`,
- a plan for verifying the fix (seeds to run, expected clean
  behaviour).

Share that report back before changing any tracked code.
