# `mb_device` genericization refactor

## Context

`src/mb_device.c` is the generic MiniBitty-protocol upper driver — it
speaks the link layer, state-fetch protocol, timesync, and frame
dispatch that every MiniBitty device shares. Device-specific behavior
(JS320 vs. js110 vs. js220 vs. future MB devices) is supposed to live
in per-device files (`js320_drv.c`, …) via the upper-driver hooks
declared in `include_private/jsdrv_prv/mb_drv.h`.

During the 2026-04-16 fuzz session, JS320-specific assumptions leaked
into `mb_device.c`. This plan identifies the leaks and proposes a
refactor that moves each one back behind a hook.

Sister plan: `doc/plans/open_state_management.md` redesigns the three
open modes using `MB_STDMSG_STATE_TYPE_SET_CMD` and drops the
`STATE_FETCH_PREFIXES` list via null-target GET_INIT. Some items
below overlap with that plan — see § Interactions.

## Findings (from audit)

| # | `mb_device.c` item | Line(s) | Why JS320-specific |
|---|---|---|---|
| 1 | `STATE_FETCH_PREFIXES = {'c', 's', 0}` | 52 | Presumes JS320's ctrl+sensor task split |
| 2 | `EV_SENSOR_READY` event | 126 | Names a JS320 concept |
| 3 | `ST_AWAIT_SENSOR_READY` state | 136, 847, 873, 957 | Entire state dedicated to "wait for sensor to boot" |
| 4 | Sensor-ready detection in `handle_in_publish` | 1368-1374 | Hardcodes `topic[0]=='s' && topic[1]=='/'` |
| 5 | State fetch loop iterates `{'c','s'}` | 338–535 | Consequence of (1) |
| 6 | `_state`/`info` topic synthesis | 506-516 | Uses the hardcoded prefix |
| 7 | Comment at line 854 | — | Documents JS320 behavior (`c/comm/sensor/_state`) |

Items that are correctly generic and stay:
- Frame SOF / link-check / connect / disconnect (MiniBitty protocol).
- `./.!ping / ./.!pong` disconnect handshake (protocol-level; the
  `"h|disconnect"` payload is opaque and device-choosable).
- Timesync (`MB_STDMSG_TIMESYNC_*`).
- State-fetch *protocol* itself (`MB_STDMSG_STATE_TYPE_GET_*`,
  blob-based metadata read) — only the prefix *iteration* is
  JS320-specific.

## Goals

* `mb_device.c` compiles and runs correctly for any MiniBitty device
  with zero JS320 knowledge. A hypothetical single-task device with
  one prefix (e.g. just `'a'`) and no sensor-boot-wait Just Works.
* JS320-specific policy lives in `src/js320_drv.c`, reached through
  `jsdrvp_mb_drv_s` callbacks declared in `mb_drv.h`.
* Existing JS320 behavior preserved: sensor-ready interlock still
  fires before state_fetch; prefix enumeration still covers `c` and
  `s`.
* No change to the `example/fuzz` interface or host-side API.

## Design

### New upper-driver hooks (added to `mb_drv.h`)

```c
struct jsdrvp_mb_drv_s {
    // ... existing hooks ...

    /**
     * Return the null-terminated list of top-level topic prefixes the
     * state-fetch loop should iterate.  NULL or empty means "let
     * mb_device use its default discovery mechanism" (null-target
     * GET_INIT when firmware supports it, or fall back to whatever
     * the protocol deems appropriate).  Returned pointer must outlive
     * the open session.
     *
     * Example for JS320: returns "cs".
     */
    const char * (*state_fetch_prefixes)(struct jsdrvp_mb_drv_s * drv);

    /**
     * Called once on open, after drv->on_open.  Return true if the
     * driver is ready for state_fetch_start to run immediately.
     * Return false to defer state_fetch until the driver explicitly
     * calls jsdrvp_mb_dev_state_fetch_start(dev).  Drivers without
     * async open requirements leave this NULL (same as returning
     * true).
     *
     * The driver is responsible for its own readiness timeout via
     * jsdrvp_mb_dev_set_timeout + on_timeout.
     */
    bool (*open_ready)(struct jsdrvp_mb_drv_s * drv,
                       struct jsdrvp_mb_dev_s * dev);
};
```

No existing hook signatures change.

