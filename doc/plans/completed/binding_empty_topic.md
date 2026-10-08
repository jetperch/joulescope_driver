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
# Python binding rejects the empty (root) topic

**Status**: completed
**Created**: 2026-10-07

## Context

Found while verifying doc/plans/completed/open_gather_completion.md.
Subscribing to the root topic from Python fails:

    d.subscribe('', 'pub', fn)
    IndexError: Out of bounds on buffer access (axis 0)

`binding.pyx` encodes each topic to a `const uint8_t[:]` memoryview and
passes `<char *> &topic_str[0]` to C (`publish`, `query`, `subscribe`,
`unsubscribe` and two internal publishes).  For `''` the view is empty,
so `[0]` raises.  The same pattern also relies on the NUL that CPython
stores after every `bytes` object, which the memoryview does not include.

The C API accepts the root topic: `jsdrv_subscribe(context, "", ...)` is
how applications observe every publish, and `pubsub.c` handles `""`.

## Proposal

Encode with an explicit terminator, `(topic + '\0').encode('utf-8')`, in
one helper used by all six call sites, so the view is never empty and the
C string is terminated by construction.

## Stages

1. Add the helper and use it at every call site.  Add Python tests for
   `subscribe('')` / `unsubscribe('')` on a driver without devices, and
   for a normal topic round trip.
2. Rebuild the extension and run `pyjoulescope_driver/test`.

## Outcome

Completed 2026-10-08.  `_c_str()` in `binding.pyx` encodes all seven call
sites, including the `rsp_topic` copy in `_pack_buffer_req`, which the
plan missed.  `test_root_topic` and `test_publish_query_round_trip` in
`pyjoulescope_driver/test/test_driver_subscribe.py` cover the fix.  All
214 Python tests pass, and `test/hw/test_open_state.py` passes on JS320
8W2A.
