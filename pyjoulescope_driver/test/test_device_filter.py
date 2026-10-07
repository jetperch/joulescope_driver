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

"""Test the device filter."""

import unittest
from pyjoulescope_driver import DevicePath, device_filter
from pyjoulescope_driver.device_filter import DeviceFilterError


PATHS = [
    'u/mb/1',
    'u/js220/000415',
    'u/js320/1',
    'u/js320/12',
    'u/js110/001234',
    'u/js320/31NB',
]
JS = PATHS[1:]  # the Joulescope paths


class TestDeviceFilter(unittest.TestCase):

    def test_find_returns_device_path(self):
        paths = device_filter.find(PATHS)
        for p in paths:
            self.assertIsInstance(p, DevicePath)
        self.assertEqual(['1', '000415', '1', '12', '001234', '31NB'],
                         [p.serial_number for p in paths])
        self.assertIsInstance(device_filter.find_one(PATHS, '31NB'), DevicePath)

    def test_find_accepts_device_path(self):
        paths = [DevicePath(p) for p in PATHS]
        self.assertEqual(['u/js320/31NB'], device_filter.find(paths, '31NB'))

    def test_find_all(self):
        for specs in [None, [], (), '', ' , ']:
            self.assertEqual(PATHS, device_filter.find(PATHS, specs), specs)
        self.assertEqual([], device_filter.find([]))

    def test_find_specs_any_brand(self):
        self.assertEqual(['u/mb/1'], device_filter.find(PATHS, 'mb'))
        self.assertEqual(['u/mb/1', 'u/js320/1'], device_filter.find(PATHS, '1'))

    def test_find_brand(self):
        for brand in ['Joulescope', 'joulescope', 'JOULESCOPE', 'js', 'JS']:
            self.assertEqual(JS, device_filter.find(PATHS, brand=brand), brand)
        self.assertEqual(['u/js320/1'], device_filter.find(PATHS, '1', brand='joulescope'))
        self.assertEqual([], device_filter.find(PATHS, 'mb', brand='joulescope'))
        self.assertEqual(JS, device_filter.find(PATHS, '', brand='joulescope'))

    def test_find_bootloader(self):
        paths = ['u/js220/000415', 'u/&js220/000416', 'u/&js110/1', 'u/mb/1']
        self.assertEqual(paths[:3], device_filter.find(paths, brand='joulescope'))
        self.assertEqual(paths[:2], device_filter.find(paths, 'js220'))
        self.assertEqual(['u/&js220/000416'], device_filter.find(paths, '&js220'))
        self.assertEqual('u/&js220/000416', device_filter.find_one(paths, '000416'))

    def test_find_brand_invalid(self):
        with self.assertRaises(ValueError) as cm:
            device_filter.find(PATHS, brand='acme')
        self.assertEqual('Unsupported brand "acme", expected one of: Joulescope, js',
                         str(cm.exception))
        with self.assertRaises(TypeError):
            device_filter.find(PATHS, brand=1)

    def test_find_specs_invalid(self):
        for specs in [2.0, 1, b'js320', {'js320'}]:
            with self.assertRaises(TypeError, msg=repr(specs)) as cm:
                device_filter.find(PATHS, specs)
            self.assertIn('specs must be str or a list of str', str(cm.exception))
        with self.assertRaises(TypeError):
            device_filter.find(PATHS, ['js320', 1])

    def test_filter_serial_number_exact(self):
        self.assertEqual(['u/js320/12'], device_filter.find(PATHS, '12'))
        self.assertEqual([], device_filter.find(PATHS, '2'))

    def test_filter_model(self):
        self.assertEqual(['u/js320/1', 'u/js320/12', 'u/js320/31NB'],
                         device_filter.find(PATHS, 'js320'))

    def test_filter_backend_model(self):
        # "u/js320" and "u/js320/" select the same devices
        expect = ['u/js320/1', 'u/js320/12', 'u/js320/31NB']
        for spec in ['u/js320', 'u/js320/', 'U/JS320/', ' u/js320/ ', '/u/js320/']:
            self.assertEqual(expect, device_filter.find(PATHS, spec), spec)
        self.assertEqual(device_filter.find(PATHS, 'u/js320'),
                         device_filter.find(PATHS, 'u/js320/'))
        self.assertEqual(['u/mb/1', *expect], device_filter.find(PATHS, 'u/mb/,u/js320'))
        self.assertEqual([], device_filter.find(PATHS, 'x/js320/'))
        self.assertEqual('u/js220/000415', device_filter.find_one(PATHS, 'u/js220'))
        self.assertEqual('u/js220/000415', device_filter.find_one(PATHS, 'u/js220/'))

    def test_filter_list(self):
        self.assertEqual(['u/js220/000415', 'u/js320/31NB'],
                         device_filter.find(PATHS, ['31NB', 'u/js220/000415']))
        self.assertEqual(['u/js220/000415', 'u/js320/31NB'],
                         device_filter.find(PATHS, ('31NB', 'u/js220/000415')))

    def test_filter_comma_separated(self):
        expect = ['u/js220/000415', 'u/js320/31NB']
        self.assertEqual(expect, device_filter.find(PATHS, '31NB,u/js220/000415'))
        self.assertEqual(expect, device_filter.find(PATHS, ' 31NB , u/js220/000415 '))
        self.assertEqual(['u/js320/1', 'u/js320/12'], device_filter.find(PATHS, 'js320-1,12'))
        self.assertEqual(['u/js320/1', 'u/js320/12', 'u/js320/31NB'],
                         device_filter.find(PATHS, 'js320,31NB'))
        self.assertEqual([], device_filter.find(PATHS, '2,js999'))

    def test_filter_model_dash_serial_number(self):
        self.assertEqual(['u/js320/1'], device_filter.find(PATHS, 'JS320-1'))
        self.assertEqual(['u/js220/000415', 'u/js320/31NB'],
                         device_filter.find(PATHS, 'js320-31nb, Js220-000415'))

    def test_filter_comma_separated_empty(self):
        self.assertEqual(['u/js320/12'], device_filter.find(PATHS, '12,'))
        self.assertEqual(['u/js320/12'], device_filter.find(PATHS, ',12,,'))

    def test_find_one(self):
        self.assertEqual('u/js320/31NB', device_filter.find_one(PATHS, 'js320-31nb'))
        self.assertEqual('u/mb/1', device_filter.find_one(PATHS, 'mb'))
        self.assertEqual('u/js220/000415', device_filter.find_one(PATHS, ['js220']))
        self.assertEqual('u/js320/1', device_filter.find_one(PATHS, '1', brand='joulescope'))
        self.assertEqual('u/js320/1', device_filter.find_one(['u/mb/1', 'u/js320/1'],
                                                             brand='Joulescope'))

    def test_find_one_not_found(self):
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one(PATHS, '2, mb', brand='joulescope')
        e = cm.exception
        self.assertIsInstance(e, ValueError)
        self.assertEqual(['2', 'mb'], e.specs)
        self.assertEqual('Joulescope', e.brand)
        self.assertEqual([], e.matches)
        self.assertEqual(JS, e.available)
        self.assertEqual(f'Device "2, mb" not found in {JS}', str(e))

    def test_find_one_none_available(self):
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one(['u/mb/1'], brand='joulescope')
        e = cm.exception
        self.assertIsNone(e.specs)
        self.assertEqual([], e.matches)
        self.assertEqual([], e.available)
        self.assertEqual('No Joulescope found', str(e))
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one([])
        self.assertIsNone(cm.exception.brand)
        self.assertEqual('No device found', str(cm.exception))

    def test_find_one_multiple(self):
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one(PATHS, '1')
        e = cm.exception
        self.assertEqual(['1'], e.specs)
        self.assertIsNone(e.brand)
        self.assertEqual(['u/mb/1', 'u/js320/1'], e.matches)
        self.assertEqual(PATHS, e.available)
        self.assertEqual(f'Device "1" matched multiple devices: {e.matches}', str(e))

        expect = ['u/js320/1', 'u/js320/12', 'u/js320/31NB']
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one(PATHS, 'js320', brand='joulescope')
        self.assertEqual(f'Device "js320" matched multiple Joulescopes: {expect}',
                         str(cm.exception))

    def test_find_one_multiple_without_specs(self):
        for specs in [None, '', []]:
            with self.assertRaises(DeviceFilterError) as cm:
                device_filter.find_one(PATHS, specs)
            e = cm.exception
            self.assertIsNone(e.specs)
            self.assertEqual(PATHS, e.matches)
            self.assertEqual(f'Multiple devices found: {PATHS}', str(e))
        with self.assertRaises(DeviceFilterError) as cm:
            device_filter.find_one(PATHS, brand='joulescope')
        self.assertEqual(f'Multiple Joulescopes found: {JS}', str(cm.exception))
