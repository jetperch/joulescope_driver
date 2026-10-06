# Open issues backlog

**Status**: open (living backlog)
**Created**: 2026-10-06

This file merges the open items from three earlier reviews, which are now
archived in `completed/`:

- `completed/code_cleanup_plan.md`: a race and memory review (2026-04).
  ISSUES 1, 2, 3 and 7 are fixed, and ISSUE 6 is not a bug.
- `completed/design_review_2026-07.md`: the incomplete-features review.
  Phases 0-5 and 7 are done.
- `completed/dead_api_audit.md`: declared but unimplemented public API.

Every item below was checked against the code on 2026-10-06.  Locations
are current as of that date.  Follow CLAUDE.md for each item: write a
mini-plan for anything larger than a local fix, and add tests.  Mark
items `[x]` as they land, with the commit.

## 1. Decisions needed before code

These need a product or API decision.  Each one gets a
mini-plan first.

### Public API (from dead_api_audit.md)

- Host pubsub request suffixes and subscribe flags: moved to
  `pubsub_api.md`.
- [x] Stream buffer `g/mode` (fill and hold).  `JSDRV_BUFFER_MSG_MODE`
      (`jsdrv_prv/buffer.h:52`) is unused, and `src/buffer.c:511-514`
      rejects the topic.  Implement it with the UI roadmap, or delete the
      macro.  Also delete the unused `"s/ZZZ/..."` macros (`buffer.h:53-55`).
      DECISION: delete the unused code.  Done: removed the four macros
      and the stale todo in `src/buffer.c`.  `g/mode` still returns
      `JSDRV_ERROR_PARAMETER_INVALID` like any unknown topic.
- [x] `jsdrv_initialize()` arguments.  `c->args` is stored
      (`src/jsdrv.c:995`) but never read.  The doxygen
      (`jsdrv.h:600-618`) implies arguments exist.  Recommendation: keep
      the parameter for ABI stability, document that no arguments are
      defined yet, and reserve it for `emulated_device.md`.
      DECISION: document as recommended.  Done: `jsdrv.h` now says the
      parameter is reserved, pass NULL, and the driver ignores it.

### Features (Phase 6 of the 2026-07 review)

- [ ] JS220 `JSDRV_DEVICE_OPEN_MODE_DEFAULTS` is a no-op
      (`js220_usb.c:712-717`), while JS320 implements it.
- [ ] UART: ship or remove.  The JS220 `handle_uart_in` is an empty stub
      (`js220_usb.c:1464-1469`).  JS320 channel 13 has no ctrl topic and
      `sample_rate 0` (`js320_drv.c:124`).  Topic names differ:
      `s/uart/!data` (JS320) and `s/uart/0/!data` (JS220).
- [ ] Metadata `range` and the `ro` flag are parsed (`src/meta.c:282-331`)
      but not enforced.  `JSDRV_META_FLAG_RO` only filters the DEFAULTS
      push in `mb_device.c:809`; host writes to read-only topics pass.
- [ ] JS110 backports in `js110_usb.c`: `h/fp` (20 Hz hardcoded, :1373),
      firmware and hardware versions (`JS110_HOST_USB_REQUEST_INFO` todo,
      :1198), `h/filter`, `h/!reset`, `h/i_scale`, `h/v_scale`.  The
      sstats std is never computed (:639-649).
- [ ] JS110 bootloader has a NULL factory (`src/devices.c:30`): a JS110
      stuck in the bootloader is found but cannot be opened.
- [ ] JS320 `s/v/range/!data` is emitted (`js320_drv.c:941`) with no ctrl
      topic or metadata, so it cannot be enabled or disabled.
- [ ] JS320 calibration is offset only (`js320_cal.h:62-71`, a private
      header).  `h/cal/!cmd` has no `$` metadata, and there is no Python
      wrapper.  Only `doc/js320_cal.md` describes it.
- [ ] Buffer support for i32 and u8 elements.  The graceful reject is
      done (`buffer_signal.c:56-66`), so JS320 raw ADC and UART cannot be
      buffered yet.
