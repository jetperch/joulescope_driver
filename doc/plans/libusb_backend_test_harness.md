<!--
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
-->

# libusb backend test harness

`src/backend/libusb/backend.c` has no host test coverage.  It calls `libusb_*` directly, so every
defect in it must be found on hardware.  The bulk IN suspend/resume recovery added 2026-07-10 is
the third behavior in this file whose only proof is a bench capture.

## What needs covering

The bulk IN retry state machine, which is pure logic once libusb is behind a seam:

1. `LIBUSB_TRANSFER_ERROR` schedules a retry rather than abandoning the pipe.
2. The retry delay backs off `50 -> 100 -> ... -> 1000 ms` while failures continue.
3. `LIBUSB_TRANSFER_COMPLETED` clears the retry and resets the backoff.
4. `bulk_in_retry_process()` tops the pipe back up to `BULK_IN_TRANSFER_OUTSTANDING`, and does so
   even when a previous `libusb_submit_transfer()` failed synchronously (the case that makes a
   deficit counter leak).
5. `removing` or a non-`OPEN` mode cancels the retry, so a hotplug LEFT wins the race against a
   scheduled re-arm regardless of which lands first.
6. `poll()` timeout is clamped to the next scheduled retry.

## Approach

Introduce a thin seam over the four libusb calls the backend makes on the transfer path
(`libusb_submit_transfer`, `libusb_cancel_transfer`, `libusb_alloc_transfer`,
`libusb_free_transfer`) plus `jsdrv_time_ms_u32()`.  A function-pointer table set at
`backend_initialize()` keeps the production path a direct call and lets a test substitute a fake
that records submissions and drives completions with chosen statuses and timestamps.

This is the same shape as the existing `test/hw` seams.  It does not require a USB device, a
kernel, or root, and it makes the suspend/resume and removal sequences reproducible as ordinary
unit tests:

- suspend: 4 x `ERROR` -> expect one scheduled retry, no teardown, then `COMPLETED` -> backoff clear
- removal: 4 x `ERROR` -> `removing = true` -> expect the retry cancelled and no resubmit
- storm guard: `ERROR` repeated at the retry deadline -> expect the delay to reach the 1000 ms cap

## Related

macOS shares `src/backend/libusb/backend.c`, so the seam and tests described here cover the macOS
retry classification as well.  On-target suspend/resume validation for Windows and macOS, and the
parallel WinUSB seam, are in `doc/plans/usb_suspend_resume_windows_macos.md`.

## Not in scope

`on_hotplug` / `handle_hotplug` and the device open/close paths need a much larger libusb fake.
Their behavior is exercised by `joulescope_ci`'s `usb_suspend` HIL gate, which is where they should
stay until the seam above proves itself.