### New mb_device service (added to `mb_drv.h`)

```c
/**
 * Start the device state fetch.  Called automatically during open
 * unless drv->open_ready returned false, in which case the driver
 * is expected to call this explicitly when ready.
 */
void jsdrvp_mb_dev_state_fetch_start(struct jsdrvp_mb_dev_s * dev);
```

This is a thin wrapper around the existing static `state_fetch_start`;
making it public lets drivers invoke it asynchronously.

### Changes inside `mb_device.c`

1. **Drop `STATE_FETCH_PREFIXES` constant.** Replace with a per-device
   `sf->prefixes` pointer populated from `drv->state_fetch_prefixes()`
   (or a mb_device default like `""`) at `state_fetch_start`.
   `sf->prefix_idx` iterates the returned string. All uses
   (`state_fetch_send_get_init`, `state_fetch_send_get_next`,
   `state_fetch_advance_prefix`, GET_NEXT info-topic check) read from
   `sf->prefixes` instead of the constant.

2. **Remove `EV_SENSOR_READY` and `ST_AWAIT_SENSOR_READY`.** The
   state machine goes back to ST_LINK_IDENTITY → ST_OPEN directly.
   The `state_machine_await_sensor` transition table, the
   `on_await_sensor_enter`/`on_await_sensor_timeout` helpers, and the
   state_machine_states[] entry for ST_AWAIT_SENSOR_READY all go
   away.

3. **Remove sensor-ready detection in `handle_in_publish`** (lines
   1368-1374). Replaced by the `open_ready`/
   `jsdrvp_mb_dev_state_fetch_start` handshake below.

4. **`on_open_enter` reorganized** (approximately):
   ```c
   static bool on_open_enter(struct jsdrvp_mb_dev_s * self, uint8_t event) {
       timeout_clear(self);
       if (self->drv && self->drv->on_open) {
           self->drv->on_open(self->drv, self, &self->identity);
       }
       // existing RESUME / DEFAULTS handling unchanged here
       bool ready = true;
       if (self->drv && self->drv->open_ready) {
           ready = self->drv->open_ready(self->drv, self);
       }
       if (ready) {
           state_fetch_start(self);
       }
       return true;
   }
   ```

5. **Expose `jsdrvp_mb_dev_state_fetch_start`** — trivially thin
   wrapper around the existing static `state_fetch_start`.

### Changes inside `src/js320_drv.c`

1. **`js320_state_fetch_prefixes` callback.** Returns `"cs"` (or a
   `static const char[]` of the same). Assigned to
   `self->drv.state_fetch_prefixes` in the factory.

2. **`js320_open_ready` callback.** Returns `false` (defer state_fetch)
   and arms a 2 s driver-side timeout via `jsdrvp_mb_dev_set_timeout`.
   Records "waiting for sensor" state on `self`.

3. **`js320_handle_publish` addition.** When `self->waiting_for_sensor`
   is true and a topic starting with `s/` is observed, clear the flag,
   clear the timeout, and call
   `jsdrvp_mb_dev_state_fetch_start(dev)`.

4. **`js320_on_timeout` addition.** If `self->waiting_for_sensor` is
   true when the timeout fires, warn and call
   `jsdrvp_mb_dev_state_fetch_start(dev)` anyway (same fallback as
   the old generic 2 s timeout).

5. Move the "`c/comm/sensor/_state`" documentation comment into a
   Doxygen block above the new `js320_open_ready` callback.

Net effect: identical observable behavior for JS320, nothing generic
in `mb_device.c` knows about sensors.

### Other MiniBitty drivers

- `js220_usb.c` and `js110_usb.c`: these don't go through `mb_device`
  today (they have their own open sequences). No changes needed.
- A new minimal MiniBitty device that uses `mb_device`: would provide
  `state_fetch_prefixes` returning its subtree list (e.g. `"c"`) and
  leave `open_ready` NULL. Immediately benefits from all fuzz-hardening
  work without inheriting JS320 quirks.

## Critical files

