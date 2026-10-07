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
Test the metadata entry point.
"""

import html
import json
import os
import re
import tempfile
import unittest
import zipfile
from pyjoulescope_driver.entry_points import metadata


_DEVICE_PATH = 'u/js220/000000'

_META = {
    'h/state': {
        'dtype': 'u32',
        'brief': 'The current device state.',
        'options': [[0, 'not present'], [1, 'closed'], [2, 'opening', 'busy'], [3, 'open']],
        'flags': ['ro', 'hide'],
    },
    's/i/range/select': {
        'dtype': 'u8',
        'brief': 'The current range selection.',
        'detail': 'Longer <b>description</b> & explanation.',
        'default': 0,
        'format': 'version',
        'custom_key': 'custom_value',
    },
    's/ts/!req': None,  # devices publish null metadata for command topics
}


class DriverStub:
    """Emulate the retained metadata flush on subscribe."""

    def __init__(self, meta):
        self._meta = meta
        self.unsubscribed = False

    def subscribe(self, device_path, flags, fn):
        for subtopic, value in self._meta.items():
            fn(f'{device_path}/{subtopic}$', value)

    def unsubscribe(self, device_path, fn):
        self.unsubscribed = True


class TestFormatResolve(unittest.TestCase):

    def test_explicit_wins(self):
        self.assertEqual('yaml', metadata.format_resolve('yaml', 'x.html'))

    def test_extensions(self):
        for out, expect in [('x.json', 'json'), ('x.yaml', 'yaml'), ('x.yml', 'yaml'),
                            ('x.html', 'html'), ('x.htm', 'html')]:
            with self.subTest(out=out):
                self.assertEqual(expect, metadata.format_resolve(None, out))

    def test_extension_case_insensitive(self):
        self.assertEqual('json', metadata.format_resolve(None, 'x.JSON'))
        self.assertEqual('html', metadata.format_resolve(None, 'x.HTML'))

    def test_unknown_extension_defaults_json(self):
        self.assertEqual('json', metadata.format_resolve(None, 'x.txt'))
        self.assertEqual('json', metadata.format_resolve(None, 'x'))

    def test_no_out_defaults_json(self):
        self.assertEqual('json', metadata.format_resolve(None, None))


class TestMetadataLoad(unittest.TestCase):

    def test_load(self):
        d = DriverStub(_META)
        meta = metadata.metadata_load(d, _DEVICE_PATH)
        self.assertEqual(_META, meta)
        self.assertTrue(d.unsubscribed)


class TestToJson(unittest.TestCase):

    def test_round_trip(self):
        self.assertEqual(_META, json.loads(metadata.to_json(_META)))


class TestToYaml(unittest.TestCase):

    def test_round_trip(self):
        try:
            import yaml
        except ImportError:
            self.skipTest('pyyaml not installed')
        self.assertEqual(_META, yaml.safe_load(metadata.to_yaml(_META)))


class TestToHtml(unittest.TestCase):

    def setUp(self):
        self.html = metadata.to_html(_META, f'{_DEVICE_PATH} metadata')

    def test_topics_and_title(self):
        self.assertIn(f'{_DEVICE_PATH} metadata', self.html)
        for topic in _META.keys():
            self.assertIn(f'<td class="topic">{html.escape(topic)}</td>', self.html)

    def test_column_union_and_order(self):
        header = re.findall(r'<th[^>]*>([^<]+)</th>', self.html)
        self.assertEqual(['topic', 'dtype', 'brief', 'detail', 'default',
                          'options', 'format', 'flags', 'custom_key'], header)

    def test_escaping(self):
        self.assertNotIn('<b>description</b>', self.html)
        self.assertIn('Longer &lt;b&gt;description&lt;/b&gt; &amp; explanation.', self.html)

    def test_options_rendering(self):
        self.assertIn('0: not present', self.html)
        self.assertIn('2: opening (busy)', self.html)

    def test_flags_rendering(self):
        self.assertIn('ro, hide', self.html)

    def test_column_checkboxes(self):
        checkboxes = re.findall(r'<input type="checkbox" data-col="\d+"( checked)?>', self.html)
        self.assertEqual(8, len(checkboxes))

    def test_detail_hidden_by_default(self):
        columns = ['dtype', 'brief', 'detail', 'default', 'options', 'format', 'flags',
                   'custom_key']
        idx = columns.index('detail')
        self.assertIn(f'<th class="col-{idx} hide">detail</th>', self.html)
        self.assertNotIn(f'<input type="checkbox" data-col="{idx}" checked>', self.html)

    def test_self_contained(self):
        self.assertNotIn('http://', self.html.replace('http://www.apache.org', ''))
        self.assertNotIn('https://', self.html)
        self.assertNotIn('src=', self.html)



_FW_C = {
    'schema': {'name': 'MiniBitty.pubsub', 'version': '1.0.0'},
    'project': {'name': '__unknown__', 'version': '0.0.0', 'prefix': 'c'},
    'topics': {
        './comm/state': {'dtype': 'u8', 'brief': 'Link state.', 'flags': 'ro'},
        '././info': {'dtype': 'str', 'brief': 'Info.', 'flags': 'no_traverse'},
        './list': {'dtype': 'u8', 'brief': 'List.', 'flags': ['hide', 'ro', 'no_traverse']},
        './fw/version': {'dtype': 'u32', 'brief': 'Version.', 'default': 'MBDEF__VERSION_U32'},
        './name': {'dtype': 'str', 'brief': 'Name.', 'default': 'MBDEF__NAME'},
        './mode': {'dtype': 'u8', 'brief': 'Mode.', 'default': '2',
                   'options': [[0, 'off', 1, 'x'], [1, 'on']]},
    },
}

_FW_S = {
    'schema': {'name': 'MiniBitty.pubsub', 'version': '1.0.0'},
    'project': {'name': '__unknown__', 'version': '0.0.0', 'prefix': 's'},
    'topics': {
        './i/ctrl': {'dtype': 'bool', 'brief': 'Enable current.', 'default': 0},
    },
}


class TestFirmware(unittest.TestCase):

    def test_meta_to_device(self):
        meta = metadata.firmware_meta_to_device(_FW_C)
        self.assertEqual({
            'c/comm/state': {'dtype': 'u8', 'brief': 'Link state.', 'flags': ['ro']},
            'c/./info': {'dtype': 'str', 'brief': 'Info.'},
            'c/list': {'dtype': 'u8', 'brief': 'List.', 'flags': ['ro', 'hide']},
            'c/fw/version': {'dtype': 'u32', 'brief': 'Version.', 'default': 0},
            'c/name': {'dtype': 'str', 'brief': 'Name.', 'default': 'MBDEF__NAME'},
            'c/mode': {'dtype': 'u8', 'brief': 'Mode.', 'default': '2',
                       'options': [[0, 'off', '1', 'x'], [1, 'on']]},
        }, meta)
        self.assertEqual('ro', _FW_C['topics']['./comm/state']['flags'])  # input unmodified

    def test_load_json_and_zip(self):
        with tempfile.TemporaryDirectory() as d:
            json_path = os.path.join(d, 'pubsub_metadata.json')
            with open(json_path, 'w') as f:
                json.dump(_FW_C, f)
            zip_path = os.path.join(d, 'fw.zip')
            with zipfile.ZipFile(zip_path, 'w') as z:
                z.writestr('1/0/app/pubsub_metadata.json', json.dumps(_FW_S))
                z.writestr('1/0/app/pubsub_metadata.bin', b'ignored')
            meta = metadata.firmware_load([json_path, zip_path])
        self.assertEqual({'c/comm/state', 'c/./info', 'c/list', 'c/fw/version', 'c/name', 'c/mode',
                          's/i/ctrl'}, set(meta.keys()))

    def test_load_zip_without_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            zip_path = os.path.join(d, 'fw.zip')
            with zipfile.ZipFile(zip_path, 'w') as z:
                z.writestr('manifest.yaml', 'x')
            with self.assertRaisesRegex(ValueError, 'contains no'):
                metadata.firmware_load([zip_path])

    def test_offline_load_merges_host(self):
        with tempfile.TemporaryDirectory() as d:
            zip_path = os.path.join(d, 'fw.zip')
            with zipfile.ZipFile(zip_path, 'w') as z:
                z.writestr('0/0/app/pubsub_metadata.json', json.dumps(_FW_C))
                z.writestr('1/0/app/pubsub_metadata.json', json.dumps(_FW_S))
            meta = metadata.offline_load([zip_path], 'js320')
        for topic in ['c/comm/state', 's/i/ctrl', 'h/fs', 'm/@/!add', 'm/{buf}/g/size']:
            self.assertIn(topic, meta)


class TestMetaDiff(unittest.TestCase):

    def test_equal(self):
        self.assertEqual([], metadata.meta_diff(_META, json.loads(json.dumps(_META))))

    def test_differences(self):
        expected = {'a': {'dtype': 'u8'}, 'b': {'dtype': 'u8', 'brief': 'x'}, 'c': None}
        actual = {'b': {'dtype': 'u16', 'brief': 'x'}, 'c': None, 'd': {}}
        self.assertEqual([
            '- a',
            "~ b dtype: 'u16' != 'u8'",
            '+ d',
        ], metadata.meta_diff(actual, expected))


if __name__ == '__main__':
    unittest.main()
