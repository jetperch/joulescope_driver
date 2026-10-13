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

"""The streaming entry points stop when their device is unplugged."""

import argparse
import contextlib
import io
import os
import tempfile
import threading
import time
import unittest
from unittest import mock
from pyjoulescope_driver import DevicePath, device_filter
from pyjoulescope_driver.entry_points import measure, record, statistics

try:
    import pyjls
except ImportError:
    pyjls = None  # the wheel tests run without it: skip the record tests

PATH = DevicePath('u/js220/000001')


class _Subscription:

    def __init__(self, driver, topic, fn):
        self._driver, self._topic, self._fn = driver, topic, fn

    def unsubscribe(self, timeout=None):
        self._driver.unsubscribe(self._topic, self._fn)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.unsubscribe()


class FakeDriver:
    """A driver whose only device is removed 0.2 seconds after the first removal subscriber."""

    def __init__(self):
        self.log_level = None
        self.fns = {}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def find_devices(self, specs=None, brand=None, timeout=None):
        return device_filter.find([PATH], specs, brand)

    def find_one_device(self, specs=None, brand=None, timeout=None):
        return device_filter.find_one([PATH], specs, brand)

    def open(self, device_path, mode=None, timeout=None):
        pass

    def close(self, device_path, timeout=None):
        pass

    def publish(self, topic, value, timeout=None):
        pass

    def query(self, topic, timeout=None):
        return 1

    def subscribe(self, topic, flags, fn, timeout=None):
        self.fns.setdefault(topic, []).append(fn)
        if topic == '@/!remove':
            threading.Timer(0.2, fn, args=(topic, str(PATH))).start()
        return _Subscription(self, topic, fn)

    def unsubscribe(self, topic, fn, timeout=None):
        if fn in self.fns.get(topic, []):
            self.fns[topic].remove(fn)


class TestEntryPointUnplug(unittest.TestCase):

    def _run(self, module, **kwargs):
        args = argparse.Namespace(jsdrv_log_level='off', device=None, duration=30, verbose=False,
                                  **kwargs)
        out = io.StringIO()
        t_start = time.time()
        with mock.patch.object(module, 'Driver', FakeDriver), contextlib.redirect_stdout(out):
            rc = module.on_cmd(args)
        self.assertLess(time.time() - t_start, 5.0)
        self.assertEqual(1, rc)
        self.assertIn(f'{PATH} removed: stop', out.getvalue())

    @unittest.skipIf(pyjls is None, 'pyjls not available')
    def test_record(self):
        with tempfile.TemporaryDirectory() as path:
            self._run(record, filename=os.path.join(path, 'out.jls'), open='defaults',
                      frequency=None, set=[], signals='current,voltage', note=None)

    @unittest.skipIf(pyjls is None, 'pyjls not available')
    def test_record_close_error(self):
        # a write error makes Record.close() raise: report it, do not traceback
        class FailingRecord(record.Record):
            def close(self):
                super().close()
                raise RuntimeError('close IO[4]: Input/output error')

        args = argparse.Namespace(jsdrv_log_level='off', device=None, duration=0.05, verbose=False,
                                  open='defaults', frequency=None, set=[], signals='current',
                                  note=None)
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as path:
            args.filename = os.path.join(path, 'out.jls')
            with mock.patch.object(record, 'Driver', FakeDriver), \
                    mock.patch.object(record, 'Record', FailingRecord), \
                    contextlib.redirect_stdout(out):
                rc = record.on_cmd(args)
        self.assertEqual(1, rc)
        self.assertIn('ERROR: recording may be incomplete', out.getvalue())

    def test_statistics(self):
        self._run(statistics, frequency=2, js110_host=False)

    def test_measure(self):
        self._run(measure, no_progress=None, show_only=None)


if __name__ == '__main__':
    unittest.main()
