# Fuzz session WIP — 2026-04-16

Snapshot of fuzz-robustness work produced during the 2026-04-16
session.

## Status: complete (2026-04-17)

All 7 "In flight" items below landed on `feature/js320`:

| Item | Commit |
|---|---|
| 1. GET_INIT BUSY retry | `aac71e6` |
| 2. Close-path timeouts | `aac71e6` |
| 3. Safe UL join on timeout | `aac71e6` |
| 4. d-pointer correlation logs | `aac71e6` |
| 5. `ST_AWAIT_SENSOR_READY` | **superseded** by `dd0103b` — the mb_device genericize refactor replaced the dedicated state with a generic `open_ready` driver hook. JS320 implements sensor-ready waiting via that hook instead. |
| 6. Sample-ID skip diagnostics | `62c6f3d` |
| 7. Cross-platform crash handler | `2dae1f1` |

Follow-on work from the same session:

* `37c6b9f` — fuzz `--nice` option.
* `dd0103b` — mb_device genericize (removes JS220/JS320-specific paths;
  see `doc/plans/completed/mb_device_genericize.md`).
* `70fd94b` + minibitty `cf4ad52` — timesync `!resync` topic so the
  JS320 host forces a sensor-side burst on open, since the sensor-side
  comm link stays up across host disconnect/reconnect.

The `sample_id skip` class of fuzz failures noted below remains
deferred to `doc/plans/open_state_management.md`.

## Landed (committed on feature/js320)

* `5c6e61d` — winusb backend close/open race fix. Introduces
  `device_closed_reply()` that replies with `JSDRV_ERROR_CLOSED`
  instead of submitting to `WinUsb_*` with a dead handle.
* `31ae5d9` — finalize timeout rework:
  * New `FINALIZE_TIMEOUT_MS = 20000` in `src/jsdrv.c`
    (separate from `API_TIMEOUT_MS = 3000`, which stays generic).
  * Per-device UL joins and LL `device_thread_stop` reduced 30 s → 10 s
    (`src/mb_device.c`, `src/js220_usb.c`, `src/js110_usb.c`,
    `src/backend/winusb/backend.c`).
  * Stage instrumentation on frontend exit + winusb backend finalize.
  * Libusb backend got matching stage logs.

Firmware side (committed separately in `minibitty` on `main`):

* USBD TX race fix — `in_transmit_done` no longer asserts on stale
  XFRC; `USB_EV_TX_DMA_DONE` / `USB_EV_TX_FIFO_READY` now guarded on
  `state == BUSY`; `mb_usbd_ll_in_transmit_abort()` added and called
  from `MB_COMM_LINK_SM_EV_RESET_BUFFERS` before clearing SW state.
  Verified: 20 min fuzz run without a single hard fault.

## In flight (uncommitted on feature/js320)

Ready to commit in logical chunks — all survived the 20-minute fuzz run.

1. **GET_INIT BUSY retry** (`src/mb_device.c`). `state_fetch_on_rsp`:
   on `JSDRV_ERROR_BUSY`, re-arm the 50 ms timeout so the existing
   `on_open_timeout` retry path fires. `STATE_FETCH_GET_INIT_RETRY_MAX`
   bumped 3 → 20 (1 s retry window).

2. **Close-path timeouts** (`src/mb_device.c`). Three `// todo timeout`
   markers resolved: `on_pubsub_flush` / `on_link_disconnect` /
   `on_ll_close` now call `timeout_set(self)`; their transition tables
   each have an `EV_TIMEOUT` entry that forces progress toward
   `ST_CLOSED` with a `WARN` log.

3. **Safe UL join on timeout** (`src/mb_device.c`). `join()` returns
   early without freeing `d` when `jsdrv_thread_join` times out; logs
   `LEAK - thread still running, refusing to free`. Prevents the
   old-thread-UAF class of bugs when UL is hung.

4. **d-pointer correlation logs** (`src/mb_device.c`). UL thread
   started/done and join send/joined lines include `d=%p`.

5. **`ST_AWAIT_SENSOR_READY`** (`src/mb_device.c`). New state between
   `ST_LINK_IDENTITY` and `ST_OPEN`. Entered on
   `EV_LINK_IDENTITY_RECEIVED`; transitions to `ST_OPEN` on
   `EV_SENSOR_READY` (fired when any `s/...` publish arrives from the
   device — sensor task alive) or on `EV_TIMEOUT` (2 s fallback).
   Prevents `state_fetch` from racing the sensor power-up.

6. **Sample-ID skip diagnostics** (`src/js320_drv.c`). The skip log
   now includes `signal_dwn_n`, `gpi_dwn_mode`, `gpi_dwn_n`,
   `runtime_decimate` for root-cause analysis.

7. **Cross-platform crash handler** (`example/fuzz.c`,
   `example/CMakeLists.txt`). Windows: `SetUnhandledExceptionFilter` +
   DbgHelp `StackWalk64` on the fault `CONTEXT`. POSIX: `sigaction` +
   `execinfo` backtrace. `ENABLE_EXPORTS` on Linux so symbol names
   resolve in `backtrace_symbols`.

## Reverted (redesign in a separate plan)

During the session, an attempt was made to fix the "open-mode defaults
doesn't actually restore defaults" issue via a generic pubsub walk that
published each topic's metadata-declared `default`. That approach was
reverted — it's the wrong primitive:

* `publish_normal()` dedups against the host's cached retained value,
  so defaults matching a stale host cache never reach the device.
* One publish per parameter, not atomic.
* Topic filtering relies on leaf-name heuristics.

A redesign using `MB_STDMSG_STATE_TYPE_SET_CMD` (single atomic
state-set round-trip) plus a new `JSDRV_DEVICE_OPEN_MODE_OVERRIDE` is
planned in `doc/plans/open_state_management.md`. That plan also drops
the hardcoded `{'c', 's'}` subtree list for generic MiniBitty support.

### Known-expected fuzz failures until the redesign lands

Under fuzz, `sample_id skip` messages still appear after rapid
close/open cycles that change `h/fs` or `s/dwnN/N`:

```
ch 5 sample_id skip: expected=... received=... signal_dwn_n=0
  gpi_dwn_mode=2 gpi_dwn_n=16 runtime_decimate=16
```

The skip is now a **5× factor** (down from 1000× before host-side
changes landed), reflecting a drifted `s/dwnN/N` between host and
firmware across a close (host resets to default on close, firmware
retains its register). The generic walk that would have fixed this is
reverted. These skips are **expected** until the open-state-management
redesign is implemented per `doc/plans/open_state_management.md`.

No crashes, no hangs — just stream-data mis-alignment on channels that
depend on the drifted register.

## Next actions

1. Commit the 7 in-flight items above in logical chunks from
   `feature/js320`.
2. Start work on `doc/plans/open_state_management.md`.
3. When open-state-management is merged, re-run fuzz and confirm
   `sample_id skip` count drops to zero.
