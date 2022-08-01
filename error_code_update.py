#!/usr/bin/env python3
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

"""
Update the error code files.
- include/jsdrv/error_code.h
- src/error_code.c
"""

import os
import re

MYPATH = os.path.dirname(os.path.abspath(__file__))
H_PATH = os.path.join(MYPATH, 'include', 'jsdrv', 'error_code.h')
C_PATH = os.path.join(MYPATH, 'src', 'error_code.c')
PYX_PATH = os.path.join(MYPATH, 'pyjoulescope_driver', 'binding.pyx')


def _insert(source_lines, indicator_start, indicator_end, insert_lines):
    out = []
    while len(source_lines):
        line = source_lines.pop(0)
        out.append(line)
        if line.strip() == indicator_start:
            while len(source_lines):
                line = source_lines.pop(0)
                if line.strip() == indicator_end:
                    out.extend(insert_lines)
                    out.append(line)
                    out.extend(source_lines)
                    return out
    raise RuntimeError('Insert failed')


def _update_h(ec):
    insert_lines = [f'    JSDRV_ERROR_{name} = {value},  ///< {description}\n' for value, name, description in ec]
    with open(H_PATH, 'rt') as f:
        lines = f.readlines()
    lines = _insert(lines, '// ENUM_ERROR_CODE_START', '// ENUM_ERROR_CODE_END', insert_lines)
    with open(H_PATH, 'wt') as f:
        f.write(''.join(lines))


def _update_c(ec):
    case_names = [f'        case JSDRV_ERROR_{name}: return "{name}";\n' for _, name, _ in ec]
    case_description = [f'        case JSDRV_ERROR_{name}: return "{description}";\n' for _, name, description in ec]
    with open(C_PATH, 'rt') as f:
        lines = f.readlines()
    lines = _insert(lines, '// CASE_NAME_START', '// CASE_NAME_END', case_names)
    lines = _insert(lines, '// CASE_DESCRIPTION_START', '// CASE_DESCRIPTION_END', case_description)
    with open(C_PATH, 'wt') as f:
        f.write(''.join(lines))


def _update_pyx(ec):
    enum_vals = [f'    {name:27s} = {value}\n' for value, name, _ in ec]
    meta1 = [f"    {value}: ({value}, '{name}', '{description}'),\n" for value, name, description in ec]
    meta2 = [f"    '{name}': ({value}, '{name}', '{description}'),\n" for value, name, description in ec]
    with open(PYX_PATH, 'rt') as f:
        lines = f.readlines()
    lines = _insert(lines, '# ENUM_ERROR_CODE_START', '# ENUM_ERROR_CODE_END', enum_vals)
    lines = _insert(lines, '# ENUM_ERROR_CODE_META_START', '# ENUM_ERROR_CODE_META_END', meta1 + meta2)
    with open(PYX_PATH, 'wt') as f:
        f.write(''.join(lines))


def _parse():
    error_codes = []
    pattern = re.compile(r'(\d+)\s+(\S+)\s+"([^"]+)"')
    with open(H_PATH, 'rt') as f:
        lines = [k.strip() for k in f.readlines()]
    while len(lines):
        line = lines.pop(0)
        if line == 'DEF_ERROR_CODE_START':
            break
    for line in lines:
        if line == 'DEF_ERROR_CODE_END':
            break
        m = pattern.match(line)
        if m is None:
            msg = f'PARSE FAILED: {line}'
            print(msg)
            raise RuntimeError(msg)
        error_codes.append([int(m.group(1)), m.group(2), m.group(3)])
    return error_codes


def run():
    ec = _parse()
    _update_h(ec)
    _update_c(ec)
    _update_pyx(ec)
    return 0


if __name__ == '__main__':
    run()
