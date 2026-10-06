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

# JS320 support across examples and entry points

**Status**: complete
**Created**: 2026-10-03

## Context

An audit of every example and entry point against a JS320 (fw 1.1.9),
done after the same exercise in `pyjoulescope_examples`, found that 12 of
51 are broken or only partly work on the JS320.  The audit covered
`pyjoulescope_driver/entry_points/`, `example/jsdrv/`, `example/minibitty/`
and `node_api/example/`.  Each finding was read in the code, and the ones
marked **HW** were reproduced on hardware with the repo's own build:
`cmake-build/example/Debug/jsdrv.exe` and `minibitty.exe`, and
`python -m pyjoulescope_driver` run from the repo root.

Most of the failures come from four root causes.  Phase 1 fixes those once.
Later phases fix the individual tools.

Out of scope: items already tracked in `design_review_2026-07.md`, and the
JS220-only tools (`program.py`, `release.py`, `threads`, the `jsdrv`
`mem_*` and `reset` commands), except that Phase 4 makes them refuse a
JS320 cleanly.

Each task below touches at most three files, per CLAUDE.md.  Mark items
`[x]` as they land.

### Status at the time of the audit

Broken or partial on the JS320:

- Python entry points: statistics, record, info (no arguments), mem_test.
- `jsdrv`: statistics, capture, demo, stream_buffer, reset.
- `minibitty`: timesync, pubsub_info, sniff.
- Node: statistics.js.

Works on the JS320:

- Python entry points: scan, measure, gpi, `info <device>`, metadata, set,
  and program (by reading the code).
- `jsdrv`: scan, version, info, hotplug, stream_watch, set, dev.
- `minibitty`: info, version, stream, stream_test (10 of 10), cal, fwup,
  mem.
- Node: samples.js.

---

## Phase 1: root causes

### 1.1 The JS320 rejects "on"/"off" for `s/i/ctrl`, `s/v/ctrl` and `s/p/ctrl`

- [x] `src/devices/js320/js320_drv.c:771`: `js320_handle_stream_ctrl`
      converts the value with `jsdrv_union_as_type(&v, JSDRV_UNION_U32)`,
      which cannot parse a string.  So `publish('s/i/ctrl', 'on')`
      returns `PARAMETER_INVALID` (**HW**).  The host would have converted
      it (`src/meta.c:275`, `jsdrv_cstr_to_bool`), but the JS320 driver
      intercepts these topics first.  The JS220 and JS110 accept the
      string, so the same code fails only on a JS320.
      Fix: convert with `jsdrv_union_to_bool` (which accepts u32 and the
      `cstr.c` true/false tables), then fall back to `jsdrv_union_as_type`.
      **Outcome**: fixed in 2.4.2 without a driver change.  The real cause
      was the metadata: firmware stores bool as u8 with dtype bit 7, and
      the host parsed it as u8.  The host pubsub converts the value
      against the metadata (`pubsub.c:568`) before the JS320 driver sees
      it, so the driver receives 0 or 1.
- [x] Test: verified on hardware (JS320 fw 1.1.10).  "on", "off", "true",
      "false", "ON", "enable", 1, 0, True and False all set the expected
      state on each of the three topics.  The unit tests are in
      `test/meta_binary_test.c` and `test/meta_test.c` (2.4.2).

### 1.2 The JS320 opens with its current range off

The JS320 firmware default for `s/i/range/mode` is 0 = "off"
(`js320/firmware/fpga_mcu/src/app.c:101-112`).  The JS320 measures
nothing until the mode is set to "auto" (4), or to "manual" (5) with a
non-zero `s/i/range/select`.

Correction (2026-10-06): the JS220 also defaults to 0 = "off", with the
same options (0 off, 4 auto, 5 manual), checked on JS220 000043 with
both "restore" and "defaults" open modes.  The examples did not assume
a measuring default; the JS220 branches already set "auto", and the
JS320 branches were missing or used "manual".

