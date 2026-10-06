# Copyright 2026 Jetperch LLC
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

"""Test the Joulescope device filter."""

import unittest
from pyjoulescope_driver.device_filter import device_model, is_joulescope, \
    device_match, device_filter


PATHS = [
    'u/mb/1',
    'u/js220/000415',
    'u/js320/1',
    'u/js320/12',
    'u/js110/001234',
    'u/js320/31NB',
]


class TestDeviceFilter(unittest.TestCase):

    def test_model(self):
        self.assertEqual('js320', device_model('u/js320/31NB'))
        self.assertEqual('mb', device_model('u/mb/1'))
        self.assertIsNone(device_model('u/js320'))
        self.assertIsNone(device_model(''))

    def test_is_joulescope(self):
        self.assertTrue(is_joulescope('u/js110/1'))
        self.assertTrue(is_joulescope('u/JS220/1'))
        self.assertTrue(is_joulescope('u/js320/1'))
        self.assertFalse(is_joulescope('u/mb/1'))
        self.assertFalse(is_joulescope('u/js320'))

    def test_match(self):
        p = 'u/js320/31NB'
        for spec in ['u/js320/31NB', 'U/JS320/31nb', 'js320/31NB', 'js320', '31NB', '31nb']:
            self.assertTrue(device_match(p, spec), spec)
        for spec in [None, '', 'u', 'u/js320', '31', 'NB', 'js220', 'js320/31', 'u/js320/31']:
            self.assertFalse(device_match(p, spec), spec)

    def test_filter_all_ignores_non_joulescope(self):
        expect = PATHS[1:]
        self.assertEqual(expect, device_filter(PATHS))
        self.assertEqual(expect, device_filter(PATHS, None))
        self.assertEqual(expect, device_filter(PATHS, []))

    def test_filter_serial_number_exact(self):
        self.assertEqual(['u/js320/1'], device_filter(PATHS, '1'))
        self.assertEqual(['u/js320/12'], device_filter(PATHS, '12'))
        self.assertEqual([], device_filter(PATHS, '2'))

    def test_filter_model(self):
        self.assertEqual(['u/js320/1', 'u/js320/12', 'u/js320/31NB'], device_filter(PATHS, 'js320'))
        self.assertEqual([], device_filter(PATHS, 'mb'))

    def test_filter_list(self):
        self.assertEqual(['u/js220/000415', 'u/js320/31NB'],
                         device_filter(PATHS, ['31NB', 'u/js220/000415']))
