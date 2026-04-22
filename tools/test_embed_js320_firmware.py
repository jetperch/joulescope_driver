#!/usr/bin/env python3
# Copyright 2026 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Unit tests for tools/embed_js320_firmware.py.

Run from the repository root:
    python -m unittest tools.test_embed_js320_firmware
"""

import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import embed_js320_firmware as embed  # noqa: E402


def _index(size, digest, version=(0, 4, 3),
           path='js320-0_4_3.zip',
           changelog='js320-0_4_3_CHANGELOG.md',
           timestamp='2026-04-22T00:00:00Z'):
    return {
        'js320': {
            'stable': {
                'version': list(version),
                'path': path,
                'size': size,
                'sha256': digest,
                'changelog': changelog,
                'timestamp': timestamp,
            }
        }
    }


class _FakeHttp:
    """Stand-in for urllib.request.urlopen keyed by URL."""

    def __init__(self, responses):
        self._responses = dict(responses)
        self.requested = []

    def __call__(self, url, timeout=None):
        self.requested.append(url)
        if url not in self._responses:
            raise AssertionError(f'unexpected URL: {url}')
        return _FakeResponse(self._responses[url])


class _FakeResponse:
    def __init__(self, body):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


INDEX_URL = 'https://example.com/firmware/js320/index.json'
IMAGE_URL = 'https://example.com/firmware/js320/js320-0_4_3.zip'


class TestFetchIndex(unittest.TestCase):

    def test_fetch_and_parse(self):
        payload = _index(size=4, digest='deadbeef')
        http = _FakeHttp({INDEX_URL: json.dumps(payload).encode('utf-8')})
        with mock.patch('urllib.request.urlopen', http):
            got = embed.fetch_index(INDEX_URL)
        self.assertEqual(got['js320']['stable']['version'], [0, 4, 3])


class TestFetchImage(unittest.TestCase):

    def test_success(self):
        data = b'hello world firmware'
        digest = hashlib.sha256(data).hexdigest()
        entry = _index(len(data), digest)['js320']['stable']
        http = _FakeHttp({IMAGE_URL: data})
        with mock.patch('urllib.request.urlopen', http):
            got, url = embed.fetch_image(INDEX_URL, entry)
        self.assertEqual(got, data)
        self.assertEqual(url, IMAGE_URL)

    def test_sha256_mismatch_raises(self):
        data = b'bytes'
        entry = _index(len(data), 'wrong-digest')['js320']['stable']
        http = _FakeHttp({IMAGE_URL: data})
        with mock.patch('urllib.request.urlopen', http):
            with self.assertRaisesRegex(RuntimeError, 'sha256 mismatch'):
                embed.fetch_image(INDEX_URL, entry)

    def test_size_mismatch_raises(self):
        data = b'five!'
        entry = _index(999, hashlib.sha256(data).hexdigest())['js320']['stable']
        http = _FakeHttp({IMAGE_URL: data})
        with mock.patch('urllib.request.urlopen', http):
            with self.assertRaisesRegex(RuntimeError, 'size mismatch'):
                embed.fetch_image(INDEX_URL, entry)


class TestGenerateHeader(unittest.TestCase):

    def test_contains_version_and_size_macros(self):
        h = embed.generate_header((1, 2, 3), 42)
        self.assertIn('#define JS320_FIRMWARE_VERSION_MAJOR 1', h)
        self.assertIn('#define JS320_FIRMWARE_VERSION_MINOR 2', h)
        self.assertIn('#define JS320_FIRMWARE_VERSION_PATCH 3', h)
        self.assertIn('#define JS320_FIRMWARE_SIZE          42', h)
        self.assertIn('extern const uint8_t js320_firmware[];', h)
        self.assertIn('JSDRV_DEVICES_JS320_FIRMWARE_H_', h)
        self.assertIn('Apache License', h)

    def test_stub_header_renders_zero(self):
        h = embed.generate_header((0, 0, 0), 0)
        self.assertIn('#define JS320_FIRMWARE_SIZE          0', h)


class TestGenerateSource(unittest.TestCase):

    def test_byte_array_contents(self):
        data = bytes(range(20))  # 20 bytes -> 2 rows of 16 + 4
        c = embed.generate_source(data)
        self.assertIn('#include "jsdrv_prv/devices/js320/firmware.h"', c)
        self.assertIn('const uint8_t js320_firmware[JS320_FIRMWARE_SIZE]', c)
        self.assertIn('0x00, 0x01, 0x02', c)
        self.assertIn('0x12, 0x13,', c)  # 18, 19 on last line

    def test_empty_bytes(self):
        c = embed.generate_source(b'')
        self.assertIn('const uint8_t js320_firmware[JS320_FIRMWARE_SIZE] = {',
                      c)

    def test_16_bytes_per_line(self):
        data = bytes(range(32))
        c = embed.generate_source(data)
        # Count hex literals per line to confirm 16-per-line layout.
        data_lines = [line for line in c.splitlines()
                      if line.startswith('    0x')]
        self.assertEqual(len(data_lines), 2)
        self.assertEqual(data_lines[0].count('0x'), 16)
        self.assertEqual(data_lines[1].count('0x'), 16)


class TestWriteIfChanged(unittest.TestCase):

    def test_creates_new_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'sub', 'x.h')
            self.assertTrue(embed.write_if_changed(p, 'hello\n'))
            with open(p, 'r', encoding='utf-8') as f:
                self.assertEqual(f.read(), 'hello\n')

    def test_skips_write_when_content_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'x.h')
            embed.write_if_changed(p, 'hello\n')
            mtime = os.path.getmtime(p)
            self.assertFalse(embed.write_if_changed(p, 'hello\n'))
            self.assertEqual(os.path.getmtime(p), mtime)

    def test_rewrites_when_content_differs(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'x.h')
            embed.write_if_changed(p, 'a\n')
            self.assertTrue(embed.write_if_changed(p, 'b\n'))
            with open(p, 'r', encoding='utf-8') as f:
                self.assertEqual(f.read(), 'b\n')


class TestMainEndToEnd(unittest.TestCase):

    def test_dry_run_writes_nothing(self):
        data = b'abc'
        digest = hashlib.sha256(data).hexdigest()
        payload = _index(len(data), digest)
        http = _FakeHttp({
            INDEX_URL: json.dumps(payload).encode('utf-8'),
            IMAGE_URL: data,
        })
        with tempfile.TemporaryDirectory() as tmp:
            hpath = os.path.join(tmp, 'firmware.h')
            cpath = os.path.join(tmp, 'firmware.c')
            argv = ['embed', '--index-url', INDEX_URL,
                    '--output-h', hpath, '--output-c', cpath, '--dry-run']
            with mock.patch('urllib.request.urlopen', http), \
                 mock.patch.object(sys, 'argv', argv):
                rc = embed.main()
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(hpath))
            self.assertFalse(os.path.exists(cpath))

    def test_live_run_writes_both_files(self):
        data = b'\x01\x02\x03\x04\x05'
        digest = hashlib.sha256(data).hexdigest()
        payload = _index(len(data), digest, version=(1, 2, 3))
        http = _FakeHttp({
            INDEX_URL: json.dumps(payload).encode('utf-8'),
            IMAGE_URL: data,
        })
        with tempfile.TemporaryDirectory() as tmp:
            hpath = os.path.join(tmp, 'firmware.h')
            cpath = os.path.join(tmp, 'firmware.c')
            argv = ['embed', '--index-url', INDEX_URL,
                    '--output-h', hpath, '--output-c', cpath]
            with mock.patch('urllib.request.urlopen', http), \
                 mock.patch.object(sys, 'argv', argv):
                rc = embed.main()
            self.assertEqual(rc, 0)
            with open(hpath, 'r', encoding='utf-8') as f:
                h = f.read()
            with open(cpath, 'r', encoding='utf-8') as f:
                c = f.read()
            self.assertIn('#define JS320_FIRMWARE_VERSION_MAJOR 1', h)
            self.assertIn('#define JS320_FIRMWARE_VERSION_MINOR 2', h)
            self.assertIn('#define JS320_FIRMWARE_VERSION_PATCH 3', h)
            self.assertIn('#define JS320_FIRMWARE_SIZE          5', h)
            self.assertIn('0x01, 0x02, 0x03, 0x04, 0x05,', c)

    def test_missing_stable_entry_raises(self):
        http = _FakeHttp({
            INDEX_URL: json.dumps({'js320': {}}).encode('utf-8'),
        })
        argv = ['embed', '--index-url', INDEX_URL]
        with mock.patch('urllib.request.urlopen', http), \
             mock.patch.object(sys, 'argv', argv):
            with self.assertRaises(RuntimeError):
                embed.main()


if __name__ == '__main__':
    unittest.main()
