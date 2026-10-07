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

"""Test the statistics 128-bit accumulator conversion."""

import unittest
from pyjoulescope_driver.binding import _i128_to_int, _STATISTICS_I128_SCALE

U64 = (1 << 64) - 1


class TestI128(unittest.TestCase):

    def test_scale(self):
        self.assertEqual(2 ** -52, _STATISTICS_I128_SCALE)

    def test_positive(self):
        self.assertEqual(0, _i128_to_int(0, 0))
        self.assertEqual(5, _i128_to_int(0, 5))
        self.assertEqual(1 << 63, _i128_to_int(0, 1 << 63))  # bit 63 is not the sign
        self.assertEqual(U64, _i128_to_int(0, U64))
        self.assertEqual(1 << 64, _i128_to_int(1, 0))
        self.assertEqual((1 << 127) - 1, _i128_to_int((1 << 63) - 1, U64))

    def test_negative(self):
        self.assertEqual(-1, _i128_to_int(U64, U64))
        self.assertEqual(-2, _i128_to_int(U64, U64 - 1))
        self.assertEqual(-(1 << 64), _i128_to_int(U64, 0))
        self.assertEqual(-(1 << 127), _i128_to_int(1 << 63, 0))
        self.assertEqual(-(6 << 52), _i128_to_int(U64, (-(6 << 52)) & U64))

    def test_round_trip(self):
        for value in [0, 1, -1, 12345678901234567890, -12345678901234567890,
                      (1 << 100) + 3, -((1 << 100) + 3)]:
            words = value & ((1 << 128) - 1)
            self.assertEqual(value, _i128_to_int(words >> 64, words & U64), value)