- [ ] `downsample_sinc` has no u8 or u1 path, so JS320 GPI has no host
      downsampling.  JS110 runs a FIR over range codes and GPI bits
      (`downsample.c:301-313`, `js110_usb.c:1430`), which is not
      meaningful for enums.
- [ ] Device factory failure only logs (`src/jsdrv.c:547-554`).  The
      device never appears in `@/list`.  Needs a frontend diagnostic
      topic, for example `@/!error`.
- [ ] JS320 `h/fs` retune of the GPI, trigger and range family to match
      JS220; the `s/gpi/+/dwnN/N` register bound; whether the
      `s/dwnN/mode` bypass clears or restores `signal_host_factor`.  Needs
      gateware knowledge (from review item P2.8).
- [ ] mb_device link telemetry: `MB_STDMSG_COMM_STATS` and TIMESYNC todos
      (`mb_device.c:2083-2091`).

## 2. Correctness bugs

- [ ] JS220 statistics `decimate_factor` is always 2
      (`js220_stats.c:25-39`), so charge and energy use the wrong scale
      when on-instrument sinc1 decimation is active.
- [ ] JS110 sample processor delay: the emitted sample is 64 samples old
      (`js110_sample_processor.c:164-165,235`) but is stamped with the
      current `sample_id` (`js110_usb.c:1348`).  About 32 µs bias at
      2 Msps.
- [ ] `jsdrv_downsample` group delay is computed (`downsample.c:81-150`)
      but has no accessor and is never compensated.
- [ ] `src/pubsub.c:381-382`: the return-code suffix is appended without
      a bounds check.  A 63-character topic writes past the end.
- [ ] `src/pubsub.c:379`: `publish_return_code` creates topic nodes for
      unknown topics, so malformed responses grow the tree.
- [ ] `src/time.c:59`: divides by `counter_rate` with no zero check, and
      `jsdrv_tmf_get` can return 0 (`time_map_filter.c:122`).
- [ ] `src/buffer_signal.c:674-675`: divides before the
      `entries_length == 0` guard at :683.  Unreachable today.
- [ ] `src/topic.c:44,52-53`: the `'/'` append has no bounds check, and
      the terminator write depends on `jsdrv_fatal` not returning.
- [ ] `src/log.c:239-255`: `jsdrv_log_unregister` returns 0 when the
      handler is not found.

## 3. Threading and memory (from code_cleanup_plan.md)

- [ ] ISSUE 4: `src/backend/libusb/msg_queue.c:103` removes the message
      from its list before taking the mutex.  WinUSB is fixed.  Replace
      with an assertion that the message is not in a list.
- [ ] ISSUE 5: `volatile bool` flags with no atomics in `src/jsdrv.c:96`,
      `libusb/backend.c:185`, `js220_usb.c:286,290`,
      `js110_usb.c:476,479`, `js320_fwup_mgr.c:169-177` and
      `mb_device.c:281,285`.  Use C11 atomics for ARM hosts.
- [ ] Free-pool high-water marks: `jsdrvp_msg_free` (`src/jsdrv.c:853-858`)
      pools messages without a limit, so a burst of 256 KB data messages
      stays allocated until finalize.  Suggested caps: 16 normal, 4 data.
- [ ] Optional: pre-allocate the pools at init (`src/jsdrv.c:1001-1002`).
- [ ] Optional: message reference counting.  Only if profiling shows
      `jsdrvp_msg_clone` of data messages matters.  See the archived plan
      for the design.

## 4. Backends and platform

- [ ] `libusb/backend.c:1318`: thread priority is not set.  WinUSB uses
      `THREAD_PRIORITY_HIGHEST`.
- Host power events off Windows (P5.3), the WinUSB bulk OUT timeout
  (P5.6) and the bulk IN retry constants: moved to
  `usb_suspend_resume_windows_macos.md`, "Related open items".

## 5. Tests

