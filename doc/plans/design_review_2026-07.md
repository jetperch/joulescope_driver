# Design review 2026-07: incomplete and partially-complete features

**Status**: in progress
**Created**: 2026-07-31 (post v2.3.5 release)

A full design review focused on finding incomplete or partially-complete
features, performed as three parallel deep-dives: core C driver, device
drivers + OS backends, and Python/Node/docs/tests.  The highest-severity
findings were verified directly against the code.

This file is the in-repo tracking list.  The remediation roadmap phases
referenced below follow the approved plan:

- Phase 0: this file + deferred plans + delete emu skeleton
- Phase 1: C core correctness bugs
- Phase 2: device driver correctness bugs
- Phase 3: Python fixes
- Phase 4: Node binding broken-only fixes
- Phase 5: backend/platform parity
- Phase 6: feature completion decisions
- Phase 7: tests and docs

Mark items `[x]` as they land.  Deferred API items live in
`dead_api_audit.md`; Node maintenance strategy in `node_maintenance.md`;
emulated device proposal in `emulated_device.md`.

---

## HIGH severity (crashes, aborts, wrong data)

- [x] P1.2 `src/buffer_signal.c:79-92` — `jsdrv_bufsig_alloc` hard-faults
      (`JSDRV_ASSERT(false)` → process abort) for `JSDRV_DATA_TYPE_INT` and
      `UINT/8`, which JS320 publishes (`s/adc/N/!data` INT/32,
      `s/uart/!data` UINT/8, `js320_drv.c:107-124`).  No error return path.
- [x] P1.1 `src/log.c:65-77` — missing commas collapse the 11-entry level
      name table to 9; `jsdrv_log_level_to_str()` returns NULL for
      DEBUG3/ALL and wrong names for NOTICE..DEBUG2.
- [x] P2.1 `src/devices/js220/js220_usb.c:1182-1194,1526-1527` — JS220
      `h/i_scale`/`h/v_scale` broken two ways: metadata published without
      `$` suffix (JSON becomes the retained value), and the handler reads
      the unconverted `msg->value.value.f64` instead of the converted copy.
- [x] P2.2 `src/devices/js220/js220_usb.c:1088-1096,1017-1027` — `h/fp=0`
      and `h/fs=0` accepted → division by zero (`:1413`,
      `SAMPLING_FREQUENCY / d->fs`).  JS320 has the guard
      (`js320_drv.c:1330-1332`); JS220 does not.
- [x] P2.3 `src/devices/js110/js110_stats.c:30` — `f->max = FLT_MIN`
      (+1.18e-38) instead of `-FLT_MAX`: wrong max for negative signals.
      Also `:81-84` divide-by-zero when `valid_count == 0`; `scnt == 0`
      silently stops statistics forever.
- [x] P2.5 `src/devices/js110/js110_usb.c:1461-1471` — packet-skip fill
      loop is commented out (`// todo handle skips better`); on USB skips
      `sample_id` stops tracking wall clock: permanent sample misalignment.
- [x] P2.7 `src/devices/js320/js320_stats.c:55,65` — `decimate_factor`
      truncated to `uint8_t` (256 → 0) and unguarded `src->decimate == 0`;
      downstream `sample_freq / decimate_factor` divides by zero.
      `jsdrv_statistics_s.decimate_factor` (jsdrv.h:326) is too narrow.
- [x] P3.1 `pyjoulescope_driver/record.py:276` — only `fsr_f32` is ever
      called; `current_range` (u4) and `gpi[N]`/`trigger_in` (u1) signals
      advertised by the record CLI are written incorrectly.  pyjls `fsr`
      is required for integer types and never called.
- [x] P3.2 `pyjoulescope_driver/record.py:201-221` — multi-device open
      loops devices × all signals: N× subscribe/publish, duplicate writes,
      N-1 subscription leak on close.
- [x] P1.7 `src/tmap.c` + `src/buffer_signal.c:362` —
      `jsdrv_tmap_expire_by_sample_id()` implemented/tested/never called;
      tmap grows without bound during long captures and
      `jsdrv_bufsig_info` performs an O(N) `jsdrv_tmap_copy` per data
      message.
