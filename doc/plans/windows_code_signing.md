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
# Windows code signing

**Status**: in progress (stages 1-2 done; 3-4 written)
**Created**: 2026-10-08

## Context

Smart App Control switched itself from evaluation to enforcing on the
ci_win bench (`VerifiedAndReputablePolicyState` 1).  It then blocked the
unsigned `minibitty.exe` from a new build (CodeIntegrity events 3033,
3077, 3118; `WinError 4551`), and joulescope_driver job 3fb7b9a4 failed.
Every CI build is a new hash with no cloud reputation, so every build
fails on ci_win from now on.  Smart App Control has no per-file
exceptions.  Customer PCs with Smart App Control enforcing block the
same files.

The Joulescope UI already signs its Windows build with AzureSignTool and
an Azure Key Vault certificate (`pyjoulescope_ui/ci/windows_installer.py`
`azure_sign()`, `.github/workflows/packaging.yml`, `AZURE_*` secrets).
It signs every `.exe`, `.dll` and `.pyd`, because Smart App Control
checks every binary a process loads.

Unsigned Windows outputs of `.github/workflows/packaging.yml`:

| Output | Job | Consumers |
|---|---|---|
| `example/*.exe` (minibitty, jsdrv, ...) | `build_native_win` | CI benches, downloads |
| `jsdrv.dll` (`jsdrv_dll.zip`) | `build_native_win` shared=ON | C API users |
| `binding*.pyd` in the wheels | `build_python_wheels` | PyPI users, pyjoulescope |

## Decisions (2026-10-08)

1. **Sign only pushes to `main` and release tags.**  Pull requests and
   other branches build unsigned.  Smart App Control is off on the CI
   stations, so the benches still run unsigned dev builds.
2. **A shared GitHub composite action** in a new public repo,
   `jetperch/github_actions`, action `windows_sign/`.  The repo must be
   public because joulescope_driver and pyjoulescope_ui are public.  It
   holds no secrets: callers pass their own `AZURE_*` secrets.  Callers
   pin a tag (`@v1`) or a commit SHA.  pyjoulescope_ui switches from
   `azure_sign()` to the action in a later stage.

## Stages

1. **Shared action.**  Create `windows_sign/action.yml`: install
   AzureSignTool, then sign the files that match the `files` input in a
   single call (file list, `-mdop 4`, digicert timestamp, `-s` skips
   signed files).  Skip with a message when `AZURE_KEY_VAULT_URI` is
   unset.  Tag `v1`.
2. **Native Windows binaries.**  In `build_native_win`, when the push
   is to `main` or a `v*` tag, use the action to sign
   `cmake_build/example/Release/*.exe` and `jsdrv.dll` before the
   artifacts and the `jci-*` bench tools are uploaded.  Add the
   `AZURE_*` secrets to this repo (or the organization).  Verify that
   `Get-AuthenticodeSignature` shows Valid for a `main` build.
3. **Python wheels.**  After cibuildwheel, unpack each Windows wheel,
   sign `wheel_unpack/**/*.pyd` with the shared action, and repack
   (`wheel pack` regenerates `RECORD`).  This keeps the signing in the
   action, rather than in a cibuildwheel repair command.
   Verify the installed `.pyd` signature and an import on a machine with
   Smart App Control enforcing.
4. **pyjoulescope_ui** uses the action instead of `azure_sign()`.
5. **Other repos with Windows tools**, such as pyminibitty, if any ship
   executables.  To be listed after stage 3.

## Interim

Smart App Control is off on the CI stations (2026-10-08).  It cannot be
turned back on without a reset.

## Progress (2026-10-08)

* Stage 1 done: `jetperch/github_actions` (public) `v1.0.0` and `v1` at
  5de6765.  The self-test signs `hello.exe` on `windows-latest` and
  `windows-11-arm` with the organization `AZURE_*` secrets, and both
  verify Valid.
* Stage 2 done: run 37825328574 for 7ca6de7 signed and verified 6
  executables per static build (x64, arm64) and `jsdrv.dll`.  The
  `jci-windows-x86_64` `minibitty.exe` carries the JETPERCH LLC EV
  certificate (GlobalSign) with a DigiCert timestamp.  The CI bench
  suite passed on all four stations.  Smart App Control is off on the
  benches, so this does not yet show acceptance with it enforcing.
* `windows_sign` 1.1.0 (`v1` moved): recursive `<dir>/**/<name>`
  patterns, self-tested on both Windows runners.
* Stage 3 written: the `build_python_wheels` steps above.  A local
  unpack and repack of a Linux wheel kept its name and still imports.
* Stage 4 written: pyjoulescope_ui e2ab059 removes `azure_sign()` and
  signs with the action before (bundled `.exe`, `.dll`, `.pyd`) and after
  (installer) Inno Setup, on main and `v*` tags only.  It used to sign
  every push.
