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

"""Test the info entry point value formatting."""

import unittest
from pyjoulescope_driver.entry_points.info import format_value, version_to_str


class TestInfo(unittest.TestCase):

    def test_version_to_str(self):
        self.assertEqual('1.2.3', version_to_str(0x01020003))
        self.assertEqual('1.2.3', version_to_str('1.2.3'))

    def test_format_version(self):
        meta = {'dtype': 'u32', 'format': 'version'}
        self.assertEqual('1.1.10', format_value(meta, 0x0101000a))

    def test_format_without_version(self):
        # The JS320 c/hw/version is a u8 without the version format.
        self.assertEqual(1, format_value({'dtype': 'u8'}, 1))
        self.assertEqual(1, format_value(None, 1))
