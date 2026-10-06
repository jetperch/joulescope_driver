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

# Pubsub API: request suffixes and subscribe flags

**Status**: proposed (needs a decision)
**Created**: 2026-10-06

## Context

The original pubsub design mirrored the device's pubsub and used topic
suffix characters as discriminators, so one string-topic transport could
carry requests and responses.  The JS320 and MiniBitty moved the device
side to typed messages, so the device protocols no longer use suffixes.
The host API still declares the full request and response set, but only
half of it was ever built.

This plan came out of `completed/dead_api_audit.md` item 1 and
`open_issues.md` (2026-10-06).

## Current state

Host pubsub (`include/jsdrv.h`, `src/pubsub.c`, `src/topic.c`):

| Suffix | Meaning | Status |
|--------|---------|--------|
| `$` | metadata response | Live: routed by `publish_meta`, retained |
| `#` | return code | Live: routed by `publish_return_code` |
| `%` | metadata request | `jsdrv_query("a/b%")` returns cached metadata only; unused |
| `&` | query request | Stripped in `jsdrv_query()` and `publish_return_code`; otherwise unused |
| `?` | query response | Stripped in `publish_return_code`; otherwise unused |

- A publish to `a/b%`, `a/b&` or `a/b?` goes through `publish_normal`
  and creates a junk topic node named `b%`, `b&` or `b?`.
- `JSDRV_SFLAG_METADATA_REQ`, `QUERY_REQ` and `QUERY_RSP`
  (`jsdrv.h:578-584`) have no effect: `publish()` (`pubsub.c:505-521`)
  only delivers RETURN_CODE, METADATA_RSP and PUB.
- The doc comments disagree.  `jsdrv.h:577,581` give the examples `"$"`
  and `"?"` for the request flags (the JS220 wire characters), while
  `jsdrv.h:92-97` and the Python `SubscribeFlags` docstrings say `%` and
  `&`.
- Python exposes the flags as `SubscribeFlags.METADATA_REQ`, `QUERY_REQ`
  and `QUERY_RSP` (`binding.pyx:723-732`), the string names
  `'metadata_req'`, `'query_req'` and `'query_rsp'` for `subscribe()`
  (`binding.pyx:735-746`), and the enum in `c_jsdrv.pxd:224-227`.  The
  `SubscribeFlags.PUB` docstring says "Do not receive" but means
  "Receive".

Device protocols, which are separate from these constants:

- JS220 (`js220_usb.c`): at RESUME open the driver sends the literal
  topics `"$"` (all metadata) and `"?"` (all values) to the instrument.
  Replies arrive as `topic$` and `topic?`, and `handle_stream_in_pubsub`
  strips the `?`.  Here `?` is the request, unlike the host meaning.
- MiniBitty and JS320 (`mb_device.c`): typed `MB_STDMSG_STATE` messages
  (GET_INIT, GET_NEXT, SET_CMD) and metadata blobs from `././info`.  No
  suffixes.

So changing the host API does not affect any device protocol.  The real
question is whether the host API needs its own "ask the device now"
path.

## Options

### A. Implement on-demand requests

A publish to `a/b%` or `a/b&` goes to the owning device driver, which
reads from the device and answers with `a/b$` or `a/b?`.

- Pro: an app can force a fresh read instead of trusting the host cache.
- Con: every device driver (JS110, JS220, mb_device) needs a handler, and
  JS110 has no device-side pubsub to forward to.
- Con: it duplicates existing paths.  `jsdrv_query` answers synchronously
  from the retained cache; RESUME open reads device state and DEFAULTS
  pushes defaults.
- Con: requests wait on USB instead of returning from the cache.
- Only worth it with a concrete case where the cache goes stale, such as
  a device changing a value without publishing it.

### B. Deprecate, then remove (recommended)

- Pro: a smaller API whose documentation matches its behavior, and no
  junk topic nodes.
- Con: the flags are public C enum values and Python constants.
  Deprecating first and keeping the bit values reserved means code that
  sets them still compiles and keeps doing nothing.

### C. Leave as is

- Pro: no work.
- Con: the docs keep promising behavior that does not exist, and a publish
  with a stray suffix keeps creating topic nodes.

## Decisions needed

1. A or B.  If A, name the stale-cache case it solves.
2. For B, keep `'%'` as the metadata form of `jsdrv_query()` (tested and
   documented), or remove it too.  Keeping it is cheap and gives a
   one-call metadata lookup; removing it leaves `$` subscriptions with
   RETAIN as the only metadata path.
3. For B, the removal release: deprecate in the next minor release and
   remove at the next major API revision?

## Plan (option B)

Each stage builds, passes all tests, and is reviewable on its own.

### Stage 1: reject request-suffixed publishes

- `src/pubsub.c`: a publish to a topic ending in `%`, `&` or `?` returns
  `JSDRV_ERROR_PARAMETER_INVALID` (with a return code when the publish
  has a source) and creates no topic node.
- `test/pubsub_test.c`: the publish fails and `topic_find` finds no node.
- If `'%'` stays: add a `jsdrv_query("a/b%")` metadata test, including an
  unknown topic and a topic with no metadata.

### Stage 2: deprecate the flags and suffixes

- `include/jsdrv.h`: mark `JSDRV_SFLAG_METADATA_REQ`, `QUERY_REQ`,
  `QUERY_RSP`, `JSDRV_TOPIC_SUFFIX_QUERY_REQ` and `QUERY_RSP` (and
  `METADATA_REQ` if removed) as deprecated and reserved.  Fix the suffix
  table at `jsdrv.h:92-97` and the flag comments.
- `pyjoulescope_driver/binding.pyx`: mark the `SubscribeFlags` members and
  the `'metadata_req'`, `'query_req'` and `'query_rsp'` names deprecated
  (a `DeprecationWarning` from `subscribe()`), and fix the
  `SubscribeFlags.PUB` docstring.
- `CHANGELOG.md`: the deprecation note.

### Stage 3: remove (next major API revision)

- Delete the deprecated enum values (keep the bits reserved in a
  comment), the suffix macros, the Python constants and names, and the
  `c_jsdrv.pxd` entries.
- `src/topic.c`: simplify `is_suffix_char`.  `src/pubsub.c`: simplify the
  suffix switch in `publish_return_code` and `query`.
- Check the Node binding and the docs for references.

## Verification

- ctest and the Python tests on Windows and Linux.
- `grep` for the removed names across `src/`, `include/`,
  `pyjoulescope_driver/`, `node_api/`, `example/` and `doc/`.
