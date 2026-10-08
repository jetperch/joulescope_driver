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

# Open-path state management redesign

**Status**: completed
**Updated**: 2026-10-08

## Status summary

The sections from "Context" through "Verification" are the original
2026-04 plan, kept for history.  The two "Implementation status" sections
at the end describe what shipped.

| Item | Status |
|---|---|
| DEFAULTS = host value, else metadata default | Done: mb_device 2026-06, JS220 2026-10 |
| Separate `OVERRIDE` mode | Dropped: DEFAULTS covers it |
| Batched `SET_CMD` restore | Done 2026-06 (mb_device) |
| Drop `STATE_FETCH_PREFIXES` | Done 2026-06, using the link identity instance |
| Metadata `ro` flag, `jsdrv_meta_flags()` | Done 2026-04 (dd0103b) |
| Settable filter, not name heuristics | Done 2026-10: `jsdrv_meta_is_settable()` |
| Host-value gather completion | Done 2026-10: replaced the `mbg/` sentinel |
| JS220 `defaults` open (#16) | Done 2026-10 |
| No `default` on `!` topics | Done 2026-10: js320 and MiniBitty firmware |
| Fuzz coverage of the open-mode mix | Done 2026-10: `example/fuzz.c` `--open-mode` |
| General child-instance discovery | Moved to `../distributed_pubsub.md` |

## Context

The current `jsdrv_device_open_mode_e` enumeration defines two
semantically-interesting modes:

```c
enum jsdrv_device_open_mode_e {
    JSDRV_DEVICE_OPEN_MODE_DEFAULTS = 0,  // restore device to power-on state
    JSDRV_DEVICE_OPEN_MODE_RESUME = 1,    // adopt device's current state
    JSDRV_DEVICE_OPEN_MODE_RAW = 0xFF,    // internal tools only
};
```

Two problems surfaced during the 2026-04-16 fuzz session:

1. **DEFAULTS doesn't actually restore defaults.** For `mb_device` /
   MiniBitty-protocol drivers (JS320, future MiniBitty devices), it is
   a no-op until the upper driver opts in via a per-driver hook — and
   no driver implements the hook. The fuzz session attempted a generic
   pubsub-walk that publishes each topic's metadata `default` via the
   normal publish path, but it's fragile:
    * `publish_normal` dedups against the pubsub retained value, so
      defaults matching a stale host-cached value never reach the
      device.
    * Each topic's `s/dwnN/N = 0` default travels as an independent
      publish, taking `N × (host → device ul.cmd_q → LL → USB → wire)`
      round trips during open.
    * Topic filtering relies on leaf-name heuristics (`!`, `_`
      prefixes) rather than an explicit writable/read-only signal
      from the metadata.
2. **No way for the host to force the device to the host's stored
   state.** Common scenario: a persistent host session (e.g., the
   Joulescope UI) has cached settings from before a device power
   cycle; on reconnect the host should push those settings to the
   freshly-booted device, not adopt whatever the device happens to
   have. `RESUME` adopts; there's no push-from-host mode today.

The generic-walk attempt is in uncommitted working-tree changes on
`feature/js320` branch (see `fuzz_session_wip_2026-04-16.md`). Those
changes are being reverted in favor of this redesign.

## Goals

* Three meaningful open modes with clear, implementable semantics.
* Single atomic "state set" round-trip per open, instead of one publish
  per parameter.
* No hardcoded subtree prefix list (`c`, `s`, …) in the host driver.
  Any current or future MiniBitty device must work without code
  changes.
* The host's pubsub cache accurately reflects device state after open
  completes.

## Design

### Mode semantics

| Mode | Per writable topic, pushed to device + host cache | Notes |
|---|---|---|
| `DEFAULTS` (0) | host's retained value if present, else metadata `default` | merges the old DEFAULTS + OVERRIDE; read-only/non-retained excluded |
| `RESUME` (1) | (nothing pushed) — host adopts the device's current state | `state_fetch` populates the host cache |
| `RAW` (0xFF) | unchanged | LL-only open, existing behaviour |

> **Implemented 2026-06.** The separate `OVERRIDE` mode was dropped: the
> `DEFAULTS` semantic above (host value else default) covers both cases.
> The change is **host-only** — no `minibitty` firmware changes were
> needed.  See "Implementation status" at the end of this document.

`DEFAULTS` and `OVERRIDE` both push state *to* the device using the
same MiniBitty protocol primitive: a batched `MB_STDMSG_STATE_TYPE_SET_CMD`
with a list of `mb_stdmsg_state_entry_s` TLVs.

### Protocol flow

Current (fuzz session's interim design):
```
state_fetch meta → state_fetch values → for each default topic: jsdrvp_mb_dev_publish_to_device(...)
```

Proposed:
```
state_fetch meta → decide target state → SET_CMD(TLVs, FLAG_START|FLAG_END) → device applies atomically → SET_RESP
```

The `MB_STDMSG_STATE_TYPE_SET_CMD` primitive is already defined in
`minibitty/include/mb/stdmsg.h`:

```c
enum mb_stdmsg_state_type_e {
    ...
    MB_STDMSG_STATE_TYPE_SET_CMD  = 4,
    MB_STDMSG_STATE_TYPE_SET_RESP = 5,
};
struct mb_stdmsg_state_entry_s {
    char topic[MB_TOPIC_LENGTH_MAX];
    uint8_t value_type;
    uint16_t size;
    union mb_value_u value;
    // ...
};
```

Per the header, `SET_CMD`'s payload is a `list of mb_stdmsg_state_entry_s`.
The host packs all settings for a mode into one or more SET_CMD frames
(using `FLAG_START` / `FLAG_END` to chunk across MTU boundaries). The
device's pubsub state iterator applies each entry in order and
responds with `SET_RESP` carrying aggregate status.

### Building the TLV list

For `DEFAULTS`: after `state_fetch` has pulled metadata into
the host pubsub, walk the metadata: for every topic with a `"default"`
field and without a write-disabled flag, emit a TLV with the default
value.

For `OVERRIDE`: walk the host pubsub's retained values. For every
topic with a retained value (excluding `!`-prefixed commands and
`_`-prefixed device-reported state), emit a TLV.

Shared helper: `mb_device_state_set(dev, mode)` that constructs the
TLV list, chunks into SET_CMD frames, sends, and waits for SET_RESP.

### Dropping the hardcoded `c`/`s` subtree list

`STATE_FETCH_PREFIXES = {'c', 's', 0}` at `mb_device.c:53` is a
JS320-specific hack that works today only because every current
MiniBitty device happens to split topic-space the same way.

Replacement: issue a single `GET_INIT` with a null `target_topic`
(per the stdmsg.h header comment "null = all"). The device enumerates
every top-level subtree it hosts in the `GET_RSP` stream. The host
discovers the set of prefixes at runtime.

This requires a firmware-side confirmation: does
`minibitty/src/pubsub.c:state_get_init` currently honor a null target,
or is the current behaviour to require a specific prefix? If the
former, no firmware change is needed. If the latter, firmware must be
updated to emit a root-level enumeration when target is empty.

Pubsub-topic enumeration is a prerequisite task — see §Open questions.

### Metadata `writable` marker

Today, the host heuristically skips leaves whose name starts with
`!` (commands/streams/events) or `_` (read-only status). Both are
brittle — nothing enforces the naming conventions.

Proposed: extend `jsdrv/meta.h` metadata schema with an explicit
`"flags"` field whose presence of `"ro"` (read-only) excludes the
topic from `SET_CMD` construction. A topic with `"default"` absent
is also excluded (regardless of flags). Update
`minibitty/mbgen/...` metadata generators to emit `"flags": "ro"` for
`_*` topics and for anything the device considers read-only.
Metadata-consumer side adds `jsdrv_meta_flags()` that returns a
bitmask and a `JSDRV_META_FLAG_RO` constant.

## Critical files

Original plan; see "Status summary" for what changed.


* `C:\repos\Jetperch\joulescope_driver\include\jsdrv.h` —
  add `JSDRV_DEVICE_OPEN_MODE_OVERRIDE = 2`, update comments.
* `C:\repos\Jetperch\joulescope_driver\include\jsdrv\meta.h` —
  add `jsdrv_meta_flags()`, `JSDRV_META_FLAG_RO`.
* `C:\repos\Jetperch\joulescope_driver\src\meta.c` — implement the
  flags parser (pattern already used for `jsdrv_meta_default`).
* `C:\repos\Jetperch\joulescope_driver\src\mb_device.c` — new
  `mb_device_state_set(self, mode)` + state machine wiring in
  `on_open_enter` / `state_fetch_complete`; drop
  `STATE_FETCH_PREFIXES`; handle `SET_RESP`.
* `C:\repos\Jetperch\minibitty\src\pubsub.c` — honor null `target_topic`
  in GET_INIT (if not already); ensure SET_CMD handler exists for the
  device-side state iterator.
* `C:\repos\Jetperch\minibitty\include\mb\stdmsg.h` — no change (types
  already exist).
* `C:\repos\Jetperch\js320\mbgen\...\mb_pubsub_initialize.c` and
  `minibitty/mbgen/...` — metadata generation adds
  `"flags": "ro"` for read-only topics (`_state`, versions, fw_*, etc.).
* `C:\repos\Jetperch\joulescope_driver\example\fuzz.c` — fuzz weights
  for the new `OVERRIDE` mode.
* `C:\repos\Jetperch\joulescope_driver\test\*` — new tests for each
  open-mode end state (`DEFAULTS`, `RESUME`, `OVERRIDE`).

## Implementation ordering

1. **Firmware**: verify GET_INIT null-target enumeration in
   `minibitty` pubsub; verify SET_CMD processing path; add if missing.
2. **Metadata flags**: add `"flags"` to the schema; update mbgen
   generators; add `jsdrv_meta_flags()` parser; unit-test.
3. **Host `OVERRIDE` + unified `mb_device_state_set`**: build the TLV
   list on the host, send as SET_CMD, wait for SET_RESP. Wire into
   `on_open_enter` for modes 0 and 2.
4. **Drop STATE_FETCH_PREFIXES**: switch GET_INIT to null-target.
5. **Fuzz coverage**: add the OVERRIDE transition to fuzz; verify
   sample_id skips go to zero across open-mode mix.

## Open questions

* ~~Does `state_get_init` honor a null `target_topic`?~~  Moot: the host
  manages the single instance named in the link identity.
* ~~Should `OVERRIDE` preserve topics absent from the host cache?~~
  Moot: `OVERRIDE` was dropped.  DEFAULTS forces absent topics to their
  metadata default.
* ~~Is the `SET_CMD` MTU one frame or a chain?~~  A chain: the host chunks
  entries across frames with `FLAG_START` / `FLAG_END`.  The firmware
  reliably acks only the END frame.
* ~~Do we need a `/!sync` command to re-force state on an open device?~~
  No.  Host and device state must not diverge once open completes, so
  any divergence is a bug to fix, not to resync.

## Relationship to the fuzz session WIP

The `_/!publish_defaults` pubsub command and generic walk introduced
during the fuzz session will be **reverted**. The generic-walk idea
is correct in spirit but uses the wrong primitive (individual
publishes with dedup) for the wrong reason (incomplete host cache).
`SET_CMD` is a better fit for the atomic "restore to state X"
semantic.

Separate non-state-related fuzz improvements from that session
(close-path timeouts, sensor-ready interlock, GET_INIT BUSY retry,
LEAK-on-timeout join, diagnostic logs, crash handler) are independent
of this redesign and will be committed as-is.

## Verification

Each mode has a distinct end-state that's easy to assert in an
end-to-end test:

* `DEFAULTS`: after open, every writable topic's retained pubsub value
  equals its metadata `default`.
* `RESUME`: host retained values match what the device reported in
  GET_RSP.
* `OVERRIDE`: host retained values match the pre-open cache; device
  state matches host cache.

Under fuzz: `sample_id skip` count must be 0 (any leftover is a
genuine protocol bug, not a state-management bug). dwnN mismatches of
the kind observed during the 2026-04-16 session must be gone.

## Implementation status (2026-06)

Implemented host-side in `src/devices/mb_device/mb_device.c`,
`src/devices/js320/js320_drv.c`, and
`include_private/jsdrv_prv/devices/mb_device/mb_drv.h`.  Validated on
hardware via `test/hw/test_open_state.py`.

What shipped, and how it differs from the original plan above:

* **Single managed instance.** mb_device manages only the pubsub
  instance named in the link identity (`identity.pubsub_prefix`, `'c'`
  for the JS320).  The hard-coded `{"h","c","s"}` RESUME replay loop and
  the `"cs"` `STATE_FETCH_PREFIXES` default are gone, along with the
  `state_fetch_prefixes` and `publish_defaults` driver hooks.

* **`open_seq` sequencer.** A macro `open_phase`
  (DISCOVER→META→GATHER→SET→VALUES→DONE) inside `ST_OPEN` drives the
  restore.  RESUME is VALUES→META→DONE; DEFAULTS adds the gather + SET.
  Metadata is fetched before the device values so DEFAULTS can build the
  SET and capture host values before the read-back overwrites the cache.

* **No targeted `./info` GET was needed.** Instead, DEFAULTS runs a
  DISCOVER GET that enumerates the instance with host publishing
  *suppressed* (it only captures the `././info` blob descriptors), then
  reads metadata, then a second GET publishes the read-back values.

* **Host-value gather via loopback sentinel** (replaced 2026-10-07 by
  the subscribe completion, see `completed/open_gather_completion.md`).
  After subscribing RETAIN to the instance subtree, the device published
  a marker to a unique **out-of-prefix** topic (`mbg/<txn>`) it also
  subscribed to.
  Two non-obvious constraints, both learned the hard way:
  - The sentinel topic must NOT match a device prefix (`device_lookup`
    keys on the first 3 path segments): publishes under the device
    prefix are tagged with the device as origin and suppressed from
    echoing back (`is_same_subscriber`), so they never return.
  - Each path segment must be <= 7 chars (`JSDRV_TOPIC_LENGTH_PER_LEVEL`
    is 8); the first attempt used `mbgather/<8-hex>` and was silently
    rejected, forcing the gather onto its 250 ms timeout fallback.

* **Batched `SET_CMD`.**  Writable, retained, has-`default` topics are
  packed into `mb_stdmsg_state_entry_s` TLVs (host value if gathered,
  else metadata default) and chunked to fit one frame.  The firmware
  applies every frame but **reliably acks only the END frame**, so all
  chunks are sent back-to-back and only the final SET_RESP is awaited.

* **Child sync — open WAITS for it; order is `c → s → h → OPEN#`.**
  After the core `'c'` sync, the `drv->open_children` hook lets the
  driver DEFER OPEN# while child instances are synced, so the device is
  not declared open with `'s'`/`'h'` topics missing (one-shot tools like
  `jsdrv info` see them).
  - **`'s'` (sensor device instance):**
    `jsdrvp_mb_dev_instance_state_sync(dev,'s',emit_open=false)` reruns
    the open-mode sequence against the sensor.  js320 keys it off the
    **closed-loop** signal `c/comm/sensor/state == 1` (the ctrl reports
    the sensor link up; it tears down on close and re-establishes on
    open, so the 0→1 transition fires every open).
  - **`'h'` (host-side instance):**  `h/fp`, `h/fs`, `h/i_scale`,
    `h/v_scale` are owned by the js320 driver's `handle_cmd`, not a
    device pubsub instance.  `jsdrvp_mb_dev_host_replay(dev,'h')`
    re-delivers the host's retained `h/*` values to `handle_cmd`,
    restoring the driver's internal state from the host cache.  It runs
    AFTER `'s'` (via the `drv->on_instance_synced` hook) so a host-cached
    `h/fs` wins over the sensor sync's `s/dwnN/N`.
  - The driver then completes the open: `on_instance_synced('s')` →
    `host_replay('h')` → `jsdrvp_mb_dev_open_complete()`.  If the sensor
    never comes up, `on_timeout` does `host_replay('h')` +
    `open_complete()` (under the `jsdrv_open` timeout) so open still
    completes — it never FAILS on the sensor, letting the UI run a
    firmware update.  A general child-instance discovery abstraction
    remains future work.

* **Float metadata dtypes.**  `src/meta.c`'s `dtype_map` lacked `f32`/
  `f64`, so `jsdrv_meta_value` rejected any publish to a float-typed
  topic (e.g. `h/i_scale`/`h/v_scale`) with `PARAMETER_INVALID`.  Added
  both (tested in `meta_test.c::test_float_dtype`).

* **MiniBitty retain convention.**  Every subtopic whose leaf does not
  start with `'!'` is retained, regardless of metadata.  `handle_in_publish`
  marks such device publishes `JSDRV_UNION_FLAG_RETAIN` so the host cache
  tracks runtime transitions (e.g. `c/comm/sensor/state` 0→1); without
  it a non-retained publish would *clear* the prior cached value.

* **Failure handling.**  Every step (gather completion, SET_RESP, missing
  metadata) advances on timeout rather than aborting; open always
  reaches OPEN#.

Verification end-states (the `OVERRIDE` row is subsumed by `DEFAULTS`):

* `DEFAULTS`: with an empty host cache, every writable topic equals its
  metadata `default`; with a cached host value, that value is preserved.
* `RESUME`: host retained values match the device's GET_RSP.

## Implementation status (2026-10)

Commits e01817d through f44b562 on main.  Validated on hardware with
`test/hw/test_open_state.py` on JS220+ 002122 (fw 1.3.0) and JS320 8W2A
(fw 1.1.11).

* **JS220 `defaults` open
  ([#16](https://github.com/jetperch/joulescope_driver/issues/16)).**
  `js220_usb.c` previously pushed nothing in DEFAULTS mode, so settings
  from a previous session persisted on the instrument until power cycle.
  It now applies the same semantic as mb_device: it records each `$`
  metadata default during the open, gathers the host's retained `c/` and
  `s/` values, pushes host value else default, then reads the device
  state back.  The JS220 has no `SET_CMD`, so it pushes one publish per
  topic.  `/ctrl` topics are always left at their default, so an open
  never restarts streaming, and `*/dwnN/N` is skipped because `h/fs`
  owns it.  `mode='restore'` (RESUME) is the opt-out.  The CHANGELOG
  2.5.0 entry leads with this behavior change.
* **Shared settable filter.**  `jsdrv_meta_is_settable(topic, meta)` in
  `src/meta.c` (`include_private/jsdrv_prv/meta_settable.h`) returns true
  for a topic that is not `ro`, contains no `!` and has a scalar dtype.
  Both drivers then take the value from the public `jsdrv_meta_default()`.
  The topic check stays because older firmware defines defaults on `!`
  topics.
* **Gather completion.**  Subscribing with `JSDRV_PUBSUB_SUBSCRIBE_DONE`
  (`"_/!subd"`) makes pubsub send `JSDRVP_MSG_SUBSCRIBE_DONE` to that
  subscriber after its retained values.  Both drivers end the gather on
  it, which removed the `mbg/` loopback sentinel and its two constraints
  above.  See `completed/open_gather_completion.md`.
* **Firmware metadata.**  `!` topics no longer define a `default`: 10 in
  js320 `firmware/fpga_mcu/src/app.c` and `tick/!ev` in MiniBitty
  `src/tasks/sys.c`.  The JS320 had 12 such defaults before; fw 1.1.11
  has none.

`test/hw/test_open_state.py` now runs on both models (the sensor tests
are JS320 only).  It checks that RESUME adopts the device value and a
fresh DEFAULTS open resets it, that DEFAULTS preserves a host value, and
that `ro` topics are not corrupted.  A separate JS220 scope run found 0 of
8 changed settings left after a fresh `defaults` open.

## Fuzz coverage (2026-10-08)

`example/fuzz.c` now picks `defaults` or `restore` at random for each
open (`--open-mode defaults|restore|mix`, default `mix`).  After each
open it reads the host `h/fs`.  At each stream stop it fails when:

* a channel's `sample_id` is not continuous, or
* the delivered rate (`sample_rate / decimate_factor`) differs from the
  host `h/fs`, which means the host and device state diverged.

JS220+ 002122 passes: 60 opens and over 200 streams across three
4-minute runs.  JS320 8W2A fails within minutes in both open modes, on
stream discontinuities that are unrelated to the open mode.  They are
filed in `../js320_stream_discontinuity.md`, along with `h/fs` having no
host value after open on both models.
