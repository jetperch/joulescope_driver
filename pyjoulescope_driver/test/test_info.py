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

"""Test the info entry point."""

import argparse
import contextlib
import io
import unittest
from unittest import mock
from pyjoulescope_driver import device_filter
from pyjoulescope_driver.entry_points import info
from pyjoulescope_driver.entry_points.info import format_value, version_to_str


PATHS = ['u/js320/8W2A', 'u/mb/93NP']


class _Opened(Exception):
    pass


class FakeDriver:
    paths = PATHS

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def find_devices(self, specs=None, brand=None, timeout=None):
        return device_filter.find(self.paths, specs, brand)

    def open(self, device_path, mode=None, timeout=None):
        raise _Opened(device_path)


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


class TestSysInfo(unittest.TestCase):

    def test_sys_info(self):
        txt = info._sys_info()
        self.assertIn('SYSTEM INFORMATION', txt)
        self.assertIn('pyjoulescope_driver', txt)

    def test_cpu_freq_unavailable(self):
        # psutil.cpu_freq() returns None without CPU frequency data, such
        # as on many ARM Linux VMs.
        with mock.patch.object(info.psutil, 'cpu_freq', return_value=None):
            txt = info._sys_info()
        self.assertIn('CPU frequency        unavailable', txt)

    def test_cpu_count_unavailable(self):
        with mock.patch.object(info.psutil, 'cpu_count', return_value=None):
            txt = info._sys_info()
        self.assertIn('CPU cores            unavailable', txt)


class TestInfoDevices(unittest.TestCase):

    def run_info(self, device=None, brand=None):
        args = argparse.Namespace(device=device, brand=brand, jsdrv_log_level='off')
        out = io.StringIO()
        with mock.patch.object(info, 'Driver', FakeDriver), contextlib.redirect_stdout(out):
            rc = info.on_cmd(args)
        return rc, out.getvalue()

    def test_list_all(self):
        rc, out = self.run_info()
        self.assertEqual(0, rc)
        self.assertIn('u/js320/8W2A', out)
        self.assertIn('u/mb/93NP', out)

    def test_list_brand(self):
        rc, out = self.run_info(brand='Joulescope')
        self.assertEqual(0, rc)
        self.assertIn('u/js320/8W2A', out)
        self.assertNotIn('u/mb/93NP', out)

    def test_list_brand_none_found(self):
        with mock.patch.object(FakeDriver, 'paths', ['u/mb/93NP']):
            rc, out = self.run_info(brand='Joulescope')
        self.assertEqual(0, rc)
        self.assertIn('No connected devices found', out)
        self.assertNotIn('u/mb/93NP', out)

    def test_list_device(self):
        rc, out = self.run_info(device='mb')
        self.assertEqual(0, rc)
        self.assertIn('u/mb/93NP', out)
        self.assertNotIn('u/js320/8W2A', out)

    def test_list_device_brand_not_found(self):
        rc, out = self.run_info(device='mb', brand='Joulescope')
        self.assertEqual(0, rc)
        self.assertIn('No connected devices found', out)
        self.assertNotIn('u/mb/93NP', out)
