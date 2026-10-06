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

# Plan: `joulescope-data` skill — capture, analyze, compare JLS traces

## Context

Matt wants a Claude Code skill covering the pyjoulescope_driver + pyjls workflow:
capture data from a Joulescope, analyze captured traces, and compare a trace
against a previous one. Decisions already made:

- Skill lives in the **joulescope_driver repo** so it versions with the driver.
- Audience is **general users**: assume only `pip install pyjoulescope_driver
  pyjls` and a connected JS110/JS220/JS320 — no Jetperch-internal HIL tooling.
- Comparison supports all three modes: summary-stat deltas, threshold
  pass/fail (CI-style, exit codes), and waveform-level diff with divergence
  localization.
- The analysis must handle **repeated waveforms**: detect the period, extract
  cycles, and compare cycle-aligned. Validation target: the JS320 (8W2A) is
  wired to the JS220 EVK1, which free-runs a ~0.5 Hz wiggle (~0.1 µA ↔
  ~104 mA, so ~2 s period spanning ~6 current decades).
- Analysis helpers become **real `python -m pyjoulescope_driver` subcommands**
  (`summarize`, `compare`) so pip users get them too (user-approved).

Precedent: `pyjoulescope_ui/.claude/skills/ui-remote/SKILL.md` — minimal
frontmatter (`name`, `description` only), helper code in the repo not the
skill dir, ~80-char lines. `entry_points/__init__.py` registry: a module with
`parser_config(p)` returning the command callable, listed in `__all__`
(`__main__.py` derives the command name from the module name).

## Files (7 total → staged per CLAUDE.md rule 2)

### Stage 1 — analysis core + tests (2 files, no CLI yet)

