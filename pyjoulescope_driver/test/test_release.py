# Copyright 2022 Jetperch LLC
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
import pyjoulescope_driver.release
import os
import shutil
import tempfile


MY_PATH = os.path.dirname(os.path.abspath(__file__))
RELEASE_PATH = os.path.join(MY_PATH, 'release')


class TestRelease(unittest.TestCase):

    def setUp(self):
        self._path = tempfile.mkdtemp(prefix='joulescope_driver')
        self._release_path = pyjoulescope_driver.release.release_path
        self._url_save = pyjoulescope_driver.release.url_save
        pyjoulescope_driver.release.release_path = lambda: self._path
        pyjoulescope_driver.release.url_save = self.url_save

    def tearDown(self):
        pyjoulescope_driver.release.url_save = self._url_save
        pyjoulescope_driver.release.release_path = self._release_path
        shutil.rmtree(self._path, ignore_errors=True)

    def url_save(self, url, filename):
        src = os.path.join(RELEASE_PATH, url)
        shutil.copy(src, filename)

    def test_program_path(self):
        path = self._release_path()
        self.assertTrue(os.path.isdir(path))
        self.assertEqual(self._path, pyjoulescope_driver.release.release_path())

    def test_release_get(self):
        r = pyjoulescope_driver.release.release_get('stable')
        self.assertTrue(r.startswith(b'joulescope_img_section'))