- [x] P7.1 `test/test_invariant_tmap.c` (deleted; tmap_test now covers expire/copy/search) — regression test for the tmap
      bounds fix (1acc3ed) is dead: not in CMake, wrong framework
      (check.h vs cmocka), includes nonexistent headers.  The fix has no
      automated coverage.

## MEDIUM severity

### C core

- [x] P1.3 `src/buffer_signal.c:374-375` — u1/u4 write path byte-truncates
      the bit offset (read path at `:450-455` handles it); `:333-342`
      skip-fill wrap test mixes sample and byte units; `:113-131` free
      leaves stale `levels[]` metadata (`summary_get` keys off `k != 0`).
      Dead unreachable f64 NaN-fill branch at `:319-330`.
- [x] P1.4 `src/buffer.c` — `:388` `%s` applied to `u32_a` (crash on
      invalid buffer index); `:303-323` `req_s` leak on the two early-out
      paths of `req_handle_one`; `:728` finalize `<` vs `<=` leaves buffer
      id 16 alive (thread + cmd_q leak); `:471-476` `g/hold` documented
      1→0 clear (jsdrv_prv/buffer.h:51) not implemented.
- [x] P1.5 `src/union.c` — `:262-269` `jsdrv_union_value_to_str` writes
      nothing for F32/F64/NULL (uninitialized caller buffer in log paths);
      `:273,277` U64/I64 truncated to 32-bit; `:167-171`
      `jsdrv_union_as_type` F64→signed rejects all negatives (checks
      `v < 0` instead of type min); `:29-57` `jsdrv_union_eq` returns
      false for identical STDMSG/FRAME (pubsub dedup never fires).
- [x] P1.6 `src/json.c` — `:183-197` `parse_literal` never compares chars
      (`"txyz"` parses as `true`); `:201-233` integers i32-only with
      silent wrap though metadata documents u64/i64; no recursion depth
      limit (untrusted device metadata path).  `src/meta.c:283` —
      `s->range[s->array_idx++]` unbounded vs `range[3]`: overflow on
      malformed metadata.
- [x] P1.8 `src/jsdrv.c:462-467,482-487` — device-not-found logs an error
      then continues with `dev == NULL` → NULL deref in
      `device_subscriber`; missing returns.  Also fixed: factory-failure
      path freed the device while still linked into `c->devices`.
      Remaining (moved to Phase 6): factory failure publishes nothing;
      the device silently never appears in `@/list`.  Needs a frontend
      diagnostic topic decision (e.g. `@/!error`).
- [x] `src/tmap.c:211-263` — `while (1)` searches with no iteration bound
      (part of P1.7).  Also fixed tail-relative ring indexing.

### Device drivers

- [x] P2.4 `src/devices/js110/js110_sample_processor.c:160-171` — NaN
      suppression mode decrements `_suppress_samples_counter` then the
      next block re-increments it; NaN window tracks the wrong counter and
      the first post-switch sample is never NaN'd.
- [x] P2.6 `src/devices/js110/js110_usb.c:972-994` — suppression params
      assigned raw with no clamp (`SUPPRESS_*_MAX` macros dead,
      `js110_sample_processor.c:24-26`); `js110_sp_suppress_win` return
      code discarded (`:981-984`); `s/i/lsb_src` accepts option-less 1.
- [x] P2.8 (partial) — `s/dwnN/N` now rejects > 1000 and
      `s/gpi/+/dwnN/mode` rejects > 3.  Moved to Phase 6 (needs gateware
      knowledge / product decision): `h/fs` retuning the GPI/trigger/
      range family to match JS220; the `s/gpi/+/dwnN/N` register upper
      bound; whether `s/dwnN/mode` bypass toggle should clear the h/fs
      residual `signal_host_factor` or restore it.
- [x] P2.9 `src/downsample.c:301-309` — `jsdrv_downsample_add_u8` lacks
      the NULL-self guard `add_f32` has; JS110 callers pass NULL-able
      pointer (`js110_usb.c:1384,1405`).
- [x] P2 (js220) `js220_usb.c:1099-1106,1524,1532` — `h/filter` accepts
      out-of-range values; metadata is version-gated but value publish and
      handler are not → host/instrument decimation disagreement on
      FW < 1.3.0.
