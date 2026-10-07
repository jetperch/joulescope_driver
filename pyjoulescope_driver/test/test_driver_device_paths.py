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

"""Test the Driver device_paths and find_one_device methods."""

import unittest
from pyjoulescope_driver import Driver, DevicePath
from pyjoulescope_driver.device_filter import DeviceFilterError


LIST = 'u/js320/8W2A,u/mb/93NP,u/js220/000415'


class ListDriver(Driver):
    """Report a fixed device list, independent of the attached devices."""

    def __init__(self, device_list=LIST):
        super().__init__()
        self.device_list = device_list
        self.timeouts = []

    def query(self, topic, timeout=None):
        if topic == '@/list':
            self.timeouts.append(timeout)
            return self.device_list
        return super().query(topic, timeout)


class TestDriverDevicePaths(unittest.TestCase):

    def setUp(self):
        self.d = ListDriver()

    def tearDown(self):
        self.d.finalize()

    def test_all(self):
        paths = self.d.device_paths()
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A', 'u/mb/93NP'], paths)
        for p in paths:
            self.assertIsInstance(p, DevicePath)

    def test_empty(self):
        self.d.device_list = ''
        self.assertEqual([], self.d.device_paths())
        self.assertEqual([], self.d.device_paths('js320', brand='joulescope'))

    def test_specs(self):
        self.assertEqual(['u/mb/93NP'], self.d.device_paths('mb'))
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A'],
                         self.d.device_paths('8w2a, js220'))
        self.assertEqual(['u/js320/8W2A'], self.d.device_paths(['js320-8W2A']))
        self.assertEqual(['u/js320/8W2A'], self.d.device_paths('u/js320'))
        self.assertEqual(['u/js320/8W2A'], self.d.device_paths('u/js320/'))
        self.assertEqual('u/js320/8W2A', self.d.find_one_device('u/js320/'))
        self.assertEqual('u/js320/8W2A', self.d.find_one_device('u/js320'))

    def test_brand(self):
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A'],
                         self.d.device_paths(brand='Joulescope'))
        self.assertEqual([], self.d.device_paths('mb', brand='joulescope'))
        with self.assertRaises(ValueError):
            self.d.device_paths(brand='acme')

    def test_specs_type_error(self):
        with self.assertRaises(TypeError) as cm:
            self.d.device_paths(b'js320')
        self.assertEqual('specs must be str or a list of str, not bytes', str(cm.exception))

    def test_timeout_positional_deprecated(self):
        # Before 2.5.0, the first positional argument was timeout.
        with self.assertWarns(DeprecationWarning):
            paths = self.d.device_paths(2.0)
        with self.assertWarns(DeprecationWarning):
            self.d.device_paths(1)
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A', 'u/mb/93NP'], paths)
        self.assertEqual([2.0, 1.0], self.d.timeouts)
        with self.assertRaises(TypeError):
            self.d.device_paths(True)

    def test_timeout(self):
        self.d.device_paths(timeout=2.5)
        self.d.find_one_device('mb', timeout=1.5)
        self.assertEqual([2.5, 1.5], self.d.timeouts)

    def test_find_one_device(self):
        p = self.d.find_one_device('8W2A')
        self.assertIsInstance(p, DevicePath)
        self.assertEqual('u/js320/8W2A', p)
        self.assertEqual('u/mb/93NP', self.d.find_one_device('mb'))
        self.assertEqual('u/js320/8W2A', self.d.find_one_device('js320', brand='joulescope'))

    def test_find_one_device_single(self):
        self.d.device_list = 'u/js320/8W2A,u/mb/93NP'
        self.assertEqual('u/js320/8W2A', self.d.find_one_device(brand='joulescope'))

    def test_find_one_device_multiple(self):
        with self.assertRaises(DeviceFilterError) as cm:
            self.d.find_one_device()
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A', 'u/mb/93NP'], cm.exception.matches)
        with self.assertRaises(DeviceFilterError) as cm:
            self.d.find_one_device(brand='joulescope')
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A'], cm.exception.matches)

    def test_find_one_device_not_found(self):
        with self.assertRaises(DeviceFilterError) as cm:
            self.d.find_one_device('js110', brand='joulescope')
        e = cm.exception
        self.assertEqual([], e.matches)
        self.assertEqual(['u/js220/000415', 'u/js320/8W2A'], e.available)


class TestDriverDevicePathsLive(unittest.TestCase):
    """Check the attached devices, if any."""

    def test_device_paths(self):
        with Driver() as d:
            paths = d.device_paths()
            self.assertIsInstance(paths, list)
            for p in paths:
                self.assertIsInstance(p, DevicePath)
            self.assertEqual([p for p in paths if p.brand == 'Joulescope'],
                             d.device_paths(brand='joulescope'))
