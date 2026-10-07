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

"""Test the command line argument errors."""

import contextlib
import io
import unittest
from pyjoulescope_driver.__main__ import run


class TestMain(unittest.TestCase):

    def run_error(self, args):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
            run(args)
        self.assertEqual(2, cm.exception.code)
        return err.getvalue()

    def test_subcommand_unrecognized(self):
        err = self.run_error(['info', '-v'])
        self.assertRegex(err, r'usage: \S.* info \[-h\]')
        self.assertIn('info: error: unrecognized arguments: -v', err)

    def test_subcommand_invalid_value(self):
        err = self.run_error(['scan', '--brand', 'acme'])
        self.assertRegex(err, r'usage: \S.* scan \[-h\]')
        self.assertIn('Unsupported brand "acme"', err)

    def test_top_level_unrecognized(self):
        err = self.run_error(['--nope'])
        self.assertIn('error: unrecognized arguments: --nope', err)
        self.assertIn('{api_timeout,', err)

    def test_unknown_command(self):
        err = self.run_error(['nope'])
        self.assertIn("invalid choice: 'nope'", err)
