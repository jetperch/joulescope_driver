# Unit tests for the JS220 USB device

**Status**: proposed
**Created**: 2026-10-06

## Context

Found while fixing a `completed/js320_fix.md` low-priority item.  In
`src/devices/js220/js220_usb.c`, `mem_complete` ran
`memset(&d->mem_hdr, 0, sizeof(d->mem_topic))`.  `mem_topic` is larger
than `mem_hdr`, so the memset also zeroed `mem_offset_valid`,
`mem_offset_sent` and the `mem_data` pointer.  The `NULL != d->mem_data`
check that follows never fired, so every memory read or write leaked its
buffer.  It is now `sizeof(d->mem_hdr)`.

No unit test covers `js220_usb.c` on its own.  `frontend_test` links it,
but only to build the device table.  The fix was checked on hardware
with `jsdrv mem_read --device js220 c/pers` (two identical 256-byte
reads), which shows no regression but cannot show the leak.

## Plan

Follow the `js320_drv_test` and `mb_device_test` pattern: include the
device source directly in the test, and stub the backend, message
allocation and USB services.  Each stage touches at most three files and
passes ctest.

1. Add `test/devices/js220/js220_usb_test.c` and register it in
   `test/CMakeLists.txt`.  Stub `jsdrvp_msg_alloc`, `jsdrvp_backend_send`
   and the USB bulk transfer calls.  Count `jsdrv_alloc` and
   `jsdrv_free` to detect leaks.
2. Memory operations (`h/mem/{region}/!read`, `!write`, `!erase`):
   - each completed operation frees `mem_data`, and the alloc and free
     counts match.  This test fails with the old memset.
   - `mem_complete` clears `mem_hdr`, the offsets and `mem_topic`, and
     publishes the return code to `{topic}#`.
   - a read publishes `!rdata` with the received bytes.
   - a new command while one is in progress is rejected.
3. Parameter handling already reviewed in `design_review_2026-07.md` P2:
   `h/fs`, `h/filter` and `h/fp` validation, so those fixes stay fixed.