- [ ] `src/devices/js220/js220_stats.c:25-33` — `decimate_factor` is
      constant-folded to 2 by the preceding header validation; never
      tracks on-instrument sinc1 downsampling (wrong charge/energy scale
      at reduced rates).  Investigate with P2.7.
- [x] `js220_usb.c:1064-1071` — `h/fs` silently coerced when
      `signal_n == 2||3`; now logs and republishes the actual rate.

### Backends / platform (Phase 5)

- [x] P5.1 `src/backend/libusb/backend.c:1036-1048` — unbounded
      `device_close_all` loop (`// todo timeout?`): hang on exit if a
      device never idles.  WinUSB bounds joins at 10 s.
- [x] P5.2 `libusb/backend.c:668-701` — `bulk_in_close` clears
      `endpoint_mode[ep]` but open sets `[ep|0x80]`; cancel comparison
      never matches.  Dead code today (no sender of STREAM_CLOSE), latent.
- [ ] P5.3 libusb — `JSDRV_USBBK_MSG_POWER` never emitted on Linux/macOS;
      mb_device suspend-announce/revalidate is Windows-only
      (`winusb/device_change_notifier.c:97-106`, `mb_device.c:2213-2290`).
- [x] P5.4 `mb_device.c:2422-2428`, `js220_usb.c:372-382` — POSIX UL
      threads busy-poll (`poll(..., 2)` ms) instead of using the computed
      timeout; scheduling infrastructure bypassed off-Windows.
- [x] P5.5 `mb_device.c:1839` — device-initiated `MB_LINK_MSG_PING` never
      answered; `:1785-1786` commands to a non-open device forwarded
      instead of rejected (JS220 rejects, `js220_usb.c:1160`).
- [ ] P5.6 `winusb/backend.c:390-393` — bulk-OUT pipe timeout commented
      out (libusb uses 250 ms): wedged OUT hangs indefinitely on Windows.
- [ ] `libusb/backend.c:1258` — `// todo set thread priority`; single
      default-priority thread for all devices vs WinUSB per-device
      `THREAD_PRIORITY_HIGHEST`.
- [ ] `winusb/backend.c:60-65`, `libusb/backend.c:76-77` — bulk-IN
      retry heuristic tuned only on Linux; Windows/macOS values are
      acknowledged guesses (see usb_suspend_resume_windows_macos.md).

### Python / Node / CI (Phases 3-4)

- [x] P3.3 `entry_points/statistics.py:72-94` — JS320 skipped though the
      driver publishes JS320 statistics (`js320_drv.c:921`).
- [x] P3.3 `entry_points/measure.py:80-81` — returns tuple where callers
      expect dict (TypeError); `:102-104` unguarded `data[0]/[-1]`;
      JS220-only topics with no device check (`:116-118`).
- [x] P3.3 `entry_points/info.py:92-108` — `len(...) is None` dead check;
      JS220-only version detail.  `entry_points/record.py:85-93` — prints
      "Unsupported device" for JS320 then records anyway.
- [x] P3.4 `pyjoulescope_driver/binding.pyx:1058` — 'defaults' docstring
      contradicts the 2026-06 redefinition
      (open_state_management.md:61).  Open-mode naming drift:
      C `RESUME` / Python `'restore'` / Node doc `resume`.
- [x] P3.4 `program.py:89-98` — busy-spin loop without sleep; `:17-22`
      docstring references removed `jsdrv_util` binary.
- [x] P4.1 `node_api/src/joulescope_driver.cc:274-289` —
      `buffer_info_to_js`, `buffer_rsp_to_js`, default `bin_to_js` all
      `return env.Undefined(); // todo`.  Done in `9c1d933`.
- [x] P4.2 `.github/workflows/packaging.yml` build_node_js — `npm test`
      never run; `node_api/test/test_binding.js` misuses async
      `assert.doesNotThrow`.  Done in `9c1d933`.
- [x] P7.1 `dbc_test` and `boot_info_test` now registered with ctest.

### Docs (Phase 7)

- [x] `README.md:50,121` — claims Python 3.9+; actual
      `python_requires ~=3.12`.  No mention of JS110/JS220/JS320.
