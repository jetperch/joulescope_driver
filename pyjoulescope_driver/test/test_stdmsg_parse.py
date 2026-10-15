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

"""Test the stdmsg values that the binding decodes for subscribers."""

import struct
import unittest
from pyjoulescope_driver import Driver, StdMsg
from pyjoulescope_driver.mem_client import STDMSG_HDR


TOPIC = 'test/map'
MB_STDMSG_TIMESYNC_MAP = 0x04
# mb_timesync_map_v1_s
TIMESYNC_MAP = struct.Struct('<qQQQQQqqII')


class TestStdMsgParse(unittest.TestCase):

    def setUp(self):
        self.d = Driver()
        self.seen = []
        self.d.subscribe(TOPIC, 'pub', self._on_pub)

    def tearDown(self):
        self.d.finalize()

    def _on_pub(self, topic, value):
        self.seen.append(value)

    def _publish(self, body):
        # no responder for a local topic, so publish asynchronously and wait for the frontend
        self.d.publish(TOPIC, StdMsg(body), timeout=0)
        self.d.query('@/list')

    def test_timesync_map(self):
        body = TIMESYNC_MAP.pack(1000, 2000, 16_000_000 << 32, 25_000, 2010, 2020, 990, 995, 7, 0)
        self._publish(STDMSG_HDR.pack(0x0102, MB_STDMSG_TIMESYNC_MAP, ord('s'), 0) + body)
        self.assertEqual(1, len(self.seen))
        v = self.seen[0]
        self.assertEqual('stdmsg', v['dtype'])
        self.assertEqual('timesync_map', v['stdmsg_type'])
        self.assertEqual(1000, v['utc'])
        self.assertEqual(2000, v['counter'])
        self.assertEqual(16_000_000.0, v['counter_rate'])
        self.assertEqual(25_000, v['counter_ppb'])
        self.assertEqual(2010, v['sync_count_start'])
        self.assertEqual(2020, v['sync_count_end'])
        self.assertEqual(990, v['sync_utc_recv'])
        self.assertEqual(995, v['sync_utc_send'])
        self.assertEqual(7, v['update_counter'])

    def test_timesync_map_short(self):
        self._publish(STDMSG_HDR.pack(0, MB_STDMSG_TIMESYNC_MAP, ord('s'), 0) + bytes(8))
        self.assertEqual([None], self.seen)

    def test_unknown_type_keeps_header(self):
        self._publish(STDMSG_HDR.pack(0x0102, 0x55, ord('c'), 9) + bytes(16))
        self.assertEqual(1, len(self.seen))
        self.assertEqual({'dtype': 'stdmsg', 'version': 0x0102, 'version_major': 2, 'version_minor': 1,
                          'metadata': 9}, self.seen[0])
