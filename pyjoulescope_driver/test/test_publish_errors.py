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

"""Test that errors no caller receives surface in the log."""

import logging
import struct
import subprocess
import sys
import time
import unittest
from pyjoulescope_driver import Driver, LogLevel


class _Records(logging.Handler):

    def __init__(self):
        super().__init__(logging.DEBUG)
        self.records = []

    def emit(self, record):
        self.records.append(record)

    def errors(self):
        return [r.getMessage() for r in self.records if r.levelno >= logging.ERROR]


class TestPublishErrors(unittest.TestCase):

    def setUp(self):
        self.d = Driver()
        self.level = self.d.log_level
        self.d.log_level = 'error'
        self.handler = _Records()
        self.logger = logging.getLogger('jsdrv')
        self.logger_level = self.logger.level
        self.logger.setLevel(logging.DEBUG)
        self.logger.addHandler(self.handler)

    def tearDown(self):
        self.d.finalize()
        self.d.log_level = self.level
        self.logger.removeHandler(self.handler)
        self.logger.setLevel(self.logger_level)

    def _return_code(self, topic, rc):
        """Publish a return code that no caller waits for."""
        self.d.publish(topic + '#', struct.pack('<i', rc), timeout=0)
        self.d.query('@/list')  # wait for the frontend to process the publish

    def _wait_errors(self, count, timeout=1.0):
        t_end = time.time() + timeout
        while len(self.handler.errors()) < count and time.time() < t_end:
            time.sleep(0.01)
        return self.handler.errors()

    def test_unclaimed_error_logged(self):
        self._return_code('test/value', 5)
        self.assertEqual(['publish test/value failed: 5 PARAMETER_INVALID'],
                         self._wait_errors(1))

    def test_unclaimed_success_not_logged(self):
        self._return_code('test/value', 0)
        self._return_code('test/other', 16)  # marker: logs after the first
        self.assertEqual(['publish test/other failed: 16 NOT_FOUND'],
                         self._wait_errors(1))


class TestRejectedPublish(unittest.TestCase):
    """A rejected publish does not leave the rejected value retained."""

    TOPIC = 'test/dev/value'

    def setUp(self):
        self.d = Driver()
        self.values = []
        self.sub = self.d.subscribe(self.TOPIC, 'pub', lambda t, v: self.values.append(v))

    def tearDown(self):
        self.d.finalize()

    def _publish(self, value):
        self.d.publish(self.TOPIC, value, timeout=0)
        self.d.query('@/list')  # wait for the frontend to process the publish

    def _reject(self, rc=16):
        self.d.publish(self.TOPIC + '#', struct.pack('<i', rc), timeout=0)
        self.d.query('@/list')

    def test_restores_previous(self):
        self._publish(1)
        self._publish(2)
        self._reject()
        self.assertEqual(1, self.d.query(self.TOPIC))
        self.assertEqual([1, 2, 1], self.values)

    def test_clears_without_previous(self):
        self._publish(2)
        self._reject()
        self.assertIsNone(self.d.query(self.TOPIC))
        self.assertEqual([2], self.values)

    def test_repeat_forwarded(self):
        self._publish(2)
        self._reject()
        self._publish(2)
        self.assertEqual([2, 2], self.values)

    def test_closed_keeps_value(self):
        self._publish(1)
        self._publish(2)
        self._reject(22)  # CLOSED
        self.assertEqual(2, self.d.query(self.TOPIC))
        self.assertEqual([1, 2], self.values)


class TestLogLevelDefault(unittest.TestCase):

    def test_default_error(self):
        # A new process, since the default applies at the first Driver.
        code = ('from pyjoulescope_driver import Driver\n'
                'with Driver() as d:\n'
                '    print(d.log_level)\n')
        out = subprocess.run([sys.executable, '-c', code], capture_output=True,
                             text=True, timeout=60, check=True).stdout
        self.assertEqual(str(LogLevel.ERROR), out.strip())

    def test_application_level_kept(self):
        # The default applies once, so a later Driver keeps the level.
        with Driver() as d:
            level = d.log_level
            d.log_level = 'off'
        try:
            with Driver() as d:
                self.assertEqual(LogLevel.OFF, d.log_level)
        finally:
            d.log_level = level
