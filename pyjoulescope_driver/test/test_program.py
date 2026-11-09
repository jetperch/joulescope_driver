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

import unittest
from pyjoulescope_driver import program
from pyjoulescope_driver.release import SUBTYPE_SENSOR_FPGA


PATH = 'u/&js220/000415'
PATH_APP = 'u/js220/000415'
ERASE = 'h/mem/s/app1/!erase'
WRITE = 'h/mem/s/app1/!write'
RESET = 'h/!reset'
FPGA_VERSION = 0x01030003
QUERY = {'c/fw/version': 0x01020001, 'c/target': 1, 's/fpga/version': FPGA_VERSION}


class FakeDriver:
    """Record publishes and raise the scripted exception for each call.

    :param failures: The exception (or None) for each publish, in order.
        Publishes past the end of the list succeed, unless fail_all.
    """

    def __init__(self, failures=None, fail_all=None, path=None):
        self.publishes = []
        self._failures = list(failures or [])
        self._fail_all = fail_all
        self._removed = False
        self._path = PATH if path is None else path

    def publish(self, topic, value, timeout=None):
        self.publishes.append((topic, timeout))
        if topic.endswith('/h/!reset'):
            self._removed = True
            self._path = PATH_APP if value == 'app' else PATH
            return
        if self._failures:
            ex = self._failures.pop(0)
            if ex is not None:
                raise ex
        elif self._fail_all:
            raise RuntimeError('fail_all')

    def device_paths(self):
        if self._removed:
            self._removed = False
            return []
        return [self._path]

    def query(self, topic):
        return QUERY[topic.split('/', 3)[-1]]

    def open(self, device_path, mode=None, timeout=None):
        pass

    def close(self, device_path, timeout=None):
        pass

    @property
    def topics(self):
        return [t.split('/', 3)[-1] for t, _ in self.publishes]


def _segment(size=1000):
    return {'subtype': SUBTYPE_SENSOR_FPGA, 'version': FPGA_VERSION, 'img': b'\x5a' * size}


def _program(driver, size=1000):
    p = program.Programmer(driver, PATH)
    return p.segment_program(_segment(size), lambda x, y: None, 'fpga', 0.3, 0.5)


class TestProgramSensorFpga(unittest.TestCase):

    def test_success(self):
        d = FakeDriver()
        self.assertIsNone(_program(d))
        self.assertEqual([ERASE, WRITE], d.topics)

    def test_write_timeout_scales_with_size(self):
        d = FakeDriver()
        _program(d, 10)
        self.assertEqual(program.FPGA_WRITE_TIMEOUT_MIN, d.publishes[1][1])
        d = FakeDriver()
        size = int(program.FPGA_WRITE_RATE_MIN * 30)
        _program(d, size)
        self.assertAlmostEqual(30.0, d.publishes[1][1])

    def test_write_timeout_erases_resets_and_retries(self):
        d = FakeDriver([None, TimeoutError('write')])
        self.assertIsNone(_program(d))
        self.assertEqual([ERASE, WRITE, ERASE, RESET, ERASE, WRITE], d.topics)

    def test_stale_abort_on_erase_retries(self):
        d = FakeDriver([RuntimeError('ABORTED')])
        self.assertIsNone(_program(d))
        self.assertEqual([ERASE, ERASE, RESET, ERASE, WRITE], d.topics)

    def test_failure_leaves_erased_so_golden_boots(self):
        n = program.FPGA_ATTEMPTS
        d = FakeDriver([None, TimeoutError('write'), None] * n)
        with self.assertRaises(RuntimeError):
            _program(d)
        expect = [ERASE, WRITE, ERASE] + [RESET, ERASE, WRITE, ERASE] * (n - 1)
        self.assertEqual(expect, d.topics)

    def test_failure_everything(self):
        n = program.FPGA_ATTEMPTS
        d = FakeDriver(fail_all=True)
        with self.assertRaises(RuntimeError):
            _program(d)
        expect = [ERASE] + [ERASE] * n + ([RESET, ERASE] + [ERASE] * n) * (n - 1)
        self.assertEqual(expect, d.topics)
        self.assertNotIn(WRITE, d.topics)


class TestReleaseProgram(unittest.TestCase):

    def setUp(self):
        self._release_to_segments = program.release_to_segments
        program.release_to_segments = lambda img: {SUBTYPE_SENSOR_FPGA: _segment()}

    def tearDown(self):
        program.release_to_segments = self._release_to_segments

    def test_start_in_updater_erases_fpga_first(self):
        d = FakeDriver(path=PATH)
        program.release_program(d, PATH, b'')
        self.assertEqual(ERASE, d.topics[0])

    def test_start_in_app_does_not_erase(self):
        d = FakeDriver(path=PATH_APP)
        program.release_program(d, PATH_APP, b'')
        self.assertNotIn(ERASE, d.topics)
