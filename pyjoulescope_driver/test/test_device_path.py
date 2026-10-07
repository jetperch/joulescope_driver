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

"""Test the device path parsing and matching."""

import pickle
import unittest
from pyjoulescope_driver import DevicePath
from pyjoulescope_driver.device_path import BRANDS_TO_MODELS, BRAND_ALIASES, brand_validate


class TestDevicePath(unittest.TestCase):

    def test_is_str(self):
        p = DevicePath('u/js320/31NB')
        self.assertIsInstance(p, str)
        self.assertEqual('u/js320/31NB', p)
        self.assertEqual(hash('u/js320/31NB'), hash(p))
        self.assertEqual({'u/js320/31NB': 1}[p], 1)
        self.assertEqual("'u/js320/31NB'", repr(p))
        self.assertEqual('u/js320/31NB/s/i', f'{p}/s/i')
        self.assertEqual('u/js320/31NB/s/i', p + '/s/i')

    def test_pickle(self):
        p = pickle.loads(pickle.dumps(DevicePath('u/js320/31NB')))
        self.assertIsInstance(p, DevicePath)
        self.assertEqual('js320', p.model)

    def test_fields(self):
        p = DevicePath('u/JS320/Y9s4')
        self.assertEqual('u', p.backend)
        self.assertEqual('js320', p.model)
        self.assertEqual('Y9s4', p.serial_number)
        self.assertEqual('Joulescope', p.brand)

    def test_non_joulescope(self):
        p = DevicePath('u/mb/1')
        self.assertEqual('mb', p.model)
        self.assertEqual('1', p.serial_number)
        self.assertIsNone(p.brand)

    def test_malformed(self):
        for s in ['', 'u', 'u/js320', 'u/js320/1/s']:
            p = DevicePath(s)
            self.assertIsNone(p.backend, s)
            self.assertIsNone(p.model, s)
            self.assertIsNone(p.serial_number, s)
            self.assertIsNone(p.brand, s)
            self.assertFalse(p.match('js320'), s)

    def test_brand(self):
        for s in ['u/js110/1', 'u/JS220/1', 'u/js320/1']:
            self.assertEqual('Joulescope', DevicePath(s).brand, s)
        for s in ['u/mb/1', 'u/js999/1']:
            self.assertIsNone(DevicePath(s).brand, s)

    def test_bootloader(self):
        p = DevicePath('u/&js220/000415')
        self.assertTrue(p.is_bootloader)
        self.assertEqual('u', p.backend)
        self.assertEqual('js220', p.model)
        self.assertEqual('000415', p.serial_number)
        self.assertEqual('Joulescope', p.brand)
        self.assertEqual('Joulescope', DevicePath('u/&JS110/1').brand)
        for s in ['u/js220/000415', 'u/mb/1', 'u/js320', '']:
            self.assertFalse(DevicePath(s).is_bootloader, s)

    def test_bootloader_match(self):
        p = DevicePath('u/&js220/000415')
        # Model specifications match the device in either mode.
        for spec in ['u/&js220/000415', 'js220', 'u/js220', 'js220/000415',
                     'js220-000415', '000415', 'JS220']:
            self.assertTrue(p.match(spec), spec)
        # The "&" forms select bootloader devices.
        for spec in ['&js220', 'u/&js220', 'u/&js220/', '&js220/000415', '&js220-000415']:
            self.assertTrue(p.match(spec), spec)
            self.assertFalse(DevicePath('u/js220/000415').match(spec), spec)
        for spec in ['u/js220/000415', 'js320', '&js110']:
            self.assertFalse(p.match(spec), spec)

    def test_brand_tables(self):
        brands = [brand.lower() for brand in BRANDS_TO_MODELS]
        self.assertEqual(len(brands), len(set(brands)))
        models = [model for models in BRANDS_TO_MODELS.values() for model in models]
        self.assertEqual(len(models), len(set(models)), 'model in multiple brands')
        aliases = [alias.lower() for alias in BRAND_ALIASES]
        self.assertEqual(len(aliases), len(set(aliases)))
        for alias, brand in BRAND_ALIASES.items():
            self.assertNotIn(alias.lower(), brands, alias)
            self.assertIn(brand, BRANDS_TO_MODELS, alias)

    def test_brand_validate(self):
        for brand in ['Joulescope', 'joulescope', 'JOULESCOPE', 'js', 'JS']:
            self.assertEqual('Joulescope', brand_validate(brand), brand)
        with self.assertRaises(ValueError):
            brand_validate('acme')
        with self.assertRaises(ValueError):
            brand_validate('')
        with self.assertRaises(TypeError):
            brand_validate(None)

    def test_match(self):
        p = DevicePath('u/js320/31NB')
        for spec in ['u/js320/31NB', 'U/JS320/31nb', '/u/js320/31NB/', 'js320/31NB',
                     'js320', '31NB', '31nb', 'u/js320', 'u/js320/', 'U/JS320/']:
            self.assertTrue(p.match(spec), spec)
        for spec in [None, '', 'u', 'u/', '31', 'NB', 'js220', 'js320/31', 'u/js320/31',
                     'u/js220', 'x/js320', 'u/js32']:
            self.assertFalse(p.match(spec), spec)

    def test_match_model_dash_serial_number(self):
        p = DevicePath('u/js320/Y9S4')
        for spec in ['js320-Y9S4', 'jS320-Y9s4', 'JS320-y9s4']:
            self.assertTrue(p.match(spec), spec)
        for spec in ['js320-Y9', 'js320-9S4', 'js220-Y9S4', 'js320-', '-Y9S4', 'u/js320-Y9S4']:
            self.assertFalse(p.match(spec), spec)
