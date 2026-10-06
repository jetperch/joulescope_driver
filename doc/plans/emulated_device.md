# Emulated device backend for host-side testing

**Status**: proposed
**Created**: 2026-07-31 (design review 2026-07)

## Context

The 2026-07 design review found `src/emu.c` / `src/emulated.c` to be a
~10% skeleton: commented out of every build, never registered
(`src/jsdrv.c` `// todo BACKEND_INIT`), Windows-only threading, and all
device handlers `// todo`.  The skeleton was deleted rather than kept as
misleading dead code.  This plan proposes what should replace it.

## Why this is the highest-leverage test investment

A working emulated device would let:

- `frontend_test` and new integration tests exercise open/close/stream/
  statistics/buffer paths without hardware.
- The Python suite (currently 4 test files, none covering `binding.pyx`,
  `record.py`, or any CLI entry point) run end-to-end in CI.
- Bugs like the JS220 `h/i_scale` metadata-suffix mistake, the buffer
  element-type abort, and `record.py`'s u1/u4 write path be caught by
  automated tests instead of design review.
- Fault injection: USB packet skips, device removal mid-stream, malformed
  metadata — paths that are essentially untestable on real hardware.

## Design sketch

Register a lower-level backend (prefix letter reserved for emulation)
via the existing `BACKEND_INIT` mechanism in `src/jsdrv.c`, gated at
runtime rather than compile time:

1. **Activation**: off by default.  Activate via a `jsdrv_initialize()`
   argument (giving the currently-unread `jsdrv_arg_s` its first real
   consumer — see `open_issues.md` section 1), e.g.
   `{"emu", 1}` or an `emu/devices` list.
2. **Backend layer** (`src/backend/emu.c`): implements the
   `jsdrvbk_s` contract — device add/remove on command, no OS
   dependencies, uses `jsdrv_prv/thread.h` portable threading.
3. **Device layer**: one emulated device model that mimics the
   mb_device/JS320 topic surface (the most complete driver), publishing
   metadata from a static table and generating deterministic waveforms
   (sine/ramp/constant per signal) with correct `sample_id` progression
   and time-map messages.
4. **Fault injection topics** under the device's `h/emu/` namespace:
   drop-next-N-packets, delay-response, disconnect, malformed-metadata.
5. **Bindings**: nothing new needed — once the backend registers, the
   device appears in `@/list` like any other, so Python/Node tests drive
   it through the public API.

## Incremental milestones

1. Backend skeleton + add/remove + `@/!add` publishes; frontend_test
   opens/closes the emulated device.
2. Metadata + parameter retention (validates open-mode logic).
3. Streaming f32 i/v/p with time map; buffer + record tests.
4. u1/u4 GPI signals (validates the buffer bit-path and pyjls `fsr`).
5. Statistics generation; Python CLI tests.
6. Fault injection.

Each milestone is a ≤3 file change with tests, per CLAUDE.md.

## Non-goals

- Emulating USB transport details (winusb/libusb stay untested by this;
  see `libusb_backend_test_harness.md` for that seam).
- Bit-exact device firmware behavior.
