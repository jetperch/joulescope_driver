# Deduplicate `mb_check32_xxhash` in joulescope_driver

## Problem

The 7-line `mb_check32_xxhash` helper (xxHash-inspired 32-bit integrity
check, initial value `0x9e3779b1`) is implemented as a private `static`
function in at least two places inside `joulescope_driver`:

- `src/meta_binary.c:30` — `static uint32_t check32_xxhash(...)`
- `src/devices/js320/js320_cal_mgr.c` (added 2026-05-29 to compute the
  `check32` field of the JS320 calibration record) — same body, also
  `static`.

The canonical algorithm lives in
`../minibitty/include/mb/check32.h` / `mb_check32_xxhash`, but
`joulescope_driver` does not link against minibitty as a library, so it
keeps reimplementing the function locally.

## Fix

1. Add `src/check32.c` exposing a single
   `jsdrv_check32_xxhash(const uint32_t * data, size_t length)` that
   bit-for-bit matches `mb_check32_xxhash`.
2. Add a private header
   `include_private/jsdrv_prv/check32.h` declaring the function.
3. Replace the local `static` copies in `meta_binary.c` and
   `js320_cal_mgr.c` with calls to `jsdrv_check32_xxhash`.
4. Update `src/CMakeLists.txt` to include the new source.
5. Add a small `check32_test.c` (cmocka) covering:
   - Empty input
   - Single-word input
   - The same vector the firmware uses (`mb/test/check32_test.c`) so
     host and target agree.

## Risk

Algorithmic compatibility is checked by the existing meta-binary parser
and by the JS320 device's flash-validation path. As long as the
extracted constant `0x9e3779b1` and the operations match the existing
inline copies, behaviour is unchanged.

## Scope

3–4 file changes, low risk, no behavioural changes. Schedule when no
in-flight cal/meta work would conflict.