- [x] `doc/js320_cal.md`, `doc/js320_fwup.md`, `doc/js220.txt` — thorough
      docs orphaned from the sphinx toctree (`doc/sphinx/index.rst`).
- [x] `doc/sphinx/py_api.rst` — missing program, program_js320, TimeMap,
      MemClient, StdMsg, release, CLI reference; `TimeMap` not exported in
      `pyjoulescope_driver/__init__.py` `__all__`.
- [ ] `doc/sphinx/conf.py:86` — breathe path hardcoded to `cmake-build`;
      other build dirs silently produce an empty C API section.

## Feature completion decisions (Phase 6 — mini-plan each before code)

- [ ] JS220 `JSDRV_DEVICE_OPEN_MODE_DEFAULTS` is a no-op
      (`js220_usb.c:710-716` `// todo`) while JS320 fully implements it.
- [ ] UART: JS220 `handle_uart_in` empty stub (`js220_usb.c:1436-1441`,
      "Not yet generated by the JS220 instrument"); JS320 channel 13 has
      no ctrl topic and `sample_rate 0` (`js320_drv.c:124`); topic naming
      inconsistent (`s/uart/!data` vs `s/uart/0/!data`).  Ship or remove.
- [ ] meta `range` and `flags: ro` parsed but never enforced
      (`src/meta.c:283`; `JSDRV_META_FLAG_RO` unused by pubsub).
- [ ] JS110 backports: `h/fp` (20 Hz hardcoded, `js110_usb.c:1348`),
      fw/hw version topics (`JS110_HOST_USB_REQUEST_INFO` never issued,
      `:1173` todo), `h/filter`, `h/!reset`, `h/i_scale`/`h/v_scale`;
      sstats std never computed (`:634-646`).
- [ ] JS110 bootloader factory NULL (`src/devices.c:30`) — a JS110 stuck
      in bootloader is discovered but can never be opened/recovered.
- [ ] JS320 `s/v/range/!data` emitted with no ctrl topic/metadata
      (`js320_drv.c:995-1002`); no way to enable/disable.
- [ ] JS320 calibration is offset-only (`js320_cal.h:68-71`), private
      header only, no `h/cal/!cmd` metadata published, no Python wrapper.
- [ ] buffer i32/u8 element-type support (follow-on to P1.2 graceful
      reject): JS320 raw ADC and UART signals cannot be buffered.
- [ ] `downsample_sinc` has no u8/u1 path — JS320 GPI has no host-side
      downsampling (`js320_drv.c:119-123`); JS110 conversely runs a
      linear-phase FIR over range codes / GPI bits, which is not
      meaningful for unordered enums.
- [ ] mb_device `MB_STDMSG_COMM_STATS` todo (`mb_device.c:2078-2090`) —
      no link-quality telemetry.
- [ ] JS220 sample-processor delay: emitted sample is 64 samples old but
      stamped with current `sample_id` (`js110_usb.c:1323`); fixed ~32 µs
      bias at 2 MSPS.  (JS110; listed here for scoping.)
- [ ] `jsdrv_downsample` group delay (`downsample.c:81-150`
      `sample_delay`) computed but has no accessor and is never
      compensated.

## LOW severity / cleanup backlog (unscheduled)

- `src/topic.c:41-55` — `jsdrv_topic_append` writes the terminator before
  the bounds assert can truncate at exact-fill.
- `src/pubsub.c:381` — return-code suffix append without bounds check on
  a maximum-length topic (1-byte overflow).
- `src/pubsub.c:365-386` — `publish_return_code` creates topic nodes for
  unknown topics (unbounded tree growth from malformed responses).
- `src/pubsub.c:83-102` — `#if 0` `topic_str_pop`; `:282` `// todo handle
  error` on subscriber failure.
- `src/buffer.c:56-66` — `a/!add` / `a/!remove` metadata commented out;
  `:436` stale `// todo validate idx`; `jsdrv_prv/buffer.h:53-55` unused
  `"ZZZ"` placeholder macros diverging from hand-compared literals.
