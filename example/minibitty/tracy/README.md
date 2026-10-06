<!--
# SPDX-FileCopyrightText: Copyright 2025-2026 Jetperch LLC
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



# Tracy Profiler

This subdirectory is a fork of the 
[Tracy Profiler](https://github.com/wolfpld/tracy).
As of 2026-07-21, [v0.13.1](https://github.com/wolfpld/tracy/tree/v0.13.1)
is the original source.  The minibitty example's adapter subcommand
forwards the adapter trace information to Tracy when provided with 
the "--tracy" argument.  It functions as a Tracy client.

The [tracy.diff](tracy.diff) file contains the differences from the stock
Tracy release to this forked source.  This fork removes the actual profiling
code since it only needs to forward the already valid trace data.
[adapter_tracy.cpp](../adapter_tracy.cpp) 
reimplements the network communication to allow for correct data population. 


You can download the precompiled Windows binaries for 
[v0.13.1](https://github.com/wolfpld/tracy/releases/tag/v0.13.1).