- [x] `pyjoulescope_driver/entry_points/statistics.py:93-97`: the JS320
      branch publishes mode 5 without setting a select, so no shunt is
      selected and it reads about 1e-11 A (**HW**; auto reads 30 µA to
      2 mA on the same DUT).  Its comment "matches
      example/jsdrv/statistics.c" is true, but the C example has the same
      bug.  It also never sets `s/stats/scnt`, so `--frequency` is
      ignored on the JS320.
      Fix: merge it into the JS220 branch at `:86-91`: mode "auto", then
      `scnt = round(1e6 / frequency)`.  The firmware supports
      `s/stats/scnt` at 1 Msps (`app.c:250-255`).
- [x] `example/jsdrv/statistics.c:105`: same fix, mode "auto" (**HW**:
      reads about 1e-11 A).
- [x] `pyjoulescope_driver/entry_points/record.py:89`: commit the local
      change that adds `js320` to the JS220 range branch.  The committed
      version records 5e-11 A on a JS320; the patched one records 2 mA
      (**HW**).  Also warn when `--open restore` leaves
      `s/i/range/mode` at 0.
- [x] `example/jsdrv/capture.c:184-187`: add a JS320 branch with mode
      "auto" (**HW**: current mean 8e-11 A, now 28 �A).
- [x] Decide whether the JS320 firmware default should stay "off".
      **Outcome** (2026-10-06): keep "off".  The JS220 and JS320 both
      default `s/i/range/mode` to "off".
      Changing it to "auto" in `js320/firmware/fpga_mcu/src/app.c` would
      fix every external script at once.  That is a firmware decision for
      the js320 repo.  Record the outcome here either way.  Because the
      JS220 also defaults to "off", keeping "off" matches the JS220.

### 1.3 Every client logs four `fwup/...` warnings at startup

- [x] `src/jsdrv.c:683-706`, `handle_backend_msg`: the JS320
      firmware-update manager publishes `fwup/@/list` and
      `fwup/js320/version` through `jsdrvp_backend_send`
      (`src/devices/js320/js320_fwup_mgr.c:948,1113`).  Only `@`, local
      and `m` topics bypass `device_lookup`, so each logs
      "device_lookup(...) failed" (`:418`) and "no device match" (`:704`).
      The same happens for every `fwup/NNN/status` during an update
      (**HW**: four warnings on every `jsdrv` and `minibitty` command).
      Fix: publish topics starting with `fwup/` directly, next to the `m`
      case.
- [x] Test: `test/frontend_test.c` sends an `fwup/@/list` message through
      the backend path and checks that no warning is logged.

### 1.4 Device selection

`device_paths()` also returns non-Joulescopes, such as a MiniBitty at
`u/mb/<serial>`, and several tools act on the first device found or on
all of them.

- [x] `pyjoulescope_driver/entry_points/`: filter to `js110`, `js220` and
      `js320` before opening, and close only what was opened.  Done with
      the shared `pyjoulescope_driver/device_filter.py`, which accepts a
      device path, model or exact serial number:
  - `record.py:60-70,118`.  Also fix the `endswith` serial match, which
    has no `/`, so "1" matches several devices.
  - `measure.py:112-116`.  Its `print('Found %d devices', ...)` never
    formats the count.
  - `statistics.py:70`, which opens before the model check.
  - `gpi.py:39-43`, which skips `close()` when the query times out.
  - `program.py:83`, which reports "multiple devices" when a MiniBitty is
    attached.
- [x] `example/minibitty/main.cpp:102-125`, `app_match`: a filter is a
      raw prefix of the full path.  `u/js320/8` matches every serial that
      starts with 8, and `js320` matches nothing.  Fix: also accept a
      model (`js320`), a serial number or a full path, and require an
      exact serial-number match.  Correct the usage examples that can
      never match: `pubsub_info.c:249` (**HW**) and `pubsub_sniffer.c:27`.
      Done with the shared `example/common/device_match.c`, which both
      `jsdrv` and `minibitty` use.  A filter that ends in "/" is still a
      prefix, so `u/js320/` keeps working.  The `pubsub_info` example
      `js320` now matches, and the hotplug target checks in
      `force_remove.c`, `power_cycle.c` and `fuzz_fwup.c` use the same
      match.
- [x] `example/jsdrv` (`capture.c`, `demo.c`, `statistics.c`, `set.c`):
      `app_match(NULL)` and `foreach_device` include `u/mb/*`.  Filter to
      `u/js` prefixes.