- `src/buffer_signal.c:397-404` vs `:615-622` — `rsp_empty`/`rsp_clear`
  byte-identical duplicates; `:631-641` div-by-zero guard placed after
  the divide (currently unreachable); `jsdrv_prv/buffer_signal.h:73`
  stale `// todo summary data.`
- `src/json.c:273` — log format string missing specifier for `offset`.
- `src/log.c:239-255` — `jsdrv_log_unregister` returns 0 when handler not
  found.
- `src/statistics.c:38-43` — `jsdrv_statistics_invalid` leaves `k`
  untouched and has no callers; NaN policy differs from
  `summary_level0_get_by_idx`.
- `src/time.c:57-70` — divide by `counter_rate` with no zero check.
- `src/time_map_filter.c` — offset-only filter; `counter_rate` never
  refined (undocumented limitation).
- `src/sample_buffer_f32.c:127` — ring index without mask (safe today by
  construction).
- `src/jsdrv.c:47` — dead `API_TIMEOUT_MS`.
- `src/devices/js220/js220_usb.c:406-431` — `#if 0` block; `:1778,1784`
  `// todo keep statistics` (frame-ID mismatch counters); `:1196-1197`
  `h/state` write ignored with no return code; `:1169-1172` `h/timeout`
  sleeps the UL thread (undocumented test hook).
- JS110 hardware capabilities hidden by driver metadata: GPO
  START_PULSE/SAMPLE_TOGGLE options, extio trigger_source, ovr_to_lsb
  (`js110_api.h:171-210`, `js110_usb.c:791,856`).
- `js320_drv.c:128-152` — `h/fs` omits 500 kHz (gateware N=2,3 promoted
  to 4; correct but undocumented capability gap vs JS220).
- `src/devices/js320/firmware.c:18-23` — stub firmware in local builds
  (intentional; surprising for developers; consider a build-time note).
- Examples: no example covers all three products (capture/demo/
  stream_buffer lack JS320; statistics lacks JS110; stream_watch is
  JS320-only); `mem_*` examples JS220-only without saying so;
  `capture_viewer.py` needs matplotlib (not in requirements.txt); live
  todos in `example/minibitty/*` (fpga_mem.c:316, stream.c:190,
  loopback.c:226, adapter_tracy.cpp:717).
- Untested modules (no dedicated unit test): `src/devices.c`,
  `js110_usb.c`, `js110_stats.c`, `js220_usb.c`, `js220_params.c`,
  `js320/firmware.c`, `js320_jtag.c`, both backends, `posix.c`/
  `windows.c` (partial).  See also `libusb_backend_test_harness.md`,
  `mb_device_test_harness.md` (items 1 and 5 open).
- `test/buffer_test.c:36` — `TIMEOUT_MS = 100000; // todo 100`; `:428`
  `// todo check range?`.
- cmocka `assert_float_equal`/`assert_double_equal` treat NaN as equal
  to anything (`float_compare` returns 1 when both comparisons are
  false).  Tests that must exclude NaN need an explicit
  `assert_false(isnan(x))` — this masked the JS110 NaN-suppression bug
  for years.  Audit other float assertions.
- `test/hw/test_open_state_js320.py` — manual-run only; not referenced by
  CI or docs.
- Stale plan markers: `code_cleanup_plan.md` ISSUE 7 actually resolved by
  copy-on-publish tmap (d6c1b9e) while ISSUE 4 (msg_queue.c:101 list
  remove before mutex) and ISSUE 5 (`volatile bool` uses) remain;
  `completed/tmap_fix.md` still says "Status: proposed".
- `mem_transaction_dedup.md` — not started (no `jsdrvp_mb_dev_mem_cmd`
  anywhere).

## Verified non-issues

- `JSDRV_DOWNSAMPLE_MODE_AVERAGE` "not yet connected" (CHANGELOG:525) —
  now connected (`js220_usb.c:1063`) and tested (`downsample_test.c:79`).
- `jsdrv_buffer_info_s.time_map` deprecation — still populated alongside
  `tmap` in `binding.pyx:249-260`; correct back-compat.
- `src/error_code.c` — complete; every `JSDRV_ERROR_*` has name and
  description.
- No TODO/FIXME/NotImplementedError markers in Python/Cython sources —
  all Python issues above are silent.
