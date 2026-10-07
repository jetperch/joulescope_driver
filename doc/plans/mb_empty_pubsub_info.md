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
# MiniBitty adapter reports an empty ././info

**Status**: proposed
**Created**: 2026-10-07

## Context

Found while adding the `values` entry point.  Opening the MiniBitty
adapter `u/mb/93NP` in restore mode logs:

    STDMSG malformed: size 0 < 8 byte header

The binding logs this warning for the retained `u/mb/93NP/+/./info`
value.  The driver debug log shows that the empty value comes from the
device: `mb_device.c` `state_fetch_on_rsp` publishes the GET_RSP state
entry for `./info` with `vtype=STDMSG` and `vsize=0`.

The empty value has a second, more important effect:
`state_fetch_process_info_entry` requires at least 12 bytes, so the host
captures no metadata blob descriptors and skips the metadata fetch.
`pyjoulescope_driver metadata -d mb` returns `{}`.

The JS320 controllers on firmware 1.1.9 publish a valid `c/./info` and
`s/./info`, and their metadata loads in restore mode.  The adapter
reports `+/sys/fw/version = 1`, an early build.  The current minibitty
`src/pubsub.c` sets `topic->size` from `mb_stdmsg_pubsub_value_size` on
publish and copies it into each GET_RSP entry, which looks correct.  The
adapter firmware is the likely cause.

## Plan

1. Back up the adapter state, then update the adapter to the current
   minibitty adapter firmware with `pyminibitty product update`.  Repeat
   `pyjoulescope_driver values -d mb` and `metadata -d mb`.
2. If `./info` is still empty, trace the firmware `././info` publish at
   `mb_pubsub` initialization and the `state_get_next` entry for it.
   Check that `topic->size` is set for the static `./info` topic.
3. Host: when `./info` is empty or missing, log once at warning level
   in `mb_device.c` that the device metadata is unavailable, so the
   failure is not only visible as a decode warning in the binding.
4. Host: the binding decodes `c/./info` on the JS320 as
   `{'version': 9, 'version_major': 9, ...}`, which appears to read the
   stdmsg type as the version.  Check the `mb_stdmsg_header_s` layout
   against `_jsdrv_union_to_py` and add MB_STDMSG_PUBSUB_INFO decoding.

Step 1 needs hardware.  Steps 3 and 4 build and pass the unit tests on
their own.
