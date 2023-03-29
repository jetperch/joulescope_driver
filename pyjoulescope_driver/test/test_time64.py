# Copyright 2023 Jetperch LLC
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

"""
Test the time module.
"""

import unittest
from pyjoulescope_driver import time64
import datetime


class TestTime(unittest.TestCase):

    def test_conversion(self):
        dt_now = datetime.datetime.now(tz=datetime.timezone.utc)
        t_now = dt_now.timestamp()
        t64_now = time64.as_time64(t_now)
        t2_now = time64.as_timestamp(t64_now)
        self.assertEqual(t_now, t2_now)

        dt2_now = time64.as_datetime(t64_now)
        self.assertEqual(dt_now, dt2_now)

    def test_filename(self):
        t = datetime.datetime(2023, 1, 1, tzinfo=datetime.timezone.utc)
        fname = time64.filename(extension='_hello.txt', t=t)
        self.assertEqual("20230101_000000_hello.txt", fname)

    def test_duration_to_seconds(self):
        with self.assertRaises(ValueError):
            time64.duration_to_seconds(None)
        with self.assertRaises(ValueError):
            time64.duration_to_seconds('')
        self.assertEqual(1.0, time64.duration_to_seconds(1))
        self.assertEqual(1.0, time64.duration_to_seconds(1.0))
        self.assertEqual(1.0, time64.duration_to_seconds('1'))
        self.assertEqual(1.0, time64.duration_to_seconds('1s'))
        self.assertEqual(1.5, time64.duration_to_seconds('1.5'))
        self.assertEqual(1.5, time64.duration_to_seconds('1.5s'))
        self.assertEqual(60.0, time64.duration_to_seconds('1m'))
        self.assertEqual(90.0, time64.duration_to_seconds('1.5m'))
        self.assertEqual(3600.0, time64.duration_to_seconds('1h'))
        self.assertEqual(86400.0, time64.duration_to_seconds('1d'))
        with self.assertRaises(ValueError):
            time64.duration_to_seconds('hello')
