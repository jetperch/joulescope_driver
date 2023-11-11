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

import unittest
from pyjoulescope_driver import calibration_hash
import numpy as np


class TestCalibrationHash(unittest.TestCase):

    def test_msg1(self):
        expect = np.array([
            0xf81b307a, 0xb97c67b5, 0x360014be, 0x300cb7ee,
            0x77b58eb7, 0xbef6080a, 0xb58174b2, 0x0d13d633,
            0x1e0c2be3, 0xd6341a0e, 0xcef65998, 0xee02a2a4,
            0xbbb5a4c3, 0x43a9bef2, 0x02157487, 0x04eb2736],
            dtype=np.uint32)
        msg = np.array([
            0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
            0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
            0x23222120, 0x27262524, 0x2b2a2928, 0x2f2e2d2c,
            0x33323130, 0x37363534, 0x3b3a3938, 0x3f3e3d3c],
            dtype=np.uint32)
        result = calibration_hash(msg.view(dtype=np.uint8))
        np.testing.assert_equal(expect, result)

    def test_msg2(self):
        expect = np.array([
            0xdde25ff8, 0xa97fde9e, 0x0592b620, 0x89edf047,
            0xd742aeb4, 0x28061e26, 0x94a04227, 0x315bb9b2,
            0xbf454734, 0x5e44f37f, 0x1427eba8, 0xdcdbe142,
            0xefce213f, 0x46e2d4a6, 0xb69de141, 0xbb7adc7e],
            dtype=np.uint32)
        msg = np.array([
            0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
            0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
            0x23222120, 0x27262524, 0x2b2a2928, 0x2f2e2d2c,
            0x33323130, 0x37363534, 0x3b3a3938, 0x3f3e3d3c,
            0x43424140, 0x47464544, 0x4b4a4948, 0x4f4e4d4c,
            0x53525150, 0x57565554, 0x5b5a5958, 0x5f5e5d5c,
            0x63626160, 0x67666564, 0x6b6a6968, 0x6f6e6d6c,
            0x73727170, 0x77767574, 0x7b7a7978, 0x7f7e7d7c],
            dtype=np.uint32)
        result = calibration_hash(msg.view(dtype=np.uint8))
        np.testing.assert_equal(expect, result)
