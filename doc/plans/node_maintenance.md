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

# Node.js binding (node_api) maintenance plan

**Status**: proposed
**Created**: 2026-07-31 (design review 2026-07)

## Current state

The Node binding is functionally frozen while the C driver moved on:

- Last functional change: `fa97016` (2024-04-24).  Since then only
  binary-resolution (`f01afc2`, 2025-07) and npm audit (`677773f`,
  2026-01) commits.  The C API 2.0 redesign (2026-04) and all JS320
  support landed after the freeze.
- Stream-buffer payloads are unimplemented:
  `node_api/src/joulescope_driver.cc:274-289` — `buffer_info_to_js`,
  `buffer_rsp_to_js`, and the default `bin_to_js` all
  `return env.Undefined(); // todo`.  Any Node client using `m/…`
  buffers silently receives `undefined`.
- API subset vs Python: only `publish, query, finalize, device_paths,
  open, close, subscribe`.  Missing: `unsubscribe`, `unsubscribe_all`,
  `publish_and_wait`, `log_level`, constants (ErrorCode/LogLevel/
  SubscribeFlags/Field/ElementType), `Record`, `time64`, `MemClient`,
  firmware update (`program_js320` equivalent — contradicting
  `doc/js320_fwup.md:20-22`'s "any language binding" claim).
- `open()` mode doc (`node_api/index.js:71`) omits `raw` (0xFF) and uses
  `resume` where Python says `restore`.
- Test: `node_api/test/test_binding.js` is a 31-line smoke test that
  misuses `assert.doesNotThrow` with an async function (cannot catch a
  rejection), and CI (`.github/workflows/packaging.yml`, build_node_js)
  never runs `npm test` — the package is published with zero executed
  tests.
- Examples (`samples.js`, `statistics.js`) predate the JS320.

## Decision (2026-07-31)

**Minimal maintenance.**  Fix only what is broken; no feature-parity
effort.  In scope now (remediation Phase 4):

1. Implement the three buffer/binary converters (mirror
   `pyjoulescope_driver/binding.pyx:216-475`).
2. Run `npm test` in the build_node_js CI job; fix the async assertion
   misuse so the test can actually fail.

## Recommended follow-on (unscheduled)

Ordered by value if/when Node investment resumes:

1. `unsubscribe` / `unsubscribe_all` (currently subscriptions are
   permanent for the process lifetime — arguably "broken" for long-lived
   apps).
2. Constants export and `publish_and_wait` (small, unblocks most
   scripting use cases).
3. JS320 firmware update via `h/fwup/ctrl/!cmd` publish-and-wait, making
   `doc/js320_fwup.md`'s language-agnostic claim true.
4. README section stating the supported API subset and that Python is
   the primary binding.
5. Refresh examples for JS320 topics.

If none of this is funded within a release cycle or two, consider
deprecating the npm package instead of shipping an untested binding.
