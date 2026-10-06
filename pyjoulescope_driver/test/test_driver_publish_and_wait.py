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

"""Test Driver.publish_and_wait with a local responder, no device."""

import unittest
from pyjoulescope_driver import Driver


REQ = 'test/!req'
RSP = 'test/!rsp'


class TestDriverPublishAndWait(unittest.TestCase):

    def setUp(self):
        self.d = Driver()

    def tearDown(self):
        self.d.unsubscribe(REQ, self._on_req)
        self.d.finalize()

    def _on_req(self, topic, value):
        # Respond for another transaction first, then for this one.
        self.d.publish(RSP, value + 100, timeout=0)
        self.d.publish(RSP, value, timeout=0)

    def _start(self):
        self.d.subscribe(REQ, 'pub', self._on_req)

    def test_first_response(self):
        self._start()
        self.assertEqual(101, self.d.publish_and_wait(REQ, 1, RSP, timeout=1.0))

    def test_match(self):
        self._start()
        rsp = self.d.publish_and_wait(REQ, 2, RSP, timeout=1.0, match=lambda v: v == 2)
        self.assertEqual(2, rsp)

    def test_match_none_times_out(self):
        self._start()
        with self.assertRaises(TimeoutError):
            self.d.publish_and_wait(REQ, 3, RSP, timeout=0.2, match=lambda v: False)
