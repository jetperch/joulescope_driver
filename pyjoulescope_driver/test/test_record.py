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

"""Test pyjoulescope_driver.record using a fake driver and real JLS files."""

import numpy as np
import os
import tempfile
import unittest

from pyjoulescope_driver import time64
from pyjoulescope_driver.record import Record

try:
    from pyjls import Reader
except ImportError:
    Reader = None


_SAMPLE_RATE = 1000000


class FakeDriver:
    """Capture subscribe/publish calls and allow data injection."""

    def __init__(self):
        self.subscriptions = {}   # topic -> [fn, ...]
        self.publishes = []       # (topic, value)

    def subscribe(self, topic, flags, fn, timeout=None):
        self.subscriptions.setdefault(topic, []).append(fn)

    def unsubscribe(self, topic, fn, timeout=None):
        if topic in self.subscriptions:
            self.subscriptions[topic] = [f for f in self.subscriptions[topic] if f is not fn]

    def publish(self, topic, value, timeout=None):
        self.publishes.append((topic, value))

    def inject(self, topic, value):
        for fn in self.subscriptions.get(topic, []):
            fn(topic, value)


def _stream_value(sample_id, data, sample_rate=_SAMPLE_RATE, decimate_factor=1):
    return {
        'sample_id': sample_id,
        'utc': time64.HOUR + (sample_id * time64.SECOND) // sample_rate,
        'field_id': 0,
        'index': 0,
        'sample_rate': sample_rate,
        'decimate_factor': decimate_factor,
        'time_map': {'offset_time': time64.HOUR, 'offset_counter': 0,
                     'counter_rate': float(sample_rate)},
        'data': data,
    }


@unittest.skipIf(Reader is None, 'pyjls not available')
class TestRecord(unittest.TestCase):

    def setUp(self):
        self.driver = FakeDriver()
        f = tempfile.NamedTemporaryFile(suffix='.jls', delete=False)
        f.close()
        self.filename = f.name

    def tearDown(self):
        if os.path.isfile(self.filename):
            os.remove(self.filename)

    def _signal_by_name(self, reader, name):
        for s in reader.signals.values():
            if s.name == name:
                return s
        raise KeyError(name)

    def _read_signal(self, name, length):
        with Reader(self.filename) as r:
            return r.fsr(self._signal_by_name(r, name).signal_id, 0, length)

    def test_f32_roundtrip(self):
        r = Record(self.driver, 'u/js220/000001', signals=['current'])
        r.open(self.filename)
        expect = np.arange(1000, dtype=np.float32) * 0.001
        self.driver.inject('u/js220/000001/s/i/!data', _stream_value(0, expect[:500].copy()))
        self.driver.inject('u/js220/000001/s/i/!data', _stream_value(500, expect[500:].copy()))
        r.close()
        data = self._read_signal('current', 1000)
        np.testing.assert_allclose(expect, data)

    def test_u1_roundtrip(self):
        # the driver provides u1 data packed little-endian
        r = Record(self.driver, 'u/js220/000001', signals=['gpi[0]'])
        r.open(self.filename)
        expect = ((np.arange(1024) * 0x9E37) >> 7).astype(np.uint8) & 1
        packed = np.packbits(expect, bitorder='little')
        self.driver.inject('u/js220/000001/s/gpi/0/!data', _stream_value(0, packed[:64].copy()))
        self.driver.inject('u/js220/000001/s/gpi/0/!data', _stream_value(512, packed[64:].copy()))
        r.close()
        data = self._read_signal('gpi[0]', 1024)
        unpacked = np.unpackbits(np.asarray(data), bitorder='little')[:1024]
        np.testing.assert_array_equal(expect, unpacked)

    def test_u4_roundtrip(self):
        # the driver provides u4 data unpacked, one sample per byte
        r = Record(self.driver, 'u/js220/000001', signals=['current_range'])
        r.open(self.filename)
        expect = (np.arange(1000, dtype=np.uint8) * 7) & 0x0f
        self.driver.inject('u/js220/000001/s/i/range/!data', _stream_value(0, expect[:500].copy()))
        self.driver.inject('u/js220/000001/s/i/range/!data', _stream_value(500, expect[500:].copy()))
        r.close()
        data = np.asarray(self._read_signal('current_range', 1000))
        unpacked = np.empty(len(data) * 2, dtype=np.uint8)
        unpacked[0::2] = data & 0x0f
        unpacked[1::2] = (data >> 4) & 0x0f
        np.testing.assert_array_equal(expect, unpacked[:1000])

    def test_trigger_in_short_name(self):
        # 'T' selects trigger_in, streamed as u1 on s/gpi/7
        r = Record(self.driver, 'u/js320/000001', signals=['T'])
        r.open(self.filename)
        expect = (np.arange(1024) & 1).astype(np.uint8)
        packed = np.packbits(expect, bitorder='little')
        self.driver.inject('u/js320/000001/s/gpi/7/!data', _stream_value(0, packed))
        r.close()
        data = self._read_signal('trigger_in', 1024)
        unpacked = np.unpackbits(np.asarray(data), bitorder='little')[:1024]
        np.testing.assert_array_equal(expect, unpacked)

    def test_decimated_sample_rate(self):
        # the recorded rate must be sample_rate // decimate_factor
        r = Record(self.driver, 'u/js320/000001', signals=['current'])
        r.open(self.filename)
        data = np.zeros(1000, dtype=np.float32)
        self.driver.inject('u/js320/000001/s/i/!data',
                           _stream_value(0, data, sample_rate=16000000, decimate_factor=16))
        r.close()
        with Reader(self.filename) as rd:
            s = self._signal_by_name(rd, 'current')
            self.assertEqual(1000000, s.sample_rate)
            self.assertEqual(1000, s.length)

    def test_multi_device_single_subscribe(self):
        # each (device, signal) pair must subscribe and enable exactly once
        paths = ['u/js220/000001', 'u/js220/000002']
        r = Record(self.driver, paths, signals=['current'])
        r.open(self.filename)
        for path in paths:
            self.assertEqual(1, len(self.driver.subscriptions[f'{path}/s/i/!data']))
        enables = [p for p in self.driver.publishes if p[1] == 1]
        self.assertEqual(2, len(enables))
        self.assertEqual(sorted([f'{p}/s/i/ctrl' for p in paths]),
                         sorted([p[0] for p in enables]))
        for path in paths:
            self.driver.inject(f'{path}/s/i/!data',
                               _stream_value(0, np.zeros(100, dtype=np.float32)))
        r.close()
        for path in paths:
            self.assertEqual(0, len(self.driver.subscriptions[f'{path}/s/i/!data']))
        with Reader(self.filename) as rd:
            self.assertEqual(2, len(rd.signals) - 1)  # signal 0 reserved

    def test_close_twice(self):
        r = Record(self.driver, 'u/js220/000001', signals=['current'])
        r.open(self.filename)
        r.close()
        r.close()  # previously raised AttributeError


@unittest.skipIf(Reader is None, 'pyjls not available')
class TestRecordClose(unittest.TestCase):

    def test_close_without_open(self):
        r = Record(FakeDriver(), 'u/js320/000001', signals=['current'])
        r.close()  # must not raise

    def test_close_after_failed_open(self):
        r = Record(FakeDriver(), 'u/js320/000001', signals=['current'])
        with self.assertRaises(Exception):
            r.open('/nonexistent_dir_zzz/out.jls')
        r.close()  # must not raise


if __name__ == '__main__':
    unittest.main()
