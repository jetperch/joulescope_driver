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
# Replace the open gather sentinel with a subscribe completion

**Status**: complete (2026-10-07)
**Created**: 2026-10-07

## Context

A `defaults` open sets each writable device topic to the host's retained
value, when present, else to its metadata default.  To collect the host's
retained values, the device driver subscribes with `JSDRV_SFLAG_RETAIN`
to its own device topics.  Pubsub delivers the matching retained values
synchronously during the subscribe.  The driver must then learn when the
delivery is complete.

Today both drivers publish a sentinel to `mbg/...` after the subscribe,
and wait for it to loop back (`JSDRVP_GATHER_TOPIC_PREFIX` in
`include_private/jsdrv_prv/frontend.h`):

* mb_device (JS320): `open_seq_start_gather()`, asynchronous, in the
  device state machine.
* JS220: `open_defaults_gather()`, which blocks in `d_open` and pumps
  `ul.cmd_q`.

The frontend delivers in FIFO order, so the sentinel arrives after the
retained values.  The sentinel cannot use the device prefix: pubsub
suppresses publishes that the device originates from echoing back to it.

## Problems with the sentinel

Found while fixing #16 (JS220 `defaults` open), 2026-10-07.

1. **Silent warnings depend on the first character.**  The frontend
   (`handle_backend_msg` in `src/jsdrv.c`) publishes backend topics
   starting with `m` without a device lookup, because that path serves the
   memory buffers (`m/...`).  Any other sentinel name logs
   `device_lookup(...) failed` and `no device match` warnings on every
   open, as `j2g/...` did during development.  The mb_device comment
   attributed this to "< 3 path segments", which only explains why the
   echo suppression does not apply.  This is now documented, but it is
   still coupling to an unrelated route.
2. **Pubsub never frees topics.**  `topic_free()` only runs at
   `jsdrv_pubsub_finalize()`.  Each distinct sentinel name permanently
   adds a topic node.  The names must therefore repeat, which conflicts
   with uniqueness (problem 3).  The JS220 uses one fixed name per device.
3. **The JS320 names are shared.**  `open_seq_begin()` resets
   `transaction_id` to `0x5F00`, so every JS320 with the same firmware
   uses `mbg/5f17` (`c` instance) and `mbg/5f32` (`s` instance), observed
   on 8W2A over repeated opens.  When two JS320s open at the same time,
   each can receive the other's sentinel.  Analysis, not reproduced (one
   JS320 available): this does not lose data, because a device's retained
   values are queued during its own RETAIN subscribe, before it subscribes
   to the sentinel.  The early end only creates a late sentinel, which
   both drivers now ignore.  The JS220 hash name can also collide, with
   the same result.
4. **Applications see the sentinels.**  They are ordinary publishes, so
   an application that subscribes to the root, or to `mbg`, receives them.
5. **The JS220 gather needs a timeout and deferral.**  It blocks for up to
   1 s if the sentinel never returns, and it must defer unrelated commands
   that arrive before the sentinel (`open_deferred`).

## Proposal

Pubsub sends a completion message to the subscriber itself, at the end of
a RETAIN subscribe that requests it.  The message is delivered through the
subscriber callback, so it reaches only that device, immediately after its
retained values.

* Add `uint8_t done_rsp` (or a flags byte) to
  `struct jsdrvp_payload_sub_s`.  Keep `jsdrv_subscribe_flag_e` unchanged:
  it is public API.
* In `subscribe()` (`src/pubsub.c`), after `subscribe_traverse()`, when
  `done_rsp` is set, call the subscriber with a message on a new command
  topic, such as `JSDRV_MSG_SUBSCRIBE_DONE` (`"@/!subdn"`; each segment is
  at most 7 characters).  Its value is the subscribed topic string, so a
  device with several gathers can match them.
* Add `jsdrvp_device_subscribe_done()`, or a parameter on
  `jsdrvp_device_subscribe()`, in `src/jsdrv.c` and `frontend.h`.
* Devices handle the message in their `JSDRV_MSG_COMMAND_PREFIX_CHAR`
  branch of `handle_cmd`.

This removes problems 1 to 4: no topic node is created, nothing is
published to other subscribers, and no name needs to be unique.  The JS220
still pumps `ul.cmd_q` (problem 5 shrinks to a safety timeout).

## Stages

Each stage builds, passes `ctest` and the Python tests, and is reviewable
on its own.

1. **Pubsub and frontend.**  Add the payload field, the completion
   message and the frontend function.  Unit tests in `test/pubsub_test.c`:
   the subscriber receives every retained value, then the completion;
   another subscriber to the same topic does not receive the completion;
   no completion without `done_rsp`; the completion arrives for a topic
   with no retained values.  No device changes.
2. **mb_device.**  Replace the sentinel in `open_seq_start_gather()` and
   `open_seq_on_gathered()` with the completion message.  Update
   `test/devices/mb_device/mb_device_test.c`.
3. **JS220.**  Replace the sentinel in `open_defaults_gather()`.  Update
   `test/devices/js220/js220_usb_test.c`.
4. **Cleanup.**  Remove `JSDRVP_GATHER_TOPIC_PREFIX`, the `mbg/` notes in
   `src/jsdrv.c`, and the late sentinel handling in both `handle_cmd`
   functions.
5. **Hardware verification.**  `test/hw/test_open_state.py` on a JS220
   and a JS320.  Subscribe to the root during repeated opens and confirm
   that no `mbg/` publishes and no warnings appear.  If two JS320s are
   available, open them concurrently in `defaults` mode and confirm both
   restore host values.

## Outcome

Completed 2026-10-07 in commits 01ce3ae, 3c13dde, d6f8234, 0bdc9c6
and the stage 4 cleanup.

* The completion is requested with its own pubsub command topic,
  `JSDRV_PUBSUB_SUBSCRIBE_DONE` (`"_/!subd"`), not with the proposed
  `done_rsp` payload field.  The field version (01ce3ae) failed on
  hardware: `jsdrvp_msg_alloc()` reuses pooled messages without clearing
  the payload, and the other subscribe paths never set the field.  Stray
  completions reached the JS320 fwup worker and failed its FPGA step with
  rc=9.  `test_subscribe_dirty_payload_no_done` in `test/pubsub_test.c`
  covers this.
* The JS220 still pumps `ul.cmd_q` during the gather, with a 1 s safety
  timeout.  Both drivers ignore a late completion.
* Hardware: `test/hw/test_open_state.py` passes on JS220+ 002122
  (fw 1.3.0) and JS320 8W2A (fw 1.1.11).  An application subscribed to
  `mbg` sees no publishes during repeated opens of either model, and no
  gather timeout warnings appear.  Concurrent opens of two JS320s were
  not tested (one unit available).
