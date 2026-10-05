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

"""Extract host-side topic metadata from the C jsdrvp_param_s tables.

The tables in src/**/*_params.c are the single source of truth.  See
jsdrvp_param_s in include_private/jsdrv_prv/frontend.h for the format.
This module parses them into host_params.json, which ships with the
package so tools can document topics without a device or the C source.

Regenerate after editing a table:

    python -m pyjoulescope_driver.metadata_extract --write
"""

import argparse
import json
import os
import re
import sys


MYPATH = os.path.dirname(os.path.abspath(__file__))
SRC_PATH = os.path.join(os.path.dirname(MYPATH), 'src')
CACHE_PATH = os.path.join(MYPATH, 'host_params.json')
_TABLE_START = [('word', 'const'), ('word', 'struct'), ('word', 'jsdrvp_param_s')]
_WORD = re.compile(r'\w+')
_ESCAPES = {'"': '"', '\\': '\\', 'n': '\n', 't': '\t', "'": "'"}
_PLACEHOLDERS = re.compile(r'\{(\w+)\}')
_PLACEHOLDERS_VALID = {'buf', 'sig'}


class ExtractError(ValueError):
    pass


def _tokenize(text, path):
    """Tokenize C source into (kind, value) with kind in 'str', 'word', 'punct'.

    Comments are dropped and adjacent string literals are joined.
    """
    tokens = []
    idx = 0
    n = len(text)
    while idx < n:
        c = text[idx]
        if c.isspace():
            idx += 1
        elif text.startswith('//', idx):
            idx = text.find('\n', idx)
            idx = n if idx < 0 else idx
        elif text.startswith('/*', idx):
            end = text.find('*/', idx + 2)
            if end < 0:
                raise ExtractError(f'{path}: unterminated comment')
            idx = end + 2
        elif c == '"':
            idx += 1
            value = []
            while True:
                if idx >= n or text[idx] == '\n':
                    raise ExtractError(f'{path}: unterminated string literal')
                c = text[idx]
                if c == '"':
                    idx += 1
                    break
                if c == '\\':
                    e = text[idx + 1]
                    if e not in _ESCAPES:
                        raise ExtractError(f'{path}: unsupported escape \\{e}')
                    value.append(_ESCAPES[e])
                    idx += 2
                else:
                    value.append(c)
                    idx += 1
            value = ''.join(value)
            if tokens and tokens[-1][0] == 'str':
                tokens[-1] = ('str', tokens[-1][1] + value)
            else:
                tokens.append(('str', value))
        elif c.isalnum() or c == '_':
            m = _WORD.match(text, idx)
            tokens.append(('word', m.group(0)))
            idx = m.end()
        else:
            tokens.append(('punct', c))
            idx += 1
    return tokens


def _expect(tokens, idx, kind, value=None, path=''):
    if idx >= len(tokens):
        raise ExtractError(f'{path}: unexpected end of file')
    k, v = tokens[idx]
    if k != kind or (value is not None and v != value):
        raise ExtractError(f'{path}: expected {value or kind}, found {v!r}')
    return v


def _parse_field(tokens, idx, name, path):
    """Parse '.name = "literal"' or '.name = NULL'."""
    _expect(tokens, idx, 'punct', '.', path)
    _expect(tokens, idx + 1, 'word', name, path)
    _expect(tokens, idx + 2, 'punct', '=', path)
    k, v = tokens[idx + 3]
    if k == 'word' and v == 'NULL':
        return None, idx + 4
    if k != 'str':
        raise ExtractError(f'{path}: .{name} must be a string literal or NULL, found {v!r}')
    return v, idx + 4