---

### 1.5 Intermittent native crash on device close (all models)

- [x] An access violation (0xC0000005) in native code while a device
      closes, seen on two models through both Python APIs, during the
      `pyjoulescope_examples` device suite on Windows:
  - JS110: `joulescope/read_by_method.py`, in
    `joulescope/v1/device.py:468` (`self._driver.close(path)`), called
    from `with device:`.  Seen once in about three full suite runs (**HW**).
  - JS320: `pyjoulescope_driver/dut_power.py`, on leaving
    `with Driver()` after open, query and close.  Seen once (**HW**).

  It never reproduced when an example ran on its own (0 in 55 runs).  It
  appears only after other device tests, so the state the previous test
  left, such as data still arriving during close, probably matters.
  Reproduce with `python -m pytest` in `pyjoulescope_examples` with one
  instrument attached; the failure message names the example.
  Investigate the close path for callbacks that race with
  `jsdrv_close` and unsubscribe.

  **Outcome** (2026-10-06): root cause found and fixed.
  - Reproduced with `examples/joulescope/read_by_method.py --duration 0.5`
    in a loop (about 1 crash in 30 to 90 runs, JS110 and JS320), and
    caught under `cdb` with a `/Zi` build.  The suite alone did not
    reproduce it in 5 runs.  An unoptimized build did not crash in 400
    runs, since the bug depends on stack contents.
  - Stack: the frontend thread in `jsdrvp_msg_free` (`jsdrv.c:828`) from
    `publish_normal` (`pubsub.c:598`), freeing the `@/!close` i32
    message, while the main thread waited in `Driver.close`.
  - Cause: `binding.pyx` `Driver.close` set only `type` and `value` of
    a stack `jsdrv_union_s`, so `flags`, `op`, `app` and `size` were
    stack garbage.  When `app` was 3 (`BUFFER_INFO`) or 5
    (`BUFFER_RSP`), `jsdrvp_msg_free` treated the i32 as a buffer
    pointer and freed its `tmap`.  `Driver.query` had the same unset
    fields.  `open` and `publish` already zeroed theirs, and the Node
    binding zeroes all of its unions.
  - Fix: `close` and `query` zero the union.  `jsdrvp_msg_free` now reads
    `app` only for `BIN` values, as `jsdrvp_msg_clone` already does.
    `test/frontend_test.c` `test_publish_scalar_ignores_app` fails
    without the C fix.
  - Fixed along the way, not the cause of this crash:
    - `Driver.unsubscribe` dropped its callback reference by equality,
      but C matches by pointer, so unsubscribing with a new bound method
      left C calling a freed object (`test_driver_subscribe.py`).
      joulescope v1 is not affected: it stores its bound methods once
      (`device.py:72-73`).
    - `jsdrv_finalize` freed the context after a frontend join timeout
      while that thread still ran.  It now leaks instead.
    - `Driver.finalize` passed 1 s, overriding the 20 s C default.
  - Found but not fixed, in `joulescope/v1/device.py:391`:
    `unsubscribe_all` calls `self._driver.unsubscribe(fn, timeout)`,
    which passes the wrong arguments.  Raise it in pyjoulescope.

## Phase 2: wrong-device safety in `minibitty`

These tools erase, write or burn fuses on whichever device they find
first.

**Outcome** (2026-10-06): done.  `example/minibitty/main.cpp` adds
`app_match_ex(self, filter, flags)`.  `APP_MATCH_EXPLICIT` requires a
filter that matches exactly one device, and `APP_MATCH_MB` refuses the
JS110 and JS220.  `app_power_target_check` refuses a target filter that
matches the power device.  Both use the new `device_is_model` and
`device_match_list` in `example/common/device_match.c`, which
`device_match_test` covers.  Each refusal was checked on hardware with a
JS110, JS220 and JS320 attached.

- [x] `example/minibitty/fpga_mem.c:791-819`: `aes_key` burns OTP fuses on
      the first device with no confirmation; it only prints a warning at
      `:594`.  Require a full `u/js320/<serial>` and `--yes`.
