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

# pyjoulescope_driver "metadata" entry point

Status: COMPLETE, HW-validated on JS220 + JS320 (2026-08-07)
Date: 2026-08-07

Note: a topic with no metadata appears as `null` in JSON/YAML and as
an empty row in HTML.  This tool exposed 18 such JS320 topics, which
turned out to be a host-side defect: meta_binary.c did not escape
control characters when rebuilding JSON, so multi-line detail strings
produced invalid JSON that binding.pyx silently converted to None.
Fixed 2026-08-08 (meta_binary.c escaping + binding.pyx warning).


## Goal

Add a `metadata` command to the pyjoulescope_driver CLI that fetches
all retained metadata for one device and renders it in a usable form:

    pyjoulescope_driver metadata [--device DEVICE_PATH]
        [--format {json,yaml,html}] [--out FILE]

The existing `info` command interleaves metadata and values as flat
text, which is hard to read and impossible to post-process.  This new
command produces machine-readable (JSON, YAML) and human-browsable
(HTML) output.


## Command-line arguments

- `--format {json,yaml,html}`: output format, case-insensitive.
  When omitted and `--out` is given, infer the format from the file
  extension: `.json` -> json, `.yaml`/`.yml` -> yaml,
  `.html`/`.htm` -> html (case-insensitive).  An explicit `--format`
  always wins, even when it conflicts with the extension.  With no
  `--out` or an unrecognized extension, default to `json`.
  - JSON and YAML: a mapping `{topic: {metadata}, ...}`.
  - HTML: a self-contained page with one table row per topic and one
    column per metadata key, with show/hide column controls.
- `--device DEVICE_PATH`: the target device.  Optional when exactly
  one device is connected (use it).  With zero devices, or with
  multiple devices and no `--device`, print an actionable error that
  lists the connected device paths, and return nonzero.
- `--out FILE`: write output to FILE instead of stdout.  Optional but
  recommended for HTML.
- `--open {defaults,restore}`: device open mode, default `restore`.
  Metadata is static, so this command should not reset device state;
  `restore` matches the non-invasive open used by `info` device
  listing.


## Metadata collection

Reuse the retained-metadata snapshot pattern already proven in
`entry_points/info.py`:

1. `driver.open(device_path, mode=args.open)`
2. `driver.subscribe(device_path, 'metadata_rsp_retain', fn)` then
   immediately `driver.unsubscribe(device_path, fn)`; the retained
   flush delivers every known topic synchronously.
3. Strip the trailing `$` from each topic and the `device_path/`
   prefix, producing `{subtopic: metadata_dict}`.

Keys in the output mapping are device-relative subtopics
(e.g. `s/i/range/select`), since the device path is a command input.

### Deduplication (CLAUDE.md rules 3 and 6)

`info.py` contains this same snapshot logic inline.  Extract a shared
helper into the new module and refactor `info.py` to use it:

    def metadata_load(driver, device_path) -> dict[str, dict]

Place it in `entry_points/metadata.py` and import from `info.py`
(`from .metadata import metadata_load`).  `info.py` keeps its own
`pub_retain` value snapshot, which `metadata` does not need.


## Output formats

Formatting is pure `dict -> str`, isolated from `Driver`, so it is
unit-testable without hardware:

    def to_json(meta: dict) -> str
    def to_yaml(meta: dict) -> str
    def to_html(meta: dict, title: str) -> str

### JSON

`json.dumps(meta, indent=2)`.  No new dependencies.

### YAML

Use PyYAML via optional import.  Do not add a hard dependency to
`setup.py`; if `yaml` is not importable, print
`YAML output requires pyyaml: pip install pyyaml` and return nonzero.

### HTML

Fully self-contained single page: inline CSS, inline vanilla JS, no
CDN or external resources (works offline, attachable to email/issue).

- Columns: the union of metadata keys across all topics, ordered
  `dtype, brief, detail, default, options, range, format, flags`,
  then any unknown keys alphabetically.  Rows: topics, sorted.
- Column show/hide: one checkbox per column in a header bar; JS
  toggles a `hidden-col` class on the matching `<td>/<th>` cells.
  Default: `detail` hidden, everything else shown.
- Render `options` as a readable list (`value: name (aliases)`) and
  `flags` as comma-joined text; other values via `str()`.
- HTML-escape all device-sourced strings (`html.escape`).
- Light styling: sticky header row, monospace topic column, zebra
  rows.  Keep it dependency-free and small.


## File changes

1. `pyjoulescope_driver/entry_points/metadata.py` (new): parser
   config, `metadata_load`, the three formatters, `on_cmd`.
2. `pyjoulescope_driver/entry_points/__init__.py`: register the new
   module in the import list and `__all__`.
3. `pyjoulescope_driver/entry_points/info.py`: replace the inline
   metadata snapshot with `metadata_load`.
4. `pyjoulescope_driver/test/test_metadata.py` (new): unit tests.

Four files exceeds the 3-file guideline, so implement in stages.


## Stages

### Stage 1: entry point with JSON + YAML

- Add `metadata.py` with `metadata_load`, `to_json`, `to_yaml`,
  device selection, and error handling; register in `__init__.py`.
- Tests: formatter output for a representative metadata fixture
  (dtype/options/flags/format keys), device-selection errors
  (0 and >1 devices) with a stubbed driver.

### Stage 2: HTML formatter

- Add `to_html` + tests: escaping of hostile strings, column union
  and ordering, checkbox/JS presence, no external URLs in output.

### Stage 3: refactor info.py

- Switch `info.py` to `metadata_load`; verify `info` output is
  unchanged on hardware (JS220 and JS320).

### Stage 4: hardware validation

- On a station with a JS220 and a JS320: run all three formats, both
  with and without `--device`, confirm JSON round-trips through
  `json.loads`, and open the HTML in a browser to check the
  show/hide controls.


## Future extensions (out of scope)

- Optional current-value column (from `pub_retain`) merged into the
  table.
- `--device '*'` multi-device output.
- C example (`example/jsdrv/`) equivalent, if ever needed outside
  Python.
