# Plan: `minibitty force_remove` — Reproducible Forced-Removal Stress Test

## Motivation

`joulescope_driver` does not gracefully shut down when a device is
forcefully removed while streaming. The audit in
`code_cleanup_plan.md` identifies several race conditions (ISSUE 1,
ISSUE 2) that can produce use-after-free during teardown. Before we
can fix those, we need a **repeatable, automated** way to provoke the
failure so that we can (a) confirm we have reproduced the reported
bug, (b) validate fixes, and (c) run under ASan/Valgrind.

This plan adds a new `minibitty` subcommand, `force_remove`, that
uses a JS220 as a USB power switch for a JS320 DUT and loops
open → stream → yank-power until the driver crashes or the user
stops it.

## Non-goals

- Fixing the underlying race. That is a separate plan/PR once we can
  reproduce reliably.
- Testing with non-JS320 DUTs. The command is JS320-focused to match
  the available HIL fixture (JS320 u/js320/8W2A powered via JS220
  u/js220/002557).
- Stressing any path other than forced removal during streaming.

## User-facing design

New subcommand registered alongside existing ones in
`example/minibitty/main.cpp`:

```
minibitty force_remove --power <filter> --target <filter> [options]

Options:
  --power     <filter>   Power-supply device filter (e.g. u/js220/002557)
  --target    <filter>   DUT filter (e.g. u/js320/8W2A)
  --iterations <N>       Max iterations (default: infinite, stop on
                         segfault or Ctrl-C)
  --stream-ms  <ms>      Stream duration before yanking power
                         (default: 500)
  --settle-ms  <ms>      Wait after power-on for DUT re-enumeration
                         (default: 2000)
  --remove-timeout-ms <ms>
                         Wait for @/!remove after power off
                         (default: 3000)
```

Behavior loop:

1. Open power device (JS220), set `s/i/range/select = "10 A"`,
   `s/i/range/mode = "auto"` to supply DUT.
2. Wait (up to `--settle-ms`) for DUT (`@/!add`) to appear.
3. `app_match(target)` → `jsdrv_open(target, RESUME)`.
4. Start streaming (subscribe to `s/i/!data` on the target,
   enable `s/i/ctrl`, `s/v/ctrl`, `s/stats/ctrl` — same primitives
   as `stream.c`).
5. Sleep `--stream-ms` while data flows.
6. **Force remove**: publish `s/i/range/mode = "off"` on the JS220
   (cuts DUT power ⇒ USB disappears abruptly — no graceful close).
7. Wait up to `--remove-timeout-ms` for the driver to fire
   `@/!remove` for the DUT. **Do not** call `jsdrv_close` first;
   the whole point is to exercise involuntary teardown.
8. On timeout or after removal is observed, increment iteration,
   go to step 1.
9. If the process segfaults, the CI/ASan log captures it; the
   counter shows how many iterations it survived.

Ctrl-C (`quit_`) exits cleanly between iterations.

## Implementation plan

Files to add / modify (counts stay within the 3-file budget from
`CLAUDE.md`):

1. **`example/minibitty/force_remove.c`** (new) — implements
   `on_force_remove`. Structure mirrors `power_cycle.c`:
   - Reuse the `@/!add` / `@/!remove` subscription pattern and the
     `prefix_matches_target()` helper logic (copy-adapt, ~15 lines
     — not worth a shared module for one caller). If we add a
     second caller later, promote it to a helper per CLAUDE.md #6.
   - Reuse the `PUBLISH_U32` macro style from `stream.c` for
     turning streams on.
   - Single-file command, no new headers.

2. **`example/minibitty/minibitty_exe_prv.h`** — add
   `int on_force_remove(struct app_s * self, int argc, char * argv[]);`

3. **`example/minibitty/main.cpp`** — register
   `{"force_remove", on_force_remove, "Force-remove stress test ..."}`
   in the `COMMANDS` table.

4. **`example/CMakeLists.txt`** — add `minibitty/force_remove.c` to
   the `minibitty_exe` source list (line ~68).

(Four files touched; the CMake + header edits are one-liners. Still
a single coherent feature.)

## Key design choices / open questions

- **Power control.** JS220 `s/i/range/mode = "off"` kills the DUT's
  USB upstream without any software shutdown on the DUT side, which
  is exactly the "device yanked" scenario the driver should handle.
  Confirmed from `reference_hil_commands` memory and matches the
  user's request.

- **Do we `jsdrv_close` after removal?** No. The bug is that the
  driver's own teardown (triggered by backend `DEVICE_REMOVE`)
  races with in-flight data messages. Calling `jsdrv_close`
  explicitly would mask the async path. We let the backend detect
  removal and clean itself up. The next iteration's `jsdrv_open`
  re-acquires the device after power-on.

- **Device discovery.** Per Matt, each iteration waits for the
  DUT using the `@/!add` / `@/!remove` subscription pattern from
  `power_cycle.c`, then re-runs `app_match` to pick up the current
  topic before `jsdrv_open`.

- **Detecting the failure.** The crash manifests as segfault, so the
  process exits non-zero and the iteration counter is the primary
  signal. We will also:
  - Print the iteration number + timestamp before each risky step
    so the last line of stdout localizes the failure.
  - Run under ASan in CI (not part of this command, but it is the
    intended consumption).
  - Not attempt in-process signal handlers for SIGSEGV — they
    interact poorly with ASan and hide the backtrace.

- **Streaming topics.** Per Matt: subscribe to `s/i/!data` and
  `s/v/!data`; enable with `s/i/ctrl = 1` and `s/v/ctrl = 1`.
  Sufficient to exercise the data-message path through
  `JSDRV_PAYLOAD_TYPE_BUFFER_RSP` (ISSUE 1 / ISSUE 3).

- **Timing.** `--stream-ms 500` is long enough to queue several
  data messages in `msg_pend` before removal, which is the
  precondition for the use-after-free per
  `code_cleanup_plan.md` ISSUE 1.

## Testing

- Unit tests: no new pure-logic code to unit-test; this is an
  integration/HIL tool. Per CLAUDE.md #4, noting this
  explicitly — the command itself is the test.
- Manual HIL smoke: run `minibitty force_remove --power
  u/js220/002557 --target u/js320/8W2A --iterations 3` on the
  bench; expect 3 successful iterations pre-fix (or a crash within
  a few).
- Regression target: once fixes land, the command should run
  indefinitely (`--iterations -1`) without crashing or leaking
  (pair with `--log-level info` under ASan).

## Deliverable

A PR containing the four file edits above, adding the
`force_remove` subcommand. No driver-code changes in this PR.

## Follow-on work (not in this plan)

After the repro is in place, a separate PR will address the
race-condition fixes in priority order from
`code_cleanup_plan.md` (ISSUE 1 first, then ISSUE 2, then
ISSUE 3), using `force_remove` as the regression gate.
