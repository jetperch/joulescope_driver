#!/usr/bin/env python3
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

"""Copy minibitty headers to minibitty_host/include_private/mb/."""

import argparse
import os
import shutil
import sys

HEADERS = [
    'cdef.h',
    'event.h',
    'stdmsg.h',
    'topic.h',
    'value.h',
    'version.h',
    'comm/frame.h',
    'comm/link.h',
    'comm/link_sm.h',
    'state_machine.h',
]


def run():
    p = argparse.ArgumentParser(
        description='Copy minibitty headers to include_private/mb/')
    p.add_argument('minibitty_path',
                   help='Path to the minibitty repository root')
    args = p.parse_args()

    src_base = os.path.join(args.minibitty_path, 'include', 'mb')
    if not os.path.isdir(src_base):
        print(f'ERROR: {src_base} not found', file=sys.stderr)
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    dst_base = os.path.join(script_dir, '..', 'include_private', 'mb')
    dst_base = os.path.normpath(dst_base)

    for header in HEADERS:
        src = os.path.join(src_base, header)
        dst = os.path.join(dst_base, header)
        if not os.path.isfile(src):
            print(f'ERROR: missing {src}', file=sys.stderr)
            sys.exit(1)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f'  {header}')

    print('Done.')


if __name__ == '__main__':
    run()
