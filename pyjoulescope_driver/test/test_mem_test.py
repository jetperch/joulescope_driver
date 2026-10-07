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

"""Test the mem_test entry point device selection with a fake driver."""

import argparse
import contextlib
import io
import unittest
from unittest import mock

from pyjoulescope_driver import device_filter
from pyjoulescope_driver.entry_points import mem_test


class _Opened(Exception):
    pass


class FakeDriver:

    def __init__(self, paths):
        self.paths = paths
        self.log_level = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def device_paths(self, specs=None, brand=None, timeout=None):
        return device_filter.find(self.paths, specs, brand)

    def find_one_device(self, specs=None, brand=None, timeout=None):
        return device_filter.find_one(self.paths, specs, brand)

    def open(self, device, mode=None, timeout=None):
        raise _Opened(device)


def _args(device=None, brand=None):
    return argparse.Namespace(topic='s/flash/!cmd', target=0, offset=0x140000,
                              size=0x1000, device=device, brand=brand, jsdrv_log_level='off')


class TestMemTest(unittest.TestCase):

    def _run(self, paths, device=None, brand=None):
        driver = FakeDriver(paths)
        with mock.patch.object(mem_test, 'Driver', lambda: driver):
            with contextlib.redirect_stdout(io.StringIO()):
                return mem_test.on_cmd(_args(device, brand))

    def _opened(self, paths, device=None, brand=None):
        with self.assertRaises(_Opened) as ctx:
            self._run(paths, device, brand)
        return ctx.exception.args[0]

    def test_no_devices(self):
        self.assertEqual(1, self._run([]))

    def test_single_device_without_filter(self):
        self.assertEqual('u/js320/8W2A', self._opened(['u/js320/8W2A']))

    def test_multiple_devices_require_filter(self):
        self.assertEqual(1, self._run(['u/js220/000415', 'u/js320/8W2A']))

    def test_filter_by_model(self):
        paths = ['u/js220/000415', 'u/js320/8W2A']
        self.assertEqual('u/js320/8W2A', self._opened(paths, 'js320'))

    def test_filter_by_serial_number(self):
        paths = ['u/js320/8', 'u/js320/8W2A']
        self.assertEqual('u/js320/8', self._opened(paths, '8'))

    def test_filter_ambiguous(self):
        self.assertEqual(1, self._run(['u/js320/8', 'u/js320/8W2A'], 'js320'))

    def test_filter_not_found(self):
        self.assertEqual(1, self._run(['u/js320/8W2A'], 'js220'))

    def test_non_joulescope(self):
        self.assertEqual('u/mb/93NP', self._opened(['u/mb/93NP']))
        self.assertEqual('u/mb/93NP', self._opened(['u/js320/8W2A', 'u/mb/93NP'], 'mb'))

    def test_brand(self):
        paths = ['u/js320/8W2A', 'u/mb/93NP']
        self.assertEqual(1, self._run(paths))
        self.assertEqual('u/js320/8W2A', self._opened(paths, brand='Joulescope'))
        self.assertEqual(1, self._run(paths, 'mb', brand='Joulescope'))
