# Deduplicate the jsdrv and minibitty example helpers

**Status**: proposed
**Created**: 2026-10-06

## Context

Found while working on `completed/js320_fix.md` Phases 2 to 5.  The `jsdrv` and
`minibitty` example executables repeat the same helpers, so each fix has
to be made more than once.  For example, the `--log-level` with no
command crash had to be fixed in both `main` functions.

- `main`, `usage`, `on_log_recv`, `LOG_LEVEL_CONVERT`, `log_level_cvt`,
  `app_scan` and `app_match`: `example/jsdrv/jsdrv.c` and
  `example/minibitty/main.cpp`.
- `publish_str`, `device_prefix`, `on_device_add`, `on_device_remove` and
  `wait_for_condition`: `example/minibitty/power_cycle.c`,
  `force_remove.c` and `fuzz_fwup.c`.
- A `publish` helper that formats and prints the value:
  `example/jsdrv/capture.c`, `demo.c`, `threads.c` and `stream_buffer.c`.
- The `--device` option parsing: most `example/jsdrv` commands.

The two `app_match` copies also differ: `jsdrv` skips non-Joulescopes when
there is no filter, and `minibitty` has `app_match_ex` with the
`APP_MATCH_MB` and `APP_MATCH_EXPLICIT` flags.

## Plan

Each stage builds, passes `device_match_test`, and touches at most three
files.

1. Move the log-level table and `log_level_cvt` into
   `example/common/log_level.c`, with a unit test like
   `device_match_test.c`.  Use it from both `main` functions.
2. Move `app_scan` and `app_match_ex` into `example/common/app_match.c`.
   Make the `jsdrv` no-filter behavior a flag (`APP_MATCH_JOULESCOPE`).
   This needs a shared `struct app_s`, or a small struct with the
   context, device list and matched device.
3. Move the hotplug target tracking (`device_prefix`, add and remove
   callbacks, `wait_for_condition`) and `publish_str` into
   `example/minibitty/target_tracker.c`, and use it from `power_cycle`,
   `force_remove` and `fuzz_fwup`.
4. Share one `publish` helper in `example/jsdrv`.
