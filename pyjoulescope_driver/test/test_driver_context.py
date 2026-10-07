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

"""Test SubscribeContext, DeviceContext, device_watch and finalize."""

import queue
import threading
import unittest
from pyjoulescope_driver import Driver, DeviceContext, DevicePath, SubscribeContext
from pyjoulescope_driver.test.test_driver_device_paths import ListDriver


TOPIC = 'test/value'


def _publish(d, topic, value):
    """Publish a local topic, and wait for the driver to process it."""
    d.publish(topic, value, timeout=0)
    Driver.query(d, '@/list')  # bypass ListDriver.query


def _connected_device():
    with Driver() as d:
        paths = d.device_paths(brand='joulescope')
    return paths[0] if paths else None


class Recorder:

    def __init__(self):
        self.q = queue.Queue()

    def __call__(self, *args):
        self.q.put(args)

    def get(self, timeout=1.0):
        return self.q.get(timeout=timeout)

    def assert_empty(self, test, timeout=0.1):
        with test.assertRaises(queue.Empty):
            self.q.get(timeout=timeout)


class TestSubscribeContext(unittest.TestCase):

    def setUp(self):
        self.d = Driver()

    def tearDown(self):
        self.d.finalize()

    def test_unsubscribe(self):
        r = Recorder()
        s = self.d.subscribe(TOPIC, 'pub', r)
        self.assertIsInstance(s, SubscribeContext)
        _publish(self.d, TOPIC, 1)
        self.assertEqual((TOPIC, 1), r.get())
        s.unsubscribe()
        s.unsubscribe()  # again does nothing
        _publish(self.d, TOPIC, 2)
        r.assert_empty(self)

    def test_context_manager(self):
        r = Recorder()
        with self.d.subscribe(TOPIC, 'pub', r) as s:
            self.assertIsInstance(s, SubscribeContext)
            _publish(self.d, TOPIC, 1)
            self.assertEqual((TOPIC, 1), r.get())
        _publish(self.d, TOPIC, 2)
        r.assert_empty(self)

    def test_exit_unsubscribe_error_keeps_body_error(self):
        s = SubscribeContext(FakeDriver(), [(TOPIC, print)])

        def unsubscribe(topic, fn, timeout=None):
            raise RuntimeError('unsubscribe failed')
        s._driver.unsubscribe = unsubscribe
        with self.assertLogs('pyjoulescope_driver.binding', 'WARNING'):
            with self.assertRaises(ValueError):
                with s:
                    raise ValueError('body failed')

    def test_unsubscribe_after_finalize(self):
        s = self.d.subscribe(TOPIC, 'pub', Recorder())
        self.d.finalize()
        s.unsubscribe()


class FakeDriver:
    """Record the calls that DeviceContext forwards."""

    def __init__(self):
        self.calls = []

    def __getattr__(self, name):
        def fn(*args, **kwargs):
            self.calls.append((name, args, kwargs))
            return name
        return fn

    def subscribe(self, topic, flags, fn, timeout=None):
        self.calls.append(('subscribe', (topic, flags, fn, timeout), {}))
        return SubscribeContext(self, [(topic, fn)])