- [x] `example/minibitty/fuzz_fwup.c:1038`: `target_prefix_` defaults to
      `u/js320/`, but `app_match` runs with a NULL filter and takes the
      first device.  With a JS220 bench supply attached, the supply can
      receive the erase, power-cycle and firmware-update sequence.  Set
      the filter to `u/js320/` when none is given, and refuse when the
      target and power devices are the same.
      Also refuses a target filter that matches more than one device.
- [x] `fpga_mem erase/write`, `mem erase/write`, `firmware erase`,
      `power_cycle` and `force_remove`: require an explicit filter for
      destructive operations, and refuse when the power and target devices
      match.  `fpga_mem program` also requires one.  The `--power`
      filter must match exactly one device.
- [x] JS320-only commands (`cal.c:242`, `fwup.c:134`, `firmware.c`,
      `fpga_mem.c`, `mem.c`): fail on `u/js110/` or `u/js220` path before acting.
      `cal` can also wait forever on another model (`cal.c:217`); add a
      timeout to `run_cmd`.  `run_cmd` now times out after
      `CAL_CMD_TIMEOUT_MS` (60 s).
- [x] `pyjoulescope_driver/entry_points/mem_test.py:101`: it defaults to
      `devices[0]` and then erases and writes flash.  Require `--device`
      when more than one device is present, and make `--device` a filter,
      as its help text says, rather than an exact match.
      Tested by `pyjoulescope_driver/test/test_mem_test.py`.

## Phase 3: JS320 branches in the `jsdrv` examples

- [x] `example/jsdrv/demo.c:144` and `stream_buffer.c:193`: there are only
      `u/js220` and `u/js110` branches, so a JS320 gets "Unsupported
      device" (**HW**).  Add a JS320 branch with range "auto".  The Node
      examples handle this by testing for "not JS110".
      The JS320 shares the JS220 branch.  Both commands also gained
      `--device`, since they otherwise use the first Joulescope (**HW**:
      both run on the JS320 without errors).
- [x] `example/jsdrv/capture.c`: unsubscribe every channel before
      disabling any of them.  `channel_finalize(i)` disables current while
      voltage is still recording, so the current file came out 49,938
      samples (about 50 ms) shorter than voltage (**HW**).  Also publish
      `h/fs` and `h/filter` with the default timeout instead of 0, so a
      rejected value is reported.
      `capture` also subscribes to every channel before enabling any, and
      gained `--device`.  **HW**: current and voltage now differ by 123 of
      1,950,042 samples (0.006%).
- [x] `example/minibitty/stream_test.c` and `force_remove.c`: set
      `s/i/range/mode` "auto", so that current samples mean something.
      **HW**: `stream_test` passes 10 of 10.

## Phase 4: crashes and broken options

- [x] `example/minibitty/stream.c:61-62,76-77`: `&value->value.u32` takes
      the address of the union's inline storage, not the payload, and
      `p32[32]` reads 128 bytes past it.  Use `value->value.bin` cast to
      `struct jsdrv_stream_signal_s`.
- [x] `example/jsdrv/jsdrv.c:236`: `jsdrv --log-level info` with no
      command calls `strcmp(NULL)`.  Require a command first.
      Also fixed `--log-level` with no level, and the same two crashes in
      `example/minibitty/main.cpp`.
- [x] `example/jsdrv/mem_write.c:97` and `mem_erase.c:57`: `--timeout`
      sets `device = argv[0]`, which replaces the device filter with the
      timeout value.  Delete the line.
- [x] `example/jsdrv/stream_watch.c:240-244,301`: the add and remove
      callbacks point at the stack variable `watch` and are never
      unsubscribed, including on the early `ROE` returns.  Unsubscribe
      before every return, and close `--out`.
      The `!data` subscriptions, which also point at `watch`, are now
      unsubscribed too.
- [x] `example/jsdrv/reset.c:83`: `while (!counter)` ignores `quit_`, and
      `counter` is not volatile.  `:76` publishes a NULL target.  The
      JS320 does not support `h/!reset`: refuse with a message, or map to
      `c/sys/!reset`.
      Refuses the JS320 (use `minibitty firmware launch`) and a missing
      target, and stops waiting when the reset publish fails.
