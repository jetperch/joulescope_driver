# Copyright 2026 Jetperch LLC
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

"""Test the Driver subscriber lifetime.

The C library holds a borrowed pointer to each callback.  Python creates
a new bound method object on each attribute access, so unsubscribe must
find the object passed to subscribe, and must hold it until C can no
longer call it.  Otherwise C calls a freed object.
"""

import gc
import unittest
import weakref
from pyjoulescope_driver import Driver


TOPIC = 'test/value'


class Counter:

    def __init__(self):
        self.count = 0

    def on_pub(self, topic, value):
        self.count += 1


class TestDriverSubscribe(unittest.TestCase):

    def setUp(self):
        self.d = Driver()

    def tearDown(self):
        self.d.finalize()

    def _publish(self, value):
        self.d.publish(TOPIC, value, timeout=0)
        self.d.query('@/list')  # wait for the frontend to process the publish

    def _assert_released(self, ref):
        gc.collect()
        self.assertIsNone(ref())

    def test_unsubscribe_new_bound_method(self):
        c = Counter()
        ref = weakref.ref(c)
        self.d.subscribe(TOPIC, 'pub', c.on_pub)
        self._publish(1)
        self.assertEqual(1, c.count)
        self.d.unsubscribe(TOPIC, c.on_pub)  # a new, equal bound method
        self._publish(2)
        self.assertEqual(1, c.count)
        del c
        self._assert_released(ref)

    def test_unsubscribe_all_new_bound_method(self):
        c = Counter()
        ref = weakref.ref(c)
        self.d.subscribe(TOPIC, 'pub', c.on_pub)
        self.d.subscribe(TOPIC + '2', 'pub', c.on_pub)
        self.d.unsubscribe_all(c.on_pub)
        self._publish(1)
        self.assertEqual(0, c.count)
        del c
        self._assert_released(ref)

    def test_unsubscribe_async_holds_until_confirmed(self):
        c = Counter()
        ref = weakref.ref(c)
        self.d.subscribe(TOPIC, 'pub', c.on_pub)
        self.d.unsubscribe(TOPIC, c.on_pub, timeout=0)
        del c
        gc.collect()
        self.assertIsNotNone(ref())  # C may still call it

        c2 = Counter()
        self.d.subscribe(TOPIC, 'pub', c2.on_pub)
        self.d.unsubscribe(TOPIC, c2.on_pub)  # confirms the earlier one, too
        self._assert_released(ref)

    def test_unsubscribe_one_of_two(self):
        c1, c2 = Counter(), Counter()
        self.d.subscribe(TOPIC, 'pub', c1.on_pub)
        self.d.subscribe(TOPIC, 'pub', c2.on_pub)
        self.d.unsubscribe(TOPIC, c1.on_pub)
        self._publish(1)
        self.assertEqual(0, c1.count)
        self.assertEqual(1, c2.count)

    def test_finalize_twice(self):
        self.d.finalize()
        self.d.finalize()