class TestDeviceContext(unittest.TestCase):

    def setUp(self):
        self.d = FakeDriver()
        self.dev = DeviceContext(self.d, 'u/js320/8W2A')

    def test_device_path(self):
        self.assertIsInstance(self.dev.device_path, DevicePath)
        self.assertEqual('js320', self.dev.device_path.model)
        self.assertEqual("DeviceContext('u/js320/8W2A')", repr(self.dev))
        self.assertEqual('u/js320/8W2A/s/i/range/mode', self.dev.topic('s/i/range/mode'))

    def test_forward(self):
        fn = Recorder()
        self.dev.publish('s/i/range/mode', 'auto', timeout=0)
        self.dev.query('h/fs')
        self.dev.subscribe('s/stats/value', 'pub', fn)
        self.dev.unsubscribe('s/stats/value', fn)
        self.dev.publish_and_wait('s/gpi/+/!req', 0, 's/gpi/+/!value', timeout=1.0)
        self.dev.close()
        p = 'u/js320/8W2A'
        self.assertEqual([
            ('publish', (f'{p}/s/i/range/mode', 'auto', 0), {}),
            ('query', (f'{p}/h/fs', None), {}),
            ('subscribe', (f'{p}/s/stats/value', 'pub', fn, None), {}),
            ('unsubscribe', (f'{p}/s/stats/value', fn, None), {}),
            ('publish_and_wait', (f'{p}/s/gpi/+/!req', 0, f'{p}/s/gpi/+/!value'),
             {'timeout': 1.0, 'match': None}),
            ('close', (p, None), {}),
        ], self.d.calls)

    def test_context_manager(self):
        with self.dev as dev:
            self.assertIs(self.dev, dev)
        self.assertEqual([('close', ('u/js320/8W2A', None), {})], self.d.calls)

    def _close_fails(self):
        def close(device_prefix, timeout=None):
            raise RuntimeError('close failed')
        self.d.close = close

    def test_exit_close_error_keeps_body_error(self):
        self._close_fails()
        with self.assertLogs('pyjoulescope_driver.binding', 'WARNING') as cm:
            with self.assertRaises(ValueError):
                with self.dev:
                    raise ValueError('body failed')
        self.assertIn('close failed', '\n'.join(cm.output))

    def test_exit_close_error_raised(self):
        self._close_fails()
        with self.assertRaises(RuntimeError):
            with self.dev:
                pass

    def test_close_once(self):
        self.dev.close()
        self.dev.close()
        self.assertEqual([('close', ('u/js320/8W2A', None), {})], self.d.calls)

    def test_close_unsubscribes(self):
        p = 'u/js320/8W2A'
        f1, f2, f3 = Recorder(), Recorder(), Recorder()
        s1 = self.dev.subscribe('a', 'pub', f1)
        self.dev.subscribe('b', 'pub', f2)
        self.dev.subscribe('c', 'pub', f3)
        s1.unsubscribe()                 # by the application, through the context
        self.dev.unsubscribe('b', f2)    # by the application, through the device
        self.d.calls.clear()
        self.dev.close()
        self.assertEqual([('unsubscribe', (f'{p}/c', f3, None), {}),
                          ('close', (p, None), {})], self.d.calls)

    def test_close_unsubscribes_with_error(self):
        def unsubscribe(topic, fn, timeout=None):
            raise RuntimeError('unsubscribe failed')
        self.dev.subscribe('a', 'pub', Recorder())
        self.d.calls.clear()
        self.d.unsubscribe = unsubscribe
        with self.assertRaises(RuntimeError):
            self.dev.close()
        self.assertEqual([('close', ('u/js320/8W2A', None), {})], self.d.calls)

    def test_subscriptions_pruned(self):
        for idx in range(10):
            self.dev.subscribe('a', 'pub', Recorder()).unsubscribe()
        self.assertEqual(1, len(self.dev._subscriptions))


    def test_topic_type_error(self):
        with self.assertRaises(TypeError) as cm:
            self.dev.publish(1, 0)
        self.assertEqual('topic must be str, not int', str(cm.exception))
        with self.assertRaises(TypeError) as cm:
            self.dev.publish_and_wait('a', 0, None)
        self.assertIn('response_topic must be str', str(cm.exception))
        self.assertEqual([], self.d.calls)


class NoCloseDriver(Driver):
    """Use local topics as a device, without a device to close."""

    def __init__(self):
        super().__init__()
        self.closed = []

    def close(self, device_prefix, timeout=None):
        self.closed.append(device_prefix)


class TestDeviceContextDriver(unittest.TestCase):

    def test_close_unsubscribes(self):
        with NoCloseDriver() as d:
            dev = DeviceContext(d, 'test/dev')
            r = Recorder()
            with dev:
                dev.subscribe('value', 'pub', r)
                _publish(d, 'test/dev/value', 1)
                self.assertEqual(('test/dev/value', 1), r.get())
            self.assertEqual(['test/dev'], d.closed)
            _publish(d, 'test/dev/value', 2)
            r.assert_empty(self)

class TestFinalize(unittest.TestCase):

    def test_after_finalize(self):
        d = Driver()
        d.finalize()
        d.finalize()  # again does nothing
        for fn, args in [(d.publish, (TOPIC, 1)), (d.query, ('@/list',)),
                         (d.subscribe, (TOPIC, 'pub', print)), (d.open, ('u/js320/1',)),
                         (d.device_paths, ()), (d.find_one_device, ('js320',))]:
            with self.assertRaises(RuntimeError, msg=fn.__name__) as cm:
                fn(*args)
            self.assertEqual('Driver is finalized', str(cm.exception))
        d.unsubscribe(TOPIC, print)
        d.unsubscribe_all(print)
        d.close('u/js320/1')

    def test_context_manager_finalizes(self):
        with Driver() as d:
            pass
        with self.assertRaises(RuntimeError):
            d.query('@/list')


class CloseRecordDriver(Driver):

    def __init__(self):
        super().__init__()
        self.closed = []
        self.connected = None  # None for the attached devices

    def close(self, device_prefix, timeout=None):
        self.closed.append(device_prefix)
        return super().close(device_prefix, timeout)

    def query(self, topic, timeout=None):
        if topic == '@/list' and self.connected is not None:
            return self.connected
        return super().query(topic, timeout)