- [x] `example/jsdrv/threads.c`: `h/timeout` is JS220-only, and on a JS320
      it prints errors in a tight loop.  Gate it on the JS220.
      It now selects the first JS220 and fails without one.

## Phase 5: wrong output

- [x] `example/minibitty/timesync.c:179`: `history_push` stores c maps
      (about 99.997 MHz) and s maps (about 16.004 MHz) in one history.  So
      the default run reports a 72 MHz mean with 673,518 ppm stddev
      (**HW**).  Keep one history per source and report each.  Also apply
      `CONVERGE_TIME_MS` (`:35`), which is defined but never used.
      The history excludes maps before the convergence time, which
      `--converge <ms>` overrides.  **HW**: c reports 99,998,752 Hz at
      0.4 ppm and s reports 16,000,724 Hz at 1.8 ppm.
- [x] Investigate the s map's steady -63 ms skew with `--source s`, at
      2.2 ppm rate stability (**HW**).  The metric is a self-consistency
      residual, so a steady 63 ms suggests a real offset in the sensor
      map's UTC anchor.  That would affect multi-instrument time sync.
      **Outcome** (2026-10-06): not a fixed offset.  The skew decays
      exponentially with a time constant of about 275 s (**HW**:
      -12.4 ms to -8.0 ms over 120 s, and the c map behaves the same).
      `update()` in `minibitty/src/tasks/timesync.c` never steps the map
      while the device is open.  It slews the rate by
      `dc_adj = -dc_err >> 8`, an error decay rate of 1/256 per second,
      so a 63 ms error takes about 20 minutes to fall below 1 ms.  The
      changing correction term is also part of the published rate, which
      explains the rate spread.  The error builds up while no host is
      syncing the device, because the map then free-runs on its last
      rate.  So a freshly opened instrument can be tens of ms off UTC for
      several minutes, which does affect multi-instrument time sync.
      Suggested firmware fix, for the minibitty repo: step `map.utc`
      when `|du_err|` exceeds a threshold, such as 1 ms, or on the first
      sync after a host connects.  Then keep the slew for small errors.
      Tracked in `minibitty/doc/plans/timesync_recovery.md`.
- [x] `pyjoulescope_driver/entry_points/info.py:105`: `version_to_str` is
      applied to `c/hw/version`, which is a u8 on the JS320, so it prints
      `hw=0.0.1` (**HW**).  Format by the metadata `format`, as
      `Info.run` already does at `:160-162`.  Also default `--open`
      (`:31`) to "restore", because "defaults" turns the JS320 current
      range off just to print information.
      Both paths now use `format_value`, tested by
      `pyjoulescope_driver/test/test_info.py`.  `--open` already
      defaulted to "restore".  **HW**: prints `hw=1, fw=1.1.10`.
- [x] `example/minibitty/throughput.c:116`: the exit cleanup reuses
      `topic`, which still holds `comm/tpt/0/tx/task`, so it sets the task
      to 0 instead of `tx/cnt`.
      It also unsubscribes from the `!stat` topics now.
- [x] `node_api/example/statistics.js:17`: `require("joulescope_driver")`
      fails from a checkout (**HW**).  Use `require("..")`, as
      `samples.js` does.  Both examples also miss "no devices":
      `"".split(',')` returns `['']`.
      Fixed at the source: `device_paths()` in `node_api/index.js` returns
      `[]`, and `test/test_binding.js` checks for empty paths.  Both
      examples also skip non-Joulescopes and no longer register a null
      SIGINT handler.  **HW**: `statistics.js` reports the JS320.

## Phase 6: firmware metadata (js320 repo)

- [x] `js320/firmware/fpga_mcu/src/app.c`: `gpi/0/ctrl` through
      `gpi/3/ctrl` and `gpi/7/ctrl` have no `dtype`.  The host therefore
      cannot convert a value: `publish('s/gpi/0/ctrl', 'on')` is
      accepted, reads back as the string 'on', and goes to firmware that
      reads `msg->value.u32` (**HW**).  Add `dtype: bool`.  Raise this in
      the js320 repo; it is listed here so it is not lost.
      **Outcome** (2026-10-06): no firmware change needed.  `app.c:261-285`
      has declared `dtype: bool` for all five since `a559b53`
      (2026-03-15).  The symptom was the host bug fixed in 2.4.2, which
      parsed the binary bool dtype as u8, so 'on' passed through as a
      string.  The firmware converts by type (`bit_update_from_msg` calls
      `mb_value_to_bool(msg->type, ...)`), not by reading `value.u32`.

