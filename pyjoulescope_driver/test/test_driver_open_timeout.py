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

"""Test the Driver.open timeout, using a device path that does not exist."""

import time
import unittest
from pyjoulescope_driver import Driver


MISSING = 'u/js320/ZZZZ'


class TestDriverOpenTimeout(unittest.TestCase):

    def _open_duration(self, **kwargs):
        with Driver() as d:
            t = time.monotonic()
            with self.assertRaises(TimeoutError):
                d.open(MISSING, **kwargs)
            return time.monotonic() - t

    def test_default_open_timeout_is_3_seconds(self):
        # A JS320 that recovers from a host that exited without closing can
        # need more than 1 second to open, so the default is longer than
        # the 1 second default for other operations.
        duration = self._open_duration()
        self.assertGreater(duration, 2.8)
        self.assertLess(duration, 4.0)

    def test_explicit_open_timeout(self):
        duration = self._open_duration(timeout=0.25)
        self.assertLess(duration, 0.9)


if __name__ == '__main__':
    unittest.main()
