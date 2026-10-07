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

"""Test the entry point --device option."""

import argparse
import contextlib
import io
import unittest
from pyjoulescope_driver import device_filter
from pyjoulescope_driver.__main__ import get_parser
from pyjoulescope_driver.entry_points.device_arg import add_brand_argument, add_device_argument, \
    device_select


PATHS = ['u/js220/000415', 'u/js320/8W2A', 'u/mb/93NP']


class FakeDriver:

    def find_one_device(self, specs=None, brand=None, timeout=None):
        return device_filter.find_one(PATHS, specs, brand)


class TestAddDeviceArgument(unittest.TestCase):

    def parse(self, args, **kwargs):
        p = argparse.ArgumentParser()
        add_device_argument(p, **kwargs)
        return p.parse_args(args).device

    def test_device(self):
        self.assertIsNone(self.parse([]))
        self.assertEqual('js320', self.parse(['--device', 'js320']))
        self.assertEqual('js320,8W2A', self.parse(['-d', 'js320,8W2A']))

    def test_aliases(self):
        self.assertEqual('8W2A', self.parse(['--serial_number', '8W2A'], aliases=['--serial_number']))
        p = argparse.ArgumentParser()
        add_device_argument(p, 'Extra help.', aliases=['--serial_number'])
        text = p.format_help()
        self.assertRegex(text, r'--device[^\n]*-d')
        self.assertIn('Extra help.', text)
        self.assertNotIn('--serial_number', text)


class TestAddBrandArgument(unittest.TestCase):

    def parse(self, args):
        p = argparse.ArgumentParser()
        add_brand_argument(p)
        return p.parse_args(args).brand

    def test_brand(self):
        self.assertIsNone(self.parse([]))
        for brand in ['Joulescope', 'joulescope', 'js', 'JS']:
            self.assertEqual('Joulescope', self.parse(['--brand', brand]), brand)

    def test_invalid(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit):
            self.parse(['--brand', 'acme'])
        self.assertIn('Unsupported brand "acme", expected one of: Joulescope, js', err.getvalue())


class TestDeviceSelect(unittest.TestCase):

    def select(self, specs, brand=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            device_path = device_select(FakeDriver(), specs, brand)
        return device_path, out.getvalue()

    def test_one(self):
        self.assertEqual(('u/js320/8W2A', ''), self.select('8w2a'))
        self.assertEqual(('u/js320/8W2A', ''), self.select('js320', brand='joulescope'))

    def test_multiple(self):
        device_path, out = self.select(None, brand='joulescope')
        self.assertIsNone(device_path)
        self.assertIn('Multiple Joulescopes found', out)
        self.assertIn('Use "--device"', out)

    def test_not_found(self):
        device_path, out = self.select('js110')
        self.assertIsNone(device_path)
        self.assertIn('Device "js110" not found', out)
        self.assertNotIn('Use "--device"', out)


_MEM_TEST_ARGS = ['s/flash/!cmd', '0', '0', '0x1000']
GENERIC = {'info': [], 'mem_test': _MEM_TEST_ARGS, 'metadata': [], 'program': [],
           'scan': [], 'set': [], 'threads': [], 'values': []}
JOULESCOPE = {'gpi': [], 'measure': [], 'record': [], 'statistics': []}


class TestEntryPoints(unittest.TestCase):

    def parse(self, args):
        with contextlib.redirect_stderr(io.StringIO()):
            return get_parser().parse_args(args)  # raises on conflicting options

    def test_device_option(self):
        for cmd, extra in {**GENERIC, **JOULESCOPE}.items():
            args = self.parse([cmd, '-d', 'js320'] + extra)
            self.assertEqual('js320', args.device, cmd)

    def test_generic_brand(self):
        for cmd, extra in GENERIC.items():
            self.assertIsNone(self.parse([cmd] + extra).brand, cmd)
            args = self.parse([cmd, '--brand', 'js'] + extra)
            self.assertEqual('Joulescope', args.brand, cmd)

    def test_joulescope_no_brand(self):
        for cmd, extra in JOULESCOPE.items():
            self.assertFalse(hasattr(self.parse([cmd] + extra), 'brand'), cmd)
            with self.assertRaises(SystemExit, msg=cmd):
                self.parse([cmd, '--brand', 'js'] + extra)

    def test_threads(self):
        args = self.parse(['threads', '-d', 'js220', '--duration', '2'])
        self.assertEqual(('js220', 2.0), (args.device, args.duration))

    def test_aliases(self):
        self.assertEqual('8W2A', self.parse(['program', '--device-path', '8W2A']).device)
        self.assertEqual('8W2A', self.parse(['record', '--serial_number', '8W2A']).device)
