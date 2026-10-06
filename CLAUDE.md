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

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code)
when working with code in this repository.


## General Guidance

1. When planning new features, examine the architecture and
   work within the existing framework.  Write features once:
   reuse existing code first, and refactor as needed.
2. For large or risky changes, propose a staged plan first. Each
   stage must build, pass tests, and be reviewable on its own.
3. Never duplicate code to complete a new feature without approval.
4. Write unit tests for all new code, and fix any test failures your
   change causes.  If you find untested code or unrelated failing
   tests, add a plan to doc/plans/ and continue.  You are responsible
   for maintaining a clean code base.
5. If you find duplicate code, add a deduplication plan to
   doc/plans/ and continue.
6. For Markdown files (md), keep the line length to 100 characters,
   maximum, with a goal of 80 characters. Do not wrap early.
7. When working on a feature, read the relevant documentation
   file(s) in doc/ first.