## Phase 7: tests

- [x] `test/hw/`: add a JS320 hardware-in-the-loop test, next to
      `test_open_state_js320.py`, that runs `statistics`, `record` and
      `capture`.  It should fail when current is not finite, or is
      identically zero on a DUT drawing current, and when current and
      voltage sample counts differ by more than 1%.
      Done in `test/hw/test_examples_js320.py`.  "Identically zero" is
      a mean below `JSDRV_HW_MIN_CURRENT` (default 1e-7 A), since a JS320
      with its range off reads about 1e-11 A, not 0.
- [x] `pyjoulescope_driver/test/`: unit-test the entry-point device filter
      (MiniBitty ignored, exact serial-number match) with a fake driver.
      Done in `test_device_filter.py`, which tests the filter directly.

## Low-priority follow-ups

Found during the audit but not JS320-specific.

**Outcome** (2026-10-06): done.  The duplicated example helpers found along
the way are a separate plan, `example_dedup.md`.

- [x] `pyjoulescope_driver/entry_points/record.py:67`: "Device not found"
      returns None, so the exit code is 0.
- [x] `pyjoulescope_driver/mem_client.py:98`: `publish_and_wait` on
      `h/!rsp` accepts the first response without checking the
      transaction id.
      `Driver.publish_and_wait` takes a new `match` callable that skips
      other responses, and `MemClient.cmd` matches the transaction id.
      Tested by `test_driver_publish_and_wait.py` and
      `test_mem_client.py`.
- [x] `example/jsdrv/jsdrv.c:175,181` and `example/minibitty/main.cpp:186-189`:
      `"e"` maps to EMERGENCY, so the ERROR alias is unreachable.
      Removed the EMERGENCY alias, so `"e"` is ERROR.
- [x] Usage strings say `jsdrv_util`, but the binary is `jsdrv`
      (`example/CMakeLists.txt:36`).  Also fixed the `release.py`
      docstring.
- [x] `example/minibitty/power_cycle.c:42`: the usage text gives a
      `--delay` default of 2500, but the code default is 0.  Several
      tools use `atoi` with no validation.
      The usage now says 0.  `power_cycle`, `force_remove`, `cal`,
      `publish`, `state_get`, `stream` and `timesync` parse with
      `jsdrv_cstr_to_u32` or `jsdrv_cstr_to_i32` and reject bad values.
- [x] Leaks on error paths: `set.c:240`, `mem_read.c:143`,
      `mem_write.c:131`, `fpga_mem.c:315`, `mem.c:260`, `firmware.c:122`,
      `timesync.c:359-365`, and `force_remove.c:262-265`, which
      resubscribes on every iteration.
      `set`, `mem_read`, `mem_write` and `timesync` now close the device
      and unsubscribe on every path, `mem` frees its semaphore when open
      fails, and `force_remove` unsubscribes each iteration.
      `fpga_mem.c` and `firmware.c` did not leak: their callers always
      run `teardown`, which frees the semaphore and event.
- [x] `src/devices/js220/js220_usb.c:906`: `memset(&d->mem_hdr, 0,
      sizeof(d->mem_topic))` zeroes `mem_data` before it is freed, so
      every memory operation leaks its buffer.
      Now `sizeof(d->mem_hdr)`.  **HW**: `jsdrv mem_read` of the JS220
      `c/pers` region still works.  `js220_usb.c` has no unit tests, so
      they are planned in `js220_usb_test.md`.
- [x] `doc/plans/design_review_2026-07.md` P4.1: the Node
      `buffer_info_to_js`/`buffer_rsp_to_js` todos look done in the
      working tree but are still unchecked.
      Done in `9c1d933`, along with P4.2.  Both are now checked.