- [ ] No dedicated unit tests for `src/devices.c`, `js110_usb.c`,
      `js320/firmware.c`, `js320_jtag.c`, either backend, `posix.c` or
      `windows.c`.  See `libusb_backend_test_harness.md` and
      `mb_device_test_harness.md`.
- [ ] cmocka `assert_float_equal` and `assert_double_equal` treat NaN as
      equal to anything.  149 uses in 13 files.  Six files never check
      `isnan`: `statistics_test.c`, `time_test.c`, `cstr_test.c`,
      `js220_usb_test.c`, `js220_stats_test.c`, `js220_i128_test.c`.
      Add `assert_false(isnan(x))` where the value must be a number.
- [ ] `test/buffer_test.c:36`: `TIMEOUT_MS = 100000;  // todo 100`, and
      `:600` `// todo check range?`.
- [ ] `test/hw/` (`test_open_state_js320.py`, `test_examples_js320.py`)
      runs by hand only.  Document how to run it, in the README or
      `doc/`.

## 6. Docs and examples

- [ ] `doc/sphinx/conf.py:86`: the breathe path is hardcoded to
      `cmake-build`.
- [ ] `time_map_filter` is offset only (`time_map_filter.c:111-112`) and
      never refines `counter_rate`.  The header does not say so.
- [ ] JS320 `h/fs` omits 500 kHz (`js320_params.c:30-48`) because the
      gateware promotes N=2 and 3 to 4.  Say so in the metadata brief.
- [ ] JS110 hardware features hidden by driver metadata: GPO
      `START_PULSE`/`SAMPLE_TOGGLE` (`js110_api.h:186-187`),
      `trigger_source` (`js110_usb.c:859`), `ovr_to_lsb` (:794).
- [ ] `js320/firmware.c` is a stub in local builds.  Add a CMake message.
- [ ] Examples: `example/jsdrv/statistics.c` does not support JS110.  The
      `mem_read`, `mem_write` and `mem_erase` usage text does not say
      JS220 only.  `capture_viewer.py` needs matplotlib, which is not in
      `requirements.txt`.  `jsdrv mem_read` of a JS220 sensor region
      without `--size` times out (512 KB default).
- [ ] Live todos in `example/minibitty`: `fpga_mem.c:317`,
      `stream.c:204`, `loopback.c:226`, `adapter_tracy.cpp:717`.

## 7. Cleanup

- [ ] `src/pubsub.c:83-102` `#if 0 topic_str_pop`; `:282` `// todo handle
      error` on subscriber failure.
- [ ] `src/buffer.c:461` stale `// todo validate idx`.
- [ ] `src/buffer_signal.c` `rsp_empty` (:439) and `rsp_clear` (:657) are
      identical; `jsdrv_prv/buffer_signal.h:74` stale todo.
- [ ] `src/statistics.c:38-43` `jsdrv_statistics_invalid` has no callers
      and leaves `k` unchanged.  Delete it.
- [ ] `src/sample_buffer_f32.c:127` ring index without the mask (safe by
      construction today).
- [ ] `src/jsdrv.c:47` unused `API_TIMEOUT_MS`.
- [ ] `js220_usb.c`: the `#if 0 jsdrvb_ctrl_out` block (:406-431); frame
      ID mismatch `// todo keep statistics` (:1808,1814); `h/state`
      writes are ignored with no return code (:1224); `h/timeout` sleeps
      the UL thread (:1197), an undocumented test hook.
- [ ] `include/jsdrv.h:326` `jsdrv_statistics_s.decimate_factor` is
      still `uint8_t` beside the new `decimate_factor32`.  Remove the u8
      field at the next major API revision.

## Related plans

Not merged here, since each one has its own scope: `pubsub_api.md`, `example_dedup.md`,
`mem_transaction_dedup.md` (not started), `emulated_device.md`,
`node_maintenance.md`, `libusb_backend_test_harness.md`,
`mb_device_test_harness.md`, `open_state_management.md`,
`usb_suspend_resume_windows_macos.md`, `linux_host_sleep_repro.md`.