| File | Change |
|---|---|
| `include_private/jsdrv_prv/mb_drv.h` | Add `state_fetch_prefixes`, `open_ready` hooks; add `jsdrvp_mb_dev_state_fetch_start` service prototype |
| `src/mb_device.c` | Drop `STATE_FETCH_PREFIXES`, `EV_SENSOR_READY`, `ST_AWAIT_SENSOR_READY`, sensor-ready publish detection; plumb prefixes through `sf`; split `on_open_enter`; expose state_fetch_start wrapper |
| `src/js320_drv.c` | Add `js320_state_fetch_prefixes`, `js320_open_ready`, sensor-ready handling in `js320_handle_publish` + `js320_on_timeout`, struct field `waiting_for_sensor` |

No test file changes anticipated; existing `js320_drv_test` and
`frontend_test` should pass unchanged because behaviour is preserved.

## Interactions with `open_state_management.md`

Some items in that plan subsume or complement items here:

- **`STATE_FETCH_PREFIXES` removal**: the open-state-management plan
  proposes replacing prefix iteration with a null-target GET_INIT
  that enumerates all subtrees. If firmware supports null-target
  GET_INIT, the `state_fetch_prefixes` callback proposed above
  becomes a pure fallback (or is unused). If firmware does not yet
  support null-target, the callback is the immediate pragmatic fix.

- **`publish_defaults` hook**: the open-state-management plan proposes
  replacing the per-driver `publish_defaults` hook with a generic
  `SET_CMD`-based mechanism. This refactor does not touch that hook
  (the earlier revert put it back). Depending on ordering:
  - If this refactor lands first: keep `publish_defaults` hook; the
    state-management redesign removes it later.
  - If state-management lands first: skip the `publish_defaults`
    discussion in this refactor.

Either order works; prefer this refactor first because it's scoped,
mechanical, and unblocks the state-management work by clarifying the
hook surface.

## Migration / order of operations

1. Add the two new hooks + service prototype to `mb_drv.h` (no
   behavior change, pure API addition).
2. Expose `jsdrvp_mb_dev_state_fetch_start` (thin wrapper; `on_open_enter`
   still calls the existing static unchanged).
3. Plumb `sf->prefixes` from `drv->state_fetch_prefixes()` with a
   fallback to the existing `{'c','s',0}` string (preserves behavior
   if `js320_drv.c` hasn't wired up yet).
4. Add `js320_state_fetch_prefixes` in `js320_drv.c`; assign in
   factory.
5. Delete the now-orphaned `STATE_FETCH_PREFIXES` constant from
   `mb_device.c`.
6. Add `open_ready` hook plumbing in `on_open_enter`.
7. Implement `js320_open_ready`, timeout, and
   `js320_handle_publish` sensor-ready detection in `js320_drv.c`.
8. Delete `ST_AWAIT_SENSOR_READY`, `EV_SENSOR_READY`, their state-
   machine entries, and the `handle_in_publish` detection block.
9. Run fuzz seed 42 for 60 s + seed sweep 0..9 × 10 s. Expect:
   sensor-ready interlock still fires (now from JS320 driver),
   state_fetch succeeds on the first GET_INIT for both `c` and `s`,
   no host-side regressions, no new warnings besides the expected
   sample_id skip pattern (blocked on `open_state_management.md`).

Each step compiles and runs. Incremental commits possible, though the
whole refactor is small enough to land as one or two commits.

## Verification

- Build: `cmake --build cmake-build --config Debug` clean.
- Tests: `ctest --test-dir cmake-build -C Debug` 27/27 pass.
- Fuzz (JS320): 60 s seed 42 — `state_fetch: GET_INIT for 'c'` /
  `'s'` still appears on each open; `ST_AWAIT_SENSOR_READY` no longer
  appears in logs (new logs come from JS320 driver); no hangs/crashes.
- Grep check: `grep -n "sensor\|'s'\|STATE_FETCH_PREFIXES" src/mb_device.c`
  returns zero matches after the refactor.
- Hypothetical single-task device test: a stub driver that sets
  `state_fetch_prefixes -> "a"` and returns NULL for `open_ready`
  should complete open cleanly when wired to a MiniBitty-protocol
  device that publishes only under `a/...`. (No such device exists
  today; this is an architectural criterion rather than a test.)

## Out of scope

- Firmware-side null-target GET_INIT enumeration (covered by
  `open_state_management.md`).
- Replacing `publish_defaults` / `SET_CMD` rework (covered by
  `open_state_management.md`).
- `js110_usb.c` / `js220_usb.c` — they bypass `mb_device` and aren't
  affected.
