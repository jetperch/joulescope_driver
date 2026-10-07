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

"""Test the Driver topic and device_prefix arguments."""

import threading
import unittest
from pyjoulescope_driver import Driver, DevicePath


class TestDriverTopic(unittest.TestCase):

    def setUp(self):
        self.d = Driver()

    def tearDown(self):
        self.d.finalize()

    def test_str_subclass_topics(self):
        topic = DevicePath('test/value')
        event = threading.Event()
        values = []

        def on_pub(t, v):
            values.append(v)
            event.set()

        self.d.subscribe(topic, 'pub', on_pub)
        self.d.publish(topic, 1, timeout=0)
        self.assertTrue(event.wait(1.0))
        self.assertEqual([1], values)
        self.d.unsubscribe(topic, on_pub)
        self.assertIsInstance(self.d.query(DevicePath('@/list')), str)

    def _assert_type_error(self, name, fn, *args, **kwargs):
        with self.assertRaises(TypeError) as cm:
            fn(*args, **kwargs)
        self.assertIn(f'{name} must be str', str(cm.exception))

    def test_topic_type_error(self):
        fn = lambda topic, value: None
        for topic in [None, 1, b'test/value', ['test/value']]:
            self._assert_type_error('topic', self.d.publish, topic, 1, timeout=0)
            self._assert_type_error('topic', self.d.query, topic)
            self._assert_type_error('topic', self.d.subscribe, topic, 'pub', fn)
            self._assert_type_error('topic', self.d.unsubscribe, topic, fn)

    def test_topic_type_error_message(self):
        with self.assertRaises(TypeError) as cm:
            self.d.query(b'@/list')
        self.assertEqual('topic must be str, not bytes', str(cm.exception))

    def test_publish_and_wait_type_error(self):
        self._assert_type_error('publish_topic', self.d.publish_and_wait,
                                1, 1, 'test/rsp', timeout=0.01)
        self._assert_type_error('response_topic', self.d.publish_and_wait,
                                'test/req', 1, None, timeout=0.01)

    def test_buffer_request_rsp_topic_type_error(self):
        self._assert_type_error('rsp_topic', self.d.publish, 'm/001/s/001/!req',
                                {'rsp_topic': 1}, timeout=0)

    def test_device_prefix_type_error(self):
        for device_prefix in [None, 1, b'u/js320/1']:
            self._assert_type_error('device_prefix', self.d.open, device_prefix)
            self._assert_type_error('device_prefix', self.d.close, device_prefix)

    def test_device_prefix_empty(self):
        for device_prefix in ['', '/', '//']:
            with self.assertRaises(ValueError):
                self.d.open(device_prefix)
            with self.assertRaises(ValueError):
                self.d.close(device_prefix)
