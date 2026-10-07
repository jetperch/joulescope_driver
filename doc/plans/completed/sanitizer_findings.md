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
# Fix the C unit test sanitizer findings

**Status**: complete
**Created**: 2026-10-07
**Completed**: 2026-10-07

## Context

Found while adding the pubsub rejected-publish tests.  A Debug build with
`-fsanitize=address,undefined` passes 34 of the 37 C unit tests.  The three
failures also occur at `962ca8c`, before that change:

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    cmake --build build-asan && ctest --test-dir build-asan

* `os_test`: LeakSanitizer reports 16 bytes from `jsdrv_alloc` in
  `jsdrv_thread_join` (`src/backend/posix.c:269`).
* `js320_drv_test`: LeakSanitizer reports allocations from `jsdrv_alloc`
  that the test does not free.
* `frontend_test`:
  * UBSan: `src/jsdrv.c:204` passes a null pointer to `memcpy` for an
    empty binary value (size 0).
  * UBSan: `src/devices/js220/js220_usb.c:793` and `:805` left-shift a
    signed `int` out of range (`65536 << 15`, `1 << 31`).
  * LeakSanitizer reports 288 bytes, including the mutex from
    `jsdrv_platform_initialize` (`src/backend/posix.c:429`).

## Plan

1. Fix the UBSan findings: skip the `memcpy` for a zero size, and use
   unsigned shifts in `js220_usb.c`.  Add unit tests where a behavior
   changes.
2. Fix or attribute each leak: library leaks (thread join, platform mutex)
   versus test fixtures that do not free.
3. Add a CI job that builds the C unit tests with the sanitizers on Linux
   and runs ctest, so new findings fail the build.

Each step builds and passes the unit tests on its own.


## Outcome

With `-fno-sanitize-recover=all`, which the CI job uses so that UBSan
findings fail, and a rebuild, the baseline had more findings than listed
above.  All 37 tests now pass with both sanitizers.

Library fixes:

* `jsdrv_thread_join`: the joiner and the helper thread exchange an atomic
  state, and whichever finishes last frees the helper.
* `jsdrv_platform_initialize` allocates the heap mutex once.  Each
  `jsdrv_initialize` previously leaked the old mutex and replaced it while
  other threads could hold it.
* `js320_finalize` releases in-flight port messages, using the context
  saved at open.  A forced removal (`LL_TERMINATED`) closes the device
  without `on_close`, and stream data can still arrive after `on_close`.
* The libusb backend frees its pooled transfers (`transfers_free`) in
  `device_close_all`.
* `jsdrvp_msg_alloc_value` skips the `memcpy` for a zero size.
* `js220_usb.c` stream suspend and resume use unsigned masks.

Test fixture fixes: `buffer_test` (publish now owns and frees the message,
like `jsdrv_pubsub_publish`), `buffer_signal_test` (free the response tmap
copy), `tmap_test`, `js220_stats_test` (signed shifts) and
`js320_fwup_test` (null `memcpy`).

CI: the `sanitizers` job in `.github/workflows/packaging.yml` runs on
ubuntu-latest and gates `publish_python`.

Dynamic analysis on hardware: `cmake-build/example/jsdrv capture` with a
JS320 (8W2A), built with both sanitizers, runs clean for 3 to 15 second
captures at the default rate and at 1 kHz, a current-only capture, and
CTRL-C.  The first run found the two items above.


## Outcome: Windows

Windows 11 (build 26300), Visual Studio 2026 (MSVC 19.51), LLVM 23.1.3.
Windows uses `src/backend/windows.c` and `src/backend/winusb/`.  The
`jsdrv_platform_initialize` heap-mutex change compiles and runs there.

Builds, each in its own directory:

* MSVC `/fsanitize=address`, Debug with `/Zi /Od` (no `/RTC1`).
* clang-cl with `-fsanitize=address,undefined -fno-sanitize-recover=all`,
  Ninja, `/MD`.  CMake calls lld-link directly, so the link needs
  `clang_rt.asan_dynamic-x86_64.lib`,
  `/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib` and
  `clang_rt.builtins-x86_64.lib` (for `__divti3`), with the LLVM
  `lib/clang/23/lib/windows` directory on `LIB`.  The ASan runtime
  includes the UBSan handlers.
* Leaks: Dr. Memory 2.6 cannot start any target on build 26300.  Instead,
  a CRT debug heap build links an initializer object
  (`.CRT$XIU`, `/INCLUDE:<symbol>`) that sets `_CRTDBG_LEAK_CHECK_DF`, so
  no source change is needed.
* Plain Debug and Release.

clang's UBSan checks more than gcc's, and found two issues:

* `meta_binary_parse` formed `json + pos` past the end of its 2048 byte
  buffer once the JSON overflowed (size 0, so no write).  `json_append`
  now only counts past the end, and replaces 19 copies of the size guard.
* Buffer allocation with no active signals cast an infinite duration to
  `int` in a log message.  It now returns early.

The CRT leak pass found that the JS110 `wait_for_sensor_command` never
freed its status responses, leaking ten 1168 byte messages per capture.
The new `js110_usb_test` covers this with a responder thread.  Linux did
not find it because its hardware run used only a JS320.

The CRT debug heap reports every block still allocated at exit, and
LeakSanitizer only reports unreachable blocks.  Three 40 byte mutexes
remain by design, reachable through statics: the two log mutexes
(`log.c` keeps them for thread safety on exit) and the heap mutex.

Hardware: `jsdrv capture` against a JS320 (31NB), a JS220 (000043) and a
JS110 (000197), at 5 and 15 seconds, at 1 kHz, current-only for 3
seconds, and CTRL-C at 4 seconds, in the MSVC ASan, clang ASan+UBSan,
CRT leak, Debug and Release builds.  All runs exit 0, with non-empty
files and no sanitizer or leak reports beyond the three mutexes, except
for the item below.

Under ASan at `/Od`, the host-side JS110 and JS220 sample processing can
fall behind real time.  Control publishes queued behind the data then
exceed the 1 second API timeout (error 11), and the JS110 at 1 kHz misses
its voltage and power enables.  This is instrumentation overhead and not
a memory error: Debug and Release run clean, and the ASan runs drop
samples.  Not changed.

Not covered: LeakSanitizer (unsupported on Windows), and UBSan in the MSVC
build (unsupported by MSVC).
