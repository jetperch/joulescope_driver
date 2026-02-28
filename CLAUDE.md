<!--
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
-->

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code)
when working with code in this repository.


## General Guidance

1. When planning new features, examine the architecture and
   try to work within the existing framework.  Write features
   once.  Always try to use existing code first.  Plan to
   refactor as needed.
2. If a task requires changes to more than 3 files, pause and
   break it into smaller, incremental tasks first.
3. NEVER duplicate code to complete a new feature without approval.
4. Write unit tests for all new code.  If you happen to discover
   untested code during maintenance or development of a different
   feature, pause and create a plan to implement tests in doc/plans/.
5. If a unit test fails, always stop and fix it, even if it is
   unrelated to the changes you just made.  You are responsible
   for creating and maintaining a clean code base.
6. If you come across duplicate code, pause and add a plan
   file to deduplicate it to doc/plans/.
7. For Markdown files (md), keep the line length to 100 characters,
   maximum, with a goal of 80 characters. Do not wrap early.
8. When working on a feature, read the relevant documentation
   file(s) in doc/ first.
