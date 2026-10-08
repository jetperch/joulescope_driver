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
# Compiler-checked log format strings

**Status**: proposed
**Created**: 2026-10-08

## Context

Found by the JS110 fuzz run under AddressSanitizer
(`completed/js320_stream_discontinuity.md`): `device_add_announce` in the libusb
backend logged `"%s %s"` with one argument, which crashed in `vsnprintf`
at INFO level.  It is fixed.  `jsdrv_log_publish()` has no printf format
attribute, so the compiler cannot catch these.  Adding
`__attribute__((format(printf, 4, 5)))` (GCC and Clang) reports 59
mismatches in a Linux build, after removing `-Werror`.

Real bugs, not just width differences:

* `src/backend/libusb/backend.c:697`: `%d` for a `char *`.
* `src/tmap.c:145`: `%lu` for a `double`; the following arguments print
  garbage.
* `src/devices/js220/js220_usb.c:1603`: `%u` for two `uint64_t` values.
* `src/devices/js220/js220_usb.c:1694`: `%zu` for an `int`.

The rest are `%lu` or `%ld` for 32-bit values (46; harmless on LP64 and
LLP64 alike, but wrong), `%lld` / `%llu` for `int64_t` / `uint64_t` on
LP64 (3), and `%p` for typed pointers (10, pedantic).  The winusb
backend and other Windows-only code are not compiled on Linux, so a
Windows (or MinGW) build must be checked too.

## Proposal

1. Add the format attribute behind a `JSDRV_PRINTF_FORMAT(fmt, args)`
   macro that is empty for MSVC.
2. Fix every mismatch: `PRIu32` / `PRId32` / `PRIu64` from `inttypes.h`
   for fixed-width types, `(void *)` casts for `%p`.
3. Keep `-Werror`, so new mismatches fail the build.  Check the Windows
   build with MSVC `/analyze` or MinGW.
