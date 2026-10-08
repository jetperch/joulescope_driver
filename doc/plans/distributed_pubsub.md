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
# Distributed pubsub: child-instance discovery

**Status**: proposed
**Created**: 2026-10-08

## Context

Split out of `completed/open_state_management.md`.

A MiniBitty device is a distributed pubsub.  Each processor hosts one
instance, named by a one-character prefix.  The JS320 has three:

* `c`: the controller (STM32), named in the link identity, so mb_device
  syncs it generically.
* `s`: the sensor (fpga_mcu), reached through the controller's comm
  link.  It comes up after the controller, and goes down on close.
* `h`: host-side topics owned by the js320 driver, such as `h/fs`.

Only `c` is generic.  `js320_drv.c` hard-codes the rest through the
`open_children` and `on_instance_synced` hooks in `mb_drv.h`.  It waits
for `c/comm/sensor/state == 1`, syncs `s` with
`jsdrvp_mb_dev_instance_state_sync()`, then replays `h` with
`jsdrvp_mb_dev_host_replay()`, then completes the open.  A new device
with a different instance tree needs new driver code.

## Goal

mb_device discovers a device's child instances, waits for each to be
ready, and syncs them in order with the open mode, without driver code.
Host-side instances stay driver-owned, but use the same ordering hook.

## Open questions

* How does the device describe its instances?  Options: a topic in the
  core instance (such as `c/comm/+/state` plus each child's prefix), a
  field in the link identity, or metadata.
* Readiness: each child's link state is a topic today.  Is "state == 1"
  a convention that every MiniBitty comm link can follow?
* Ordering: `h` must follow `s` so a host `h/fs` wins over `s/dwnN/N`.
  Is a fixed order (device children, then host) enough?
* Children that never come up: today open completes without the sensor
  so a firmware update can still run.  Keep this per child.

## Stages

To be written once the open questions are answered.  Each stage must
keep `test/hw/test_open_state.py` and `fuzz` passing on the JS320.