@unittest.skipIf(_connected_device() is None, 'requires a connected Joulescope')
class TestLiveDevice(unittest.TestCase):

    def setUp(self):
        self.device_path = _connected_device()

    def test_open_context(self):
        with Driver() as d:
            with d.open(self.device_path, mode='restore') as dev:
                self.assertIsInstance(dev, DeviceContext)
                self.assertEqual(self.device_path, dev.device_path)
                self.assertEqual(d.query(f'{self.device_path}/h/fs'), dev.query('h/fs'))

    def test_finalize_closes_opened(self):
        d = CloseRecordDriver()
        d.open(self.device_path, mode='restore')
        d.finalize()
        self.assertEqual([self.device_path], d.closed)

    def test_finalize_skips_closed(self):
        d = CloseRecordDriver()
        d.open(self.device_path, mode='restore').close()
        d.closed.clear()
        d.finalize()
        self.assertEqual([], d.closed)

    def test_finalize_skips_removed(self):
        d = CloseRecordDriver()
        d.open(self.device_path, mode='restore')
        d.connected = ''  # the device appears removed
        d.finalize()
        self.assertEqual([], d.closed)
        with Driver() as d2:  # clean up the device left open
            d2.open(self.device_path, mode='restore').close()


class TestDeviceWatch(unittest.TestCase):

    def setUp(self):
        self.d = ListDriver('u/js320/8W2A,u/mb/93NP')
        self.events = queue.Queue()

    def tearDown(self):
        self.d.finalize()

    def on_add(self, device_path):
        self.events.put(('add', device_path))

    def on_remove(self, device_path):
        self.events.put(('remove', device_path))

    def watch(self, **kwargs):
        return self.d.device_watch(self.on_add, self.on_remove, **kwargs)

    def drain(self):
        events = []
        while not self.events.empty():
            events.append(self.events.get())
        return events

    def event(self, kind, device_path):
        _publish(self.d, f'@/!{kind}', device_path)
        return self.events.get(timeout=1.0)

    def assert_no_event(self, kind, device_path):
        _publish(self.d, f'@/!{kind}', device_path)
        self.assertEqual([], self.drain())

    def test_connected(self):
        with self.watch() as w:
            self.assertIsInstance(w, SubscribeContext)
            events = self.drain()
        self.assertEqual([('add', 'u/js320/8W2A'), ('add', 'u/mb/93NP')], events)
        for _, device_path in events:
            self.assertIsInstance(device_path, DevicePath)

    def test_add_remove(self):
        with self.watch():
            self.drain()
            kind, device_path = self.event('add', 'u/js220/000415')
            self.assertEqual(('add', 'u/js220/000415'), (kind, device_path))
            self.assertIsInstance(device_path, DevicePath)
            self.assert_no_event('add', 'u/js220/000415')  # duplicate
            self.assertEqual(('remove', 'u/js220/000415'), self.event('remove', 'u/js220/000415'))
            self.assert_no_event('remove', 'u/js220/000415')  # not connected
            self.assertEqual(('remove', 'u/js320/8W2A'), self.event('remove', 'u/js320/8W2A'))
            self.assertEqual(('add', 'u/js320/8W2A'), self.event('add', 'u/js320/8W2A'))

    def test_filter(self):
        with self.watch(specs='js220,8W2A', brand='js'):
            self.assertEqual([('add', 'u/js320/8W2A')], self.drain())
            self.assert_no_event('add', 'u/mb/1')
            self.assert_no_event('add', 'u/js320/1')
            self.assert_no_event('remove', 'u/mb/93NP')
            self.assertEqual(('add', 'u/js220/1'), self.event('add', 'u/js220/1'))

    def test_unsubscribe(self):
        w = self.watch()
        self.drain()
        w.unsubscribe()
        self.assert_no_event('add', 'u/js220/1')
        self.assert_no_event('remove', 'u/js320/8W2A')

    def test_event_while_listing(self):
        # A blocking driver call from on_add while listing must not
        # deadlock, and events received while listing follow in order.
        def on_add(device_path):
            self.on_add(device_path)
            if device_path == 'u/js320/8W2A':
                self.d.publish('@/!remove', device_path, timeout=0)
                _publish(self.d, '@/!add', 'u/js220/1')
        with self.d.device_watch(on_add, self.on_remove):
            self.assertEqual([('add', 'u/js320/8W2A'), ('add', 'u/mb/93NP'),
                              ('remove', 'u/js320/8W2A'), ('add', 'u/js220/1')],
                             self.drain())

    def test_on_add_error_unsubscribes(self):
        def on_add(device_path):
            raise ValueError('on_add failed')
        with self.assertRaises(ValueError):
            self.d.device_watch(on_add, self.on_remove)
        self.d.device_list = ''
        with self.watch():  # confirm event delivery, without the failed watch
            self.assertEqual(('add', 'u/js220/1'), self.event('add', 'u/js220/1'))

    def test_invalid(self):
        with self.assertRaises(ValueError):
            self.watch(brand='acme')
        with self.assertRaises(TypeError):
            self.watch(specs=1)


class TestDeviceWatchLive(unittest.TestCase):

    def test_connected(self):
        added = []
        with Driver() as d:
            with d.device_watch(added.append, print):
                self.assertEqual(d.device_paths(), added)


class TestFindDevices(unittest.TestCase):

    def test_alias(self):
        with ListDriver() as d:
            self.assertEqual(d.device_paths(), d.find_devices())
            self.assertEqual(d.device_paths('js320', brand='js'),
                             d.find_devices('js320', brand='js'))