def _parse_table(tokens, idx, name, path):
    """Parse table entries starting after the opening '{'."""
    entries = {}
    while True:
        _expect(tokens, idx, 'punct', '{', path)
        topic, idx = _parse_field(tokens, idx + 1, 'topic', path)
        _expect(tokens, idx, 'punct', ',', path)
        meta, idx = _parse_field(tokens, idx + 1, 'meta', path)
        if tokens[idx] == ('punct', ','):
            idx += 1
        _expect(tokens, idx, 'punct', '}', path)
        idx += 1
        if topic is None:
            if meta is not None:
                raise ExtractError(f'{path}: {name} sentinel must have NULL meta')
            break
        if topic in entries:
            raise ExtractError(f'{path}: {name} duplicate topic {topic}')
        for placeholder in _PLACEHOLDERS.findall(topic):
            if placeholder not in _PLACEHOLDERS_VALID:
                raise ExtractError(f'{path}: {name} {topic} has invalid placeholder {{{placeholder}}}')
        try:
            value = json.loads(meta)
        except json.JSONDecodeError as ex:
            raise ExtractError(f'{path}: {name} {topic} meta is invalid JSON: {ex}')
        if not isinstance(value, dict):
            raise ExtractError(f'{path}: {name} {topic} meta must be a JSON object')
        entries[topic] = value
        _expect(tokens, idx, 'punct', ',', path)
        idx += 1
    if tokens[idx] == ('punct', ','):
        idx += 1
    _expect(tokens, idx, 'punct', '}', path)
    _expect(tokens, idx + 1, 'punct', ';', path)
    return entries


def extract_text(text, path=''):
    """Extract all param tables from C source text.

    :param text: The C source text.
    :param path: The source path for error messages.
    :return: The dict mapping table name to {topic: meta}.
    :raise ExtractError: On a table that does not follow the format.
    """
    tables = {}
    tokens = _tokenize(text, path)
    for idx in range(len(tokens) - len(_TABLE_START)):
        if tokens[idx:idx + len(_TABLE_START)] != _TABLE_START:
            continue
        idx += len(_TABLE_START)
        name = _expect(tokens, idx, 'word', path=path)
        for offset, c in enumerate('[]={'):
            _expect(tokens, idx + 1 + offset, 'punct', c, path)
        tables[name] = _parse_table(tokens, idx + 5, name, path)
    return tables


def extract(src_path=None):
    """Extract the host-side metadata from the C source tree.

    Tables in src/devices/<model>/ apply to that device model, and are
    published under the device path.  Other tables are driver-level and
    published at the root topic.

    :param src_path: The joulescope_driver src directory.
    :return: {"devices": {model: {topic: meta}}, "driver": {topic: meta}}.
    """
    src_path = SRC_PATH if src_path is None else src_path
    result = {'devices': {}, 'driver': {}}
    for root, dirs, files in os.walk(src_path):
        dirs.sort()
        for fname in sorted(files):
            if not fname.endswith('_params.c'):
                continue
            path = os.path.join(root, fname)
            with open(path, 'r', encoding='utf-8') as f:
                tables = extract_text(f.read(), os.path.relpath(path, src_path))
            rel = os.path.relpath(root, src_path).replace(os.sep, '/').split('/')
            if len(rel) == 2 and rel[0] == 'devices':
                target = result['devices'].setdefault(rel[1], {})
            else:
                target = result['driver']
            for entries in tables.values():
                for topic, meta in entries.items():
                    if topic in target:
                        raise ExtractError(f'{path}: duplicate topic {topic}')
                    target[topic] = meta
    return result


def to_json(data):
    return json.dumps(data, indent=2) + '\n'


def load():
    """Load the packaged host metadata cache."""
    with open(CACHE_PATH, 'r', encoding='utf-8') as f:
        return json.load(f)


def host_metadata(model):
    """Get the host-side metadata for a device model.

    :param model: The device model, such as 'js320'.
    :return: The tuple (device, driver) of {topic: meta} dicts.  Device
        topics are relative to the device path.  Driver topics are global.
    """
    data = load()
    return dict(data['devices'].get(model.lower(), {})), dict(data['driver'])


def run():
    p = argparse.ArgumentParser(description='Extract host-side topic metadata from the C param tables.')
    p.add_argument('--src', help='The joulescope_driver src directory.')
    g = p.add_mutually_exclusive_group()
    g.add_argument('--write', action='store_true', help=f'Update {os.path.basename(CACHE_PATH)}.')
    g.add_argument('--check', action='store_true', help='Fail if the cache is stale.')
    args = p.parse_args()
    try:
        out = to_json(extract(args.src))
    except ExtractError as ex:
        print(f'ERROR: {ex}', file=sys.stderr)
        return 1
    if args.write:
        with open(CACHE_PATH, 'w', encoding='utf-8', newline='\n') as f:
            f.write(out)
    elif args.check:
        with open(CACHE_PATH, 'r', encoding='utf-8') as f:
            if f.read() != out:
                print(f'ERROR: {CACHE_PATH} is stale, run with --write', file=sys.stderr)
                return 1
    else:
        print(out, end='')
    return 0


if __name__ == '__main__':
    sys.exit(run())
