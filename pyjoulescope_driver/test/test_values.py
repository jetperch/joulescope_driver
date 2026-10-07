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

"""Test the values entry point."""

import argparse
import contextlib
import io
import unittest
from unittest import mock
from pyjoulescope_driver import device_filter
from pyjoulescope_driver.entry_points import values


PATHS = ['u/js320/8W2A', 'u/mb/93NP']
META = {'c/fw/version': {'dtype': 'u32', 'format': 'version'}}
VALUES = {'c/fw/version': 0x01010009, 's/i/range/mode': 'auto'}


class _Subscription:

    def unsubscribe(self):
        pass


class FakeDriver:
    """Emulate the retained metadata and value flush on subscribe."""

    def __init__(self):
        self.calls = []
        self.log_level = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def find_one_device(self, specs=None, brand=None, timeout=None):
        return device_filter.find_one(PATHS, specs, brand)

    @contextlib.contextmanager
    def open(self, device_path, mode=None, timeout=None):
        self.calls.append(('open', device_path, mode))
        yield
        self.calls.append(('close', device_path))

    def subscribe(self, device_path, flags, fn, timeout=None):
        self.calls.append(('subscribe', device_path, flags))
        if flags == 'metadata_rsp_retain':
            for subtopic, value in META.items():
                fn(f'{device_path}/{subtopic}$', value)
        elif flags == 'pub_retain':
            for subtopic, value in VALUES.items():
                fn(f'{device_path}/{subtopic}', value)
        return _Subscription()

    def unsubscribe(self, device_path, fn, timeout=None):
        pass


class TestValues(unittest.TestCase):

    def run_values(self, device=None, brand=None):
        args = argparse.Namespace(device=device, brand=brand, jsdrv_log_level='off')
        driver = FakeDriver()
        out = io.StringIO()
        with mock.patch.object(values, 'Driver', lambda: driver), contextlib.redirect_stdout(out):
            rc = values.on_cmd(args)
        return rc, out.getvalue(), driver.calls

    def test_values(self):
        rc, out, calls = self.run_values('js320')
        self.assertEqual(0, rc)
        self.assertEqual('u/js320/8W2A values:\n'
                         '  c/fw/version = 1.1.9\n'
                         '  s/i/range/mode = auto\n', out)
        self.assertEqual(('open', 'u/js320/8W2A', 'restore'), calls[0])
        self.assertEqual(('close', 'u/js320/8W2A'), calls[-1])

    def test_brand(self):
        rc, out, _ = self.run_values(brand='js')
        self.assertEqual(0, rc)
        self.assertTrue(out.startswith('u/js320/8W2A values:'))

    def test_multiple(self):
        rc, out, calls = self.run_values()
        self.assertEqual(1, rc)
        self.assertIn('Multiple devices found', out)
        self.assertEqual([], calls)

    def test_not_found(self):
        rc, out, calls = self.run_values('js110')
        self.assertEqual(1, rc)
        self.assertIn('Device "js110" not found', out)
        self.assertEqual([], calls)