1. **`joulescope_driver/pyjoulescope_driver/jls_analysis.py`** (new)
   Pure pyjls + numpy + stdlib; no Driver import; pyjls imported lazily with
   the same remedy message pattern as `record.py:141` (`pip3 install -U pyjls`).
   Contents:
   - Reader helpers: open, data-signal enumeration excluding the global
     annotation signal (`sample_rate == 0` / signal 0), name-based lookup
     (full `source.signal` required when ambiguous across multi-source files).
   - `summarize(path)` → per-signal name/units/sample_rate/length/duration +
     exact whole-signal stats via `fsr_statistics(sid, 0, length, 1)[0]`
     (MEAN/STD/MIN/MAX), RMS = `sqrt(mean² + std²)`, charge = i_mean·t,
     energy = p_mean·t (or chunked `fsr()` i·v integration if no power signal).
   - `compare(ref, test, ...)` — three composable modes:
     a. default: per-signal ref/test/Δ/%Δ for mean/std/min/max/RMS + charge,
        energy. %Δ guarded → `n/a` when `|ref| < atol`.
     b. thresholds: JSON spec `{"current": {"mean": {"rtol":, "atol":}}, ...}`
        or blanket `--rtol`; violations listed; pass/fail result.
     c. windowed diff: per-window mean/std via repeated `length==1`
        `fsr_statistics` calls (exact; cap ~10k windows, warn + enlarge if
        exceeded); report max-|Δmean| window, first out-of-tolerance window
        (divergence locator), violation count.
   - Alignment: default sample-0↔sample-0 truncated to common duration;
     `--offset SECONDS` explicit; `--align auto` cross-correlates coarse
     per-window means of the current signal (re-captures are noisy, so
     correlation beats verify.py's exact-match probe search).
   - **Periodic-waveform support** (`--period auto|SECONDS`):
     - Period detection: autocorrelation of the per-window mean series
       (windowed means, not raw samples — EVK1-class signals span 6 decades,
       so correlate on log-scaled means to keep the µA half of the cycle from
       vanishing next to the 100 mA half); report detected period + cycle
       count.
     - Cycle segmentation: split each capture into whole cycles (discard
       partial head/tail); per-cycle stats (mean/min/max/charge) and a
       cycle-averaged waveform (fixed number of phase bins per cycle).
     - `summarize --period auto`: adds period, cycle count, per-cycle stat
       spread (cycle-to-cycle repeatability).
     - `compare --period auto`: aligns on cycle phase (correlation offset
       reduced mod period, choosing the smallest |offset| — plain
       cross-correlation is ambiguous on periodic signals), then compares
       cycle-averaged waveforms bin-by-bin plus per-cycle stat distributions.
       Composes with thresholds (mode b) for pass/fail.
   - `summarize_cli(args)` / `compare_cli(args)` taking an argparse Namespace
     and returning exit codes — so shims hold zero logic and tests can drive
     the CLI with `SimpleNamespace`. `--json [PATH|-]` on both.
   - Exit codes: summarize 0 ok / 1 error; compare 0 pass, 1 threshold fail,
     2 usage/file error.

2. **`joulescope_driver/pyjoulescope_driver/test/test_jls_analysis.py`** (new)
   `unittest` + `skipIf` pyjls missing (match `test_record.py` style).
   Fixtures written to tempdir with `pyjls.Writer` (recipe from
   `pyjoulescope_ui/ci/uitest/jls_fixtures.py`: source_def + FSR/F32
   signal_def + `fsr_f32`, a `utc()` pair). Cases: ramp/sine stats vs numpy
   ground truth; RMS; charge/energy on constants; identical files → zero
   deltas exit 0; violated thresholds → exit 1 + failure list; shifted copy +
   `--align auto` recovers known offset; injected step localized by windowed
   diff; annotation-signal exclusion; %Δ guard near zero. Periodic cases:
   synthetic EVK1-like wiggle (two-level cycle with large dynamic range) →
   period recovered by `--period auto`; two fixtures offset by a non-integral
   number of cycles → cycle-phase alignment recovers the phase; one distorted
   cycle → per-cycle spread flags it; cycle-averaged compare of identical
   periodic fixtures → zero deltas.

### Stage 2 — CLI wiring (3 files)

3. **`entry_points/summarize.py`** (new) — `parser_config(p)` defines args,
   returns `jls_analysis.summarize_cli`.
4. **`entry_points/compare.py`** (new) — same shape for compare.
5. **`entry_points/__init__.py`** (edit) — add both modules to the import and
   `__all__`.
   Smoke: `python -m pyjoulescope_driver summarize --help` / `compare --help`.

### Stage 3 — the skill (2 files)

6. **`joulescope_driver/.claude/skills/joulescope-data/SKILL.md`** (new)
   Frontmatter: `name: joulescope-data`; folded `description:` triggering on
   capture/record current/voltage/power/energy with a Joulescope, analyze or
   summarize a .jls file, compare captures, regression vs baseline, locate
   trace divergence; explicitly NOT for the desktop UI (that's ui-remote).
   Sections (ui-remote structure, ≤100-char lines, goal 80):
   1. Orientation — `pip install pyjoulescope_driver pyjls` (**pyjls is not a
      declared dependency; record/summarize/compare all need it**).
   2. Capture decision tree — `scan` (empty → cable/udev rules
      `99-joulescope.rules`); no `--serial_number` records ALL devices into
      one multi-source file; template
      `record --duration 10s --signals current,voltage,power --note "..."
      out.jls`; **JS320 needs `--set s/i/range/mode=auto`** (defaults branch
      covers only js110/js220); quick numbers via `statistics --duration 2`;
      `measure` rejects JS320; `info '*' -v` topic dump.
   3. Analyze — `pyjls info -v`, `summarize FILE [--json -]`,
      `pyjls export|csv|extract` for slicing.
   4. Compare — the three modes, thresholds JSON format, exit codes for CI,
      `--window`/`--align auto` and reading divergence output; repeated
      waveforms: `--period auto` to detect the period, extract cycles, and
      compare cycle-aligned (use for periodic loads, where plain
      cross-correlation alignment is ambiguous).
   5. CLI cheat sheet table incl. exit codes.
   6. Python-when-CLI-can't — short Reader snippet (`signal_lookup`,
      `fsr_statistics(sid, 0, length, 1)[0]`, `fsr`, time64 epoch 2018-01-01,
      SECOND=2^30) + programmatic `Record`.
   7. Vocabulary — record signal names (`i,v,p,r,0-3,T`), signal 0 = global
      annotation signal, never analyze it.
   8. Pitfalls — no `capture` subcommand (it's `record`; stale example
      headers say otherwise); pyjls not auto-installed; multi-device default;
      `fsr_statistics` internal boundaries approximate unless length==1;
      JS110 2 MHz vs 1 MHz default rates in cross-device compares; %Δ near
      zero → use atol; prefer `fsr_statistics` over `fsr` on huge files.
   9. Reference map — `jls_analysis.py`, `entry_points/record.py`,
      `pyjoulescope_driver/record.py`, pyjls repo.
7. **`joulescope_driver/CHANGELOG.md`** (edit) — note the two new commands.

### Stage 4 — hardware validation (JS320 8W2A + JS220 EVK1, no file changes)

The JS320 is wired to the JS220 EVK1, which free-runs its ~0.5 Hz wiggle
(~0.1 µA ↔ ~104 mA) — a natural repeated-waveform source.

- `python -m pyjoulescope_driver scan` (expect u/js220/002557 + u/js320/8W2A;
  target the JS320 with `--serial_number 8W2A` since both are connected).
- `record --duration 30s --serial_number 8W2A --set s/i/range/mode=auto
  evk1_a.jls` (~15 EVK1 cycles; auto-range required to track the 6-decade
  swing); once briefly without the `--set` to confirm the SKILL.md caveat.
- `summarize evk1_a.jls --period auto`: detected period ≈ 2 s, cycle count
  ≈ 15, per-cycle spread small; sanity-check mean/min/max against the known
  ~0.1 µA / ~104 mA levels.
- Second 30 s capture `evk1_b.jls` → `compare` in all three modes plus
  `--period auto` cycle-aligned; loose thresholds pass (exit 0);
  deliberately-too-tight thresholds exit 1; verify the reported phase
  alignment is stable across reruns.
- Divergence localization check: capture with a manual range change or a
  shorter duration mid-set, confirm the windowed diff points at the right
  region.
- Per memory: JS320 personality backup is only needed for flash writes —
  streaming capture is non-destructive, no backup step required. If a capture
  is interrupted uncleanly and the JS320 wedges, recover via JS220
  power_cycle (reference_hil_commands).

## Reuse / no-duplication notes

- `pyjoulescope_ui/ci/uitest/verify.py` overlaps conceptually
  (summarize/compare_subrange) but lives in a different repo and is
  UI-test-scoped — not importable here. `jls_analysis.py` becomes the new
  canonical home (broader scope: windows, 3 modes, JSON/CI output); reuse its
  proven ideas (signal-0 exclusion, name lookup), no verbatim copying. Add a
  follow-up note that pyjoulescope_ui `doc/plans/` should get a migration
  plan onto `pyjoulescope_driver.jls_analysis` once released.
- No setup.py source-manifest edits needed: the 3-parallel-manifest rule
  covers C sources; these are pure-Python package files picked up by
  find_packages. pyjls stays out of `install_requires` (matches `Record`'s
  existing lazy-import approach).

## Verification

1. Stage 1: `python -m unittest pyjoulescope_driver.test.test_jls_analysis`
   green (and run the existing suite to confirm nothing else breaks).
2. Stage 2: `--help` smoke for both commands; `summarize`/`compare` against a
   fixture file from the tests.
3. Stage 3: skill loads (start a session in joulescope_driver/, confirm the
   skill is listed); markdown line-length check.
4. Stage 4: full HW pass on the JS320 as listed above.
