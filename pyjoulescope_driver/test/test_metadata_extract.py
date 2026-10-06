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

"""
Test the host-side metadata extractor.
"""

import os
import tempfile
import unittest
from pyjoulescope_driver import metadata_extract
from pyjoulescope_driver.metadata_extract import ExtractError, extract, extract_text


_TABLE = r'''
#include "jsdrv_prv/frontend.h"

// const struct jsdrvp_param_s commented_out[] = {
const struct jsdrvp_param_s my_params[] = {
    {
        .topic = "h/a",
        .meta = "{"
            "\"dtype\": \"u32\","  // trailing comment with "quotes"
            /* block comment */
            "\"brief\": \"Path a//b and \\\\ backslash.\""
        "}",
    },
    {.topic = "m/{buf}/s/{sig}/b", .meta = "{\"brief\": \"b\"}"},
    {.topic = NULL, .meta = NULL}  // end of list
};
'''


def _table(entry):
    return 'const struct jsdrvp_param_s t[] = {\n' + entry + '\n    {.topic = NULL, .meta = NULL}\n};\n'


class TestMetadataExtract(unittest.TestCase):

    def test_extract_text(self):
        tables = extract_text(_TABLE)
        self.assertEqual(['my_params'], list(tables.keys()))
        self.assertEqual({
            'h/a': {'dtype': 'u32', 'brief': 'Path a//b and \\ backslash.'},
            'm/{buf}/s/{sig}/b': {'brief': 'b'},
        }, tables['my_params'])

    def test_invalid_json(self):
        with self.assertRaisesRegex(ExtractError, 'invalid JSON'):
            extract_text(_table('{.topic = "h/a", .meta = "{\\"brief\\": }"},'))

    def test_meta_not_object(self):
        with self.assertRaisesRegex(ExtractError, 'JSON object'):
            extract_text(_table('{.topic = "h/a", .meta = "[1]"},'))

    def test_duplicate_topic(self):
        entry = '{.topic = "h/a", .meta = "{}"},\n'
        with self.assertRaisesRegex(ExtractError, 'duplicate'):
            extract_text(_table(entry + entry))

    def test_invalid_placeholder(self):
        with self.assertRaisesRegex(ExtractError, 'placeholder'):
            extract_text(_table('{.topic = "m/{bad}/a", .meta = "{}"},'))

    def test_non_literal_meta(self):
        with self.assertRaisesRegex(ExtractError, 'string literal'):
            extract_text(_table('{.topic = "h/a", .meta = some_meta},'))

    def test_extract_tree_device_and_driver(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'devices', 'jsx'))
            with open(os.path.join(d, 'devices', 'jsx', 'jsx_params.c'), 'w') as f:
                f.write(_table('{.topic = "h/a", .meta = "{}"},'))
            with open(os.path.join(d, 'drv_params.c'), 'w') as f:
                f.write(_table('{.topic = "m/@/b", .meta = "{}"},'))
            with open(os.path.join(d, 'other.c'), 'w') as f:
                f.write(_table('{.topic = "ignored", .meta = "{}"},'))
            self.assertEqual({'devices': {'jsx': {'h/a': {}}}, 'driver': {'m/@/b': {}}}, extract(d))

    @unittest.skipUnless(os.path.isdir(metadata_extract.SRC_PATH), 'requires C source tree')
    def test_cache_is_fresh(self):
        expect = metadata_extract.to_json(extract())
        with open(metadata_extract.CACHE_PATH, 'r', encoding='utf-8') as f:
            actual = f.read()
        self.assertEqual(expect, actual,
                         'host_params.json is stale: python -m pyjoulescope_driver.metadata_extract --write')

    def test_host_metadata_js320(self):
        device, driver = metadata_extract.host_metadata('JS320')
        self.assertEqual({'h/fs', 'h/fp', 'h/i_scale', 'h/v_scale'}, set(device.keys()))
        self.assertIn('m/@/!add', driver)
        self.assertIn('m/{buf}/g/size', driver)
        self.assertIn('m/{buf}/s/{sig}/!req', driver)

    def test_host_metadata_unknown_model(self):
        device, driver = metadata_extract.host_metadata('js999')
        self.assertEqual({}, device)
        self.assertIn('m/@/!add', driver)
