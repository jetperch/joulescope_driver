#!/usr/bin/env python3
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

"""Hardware-in-the-loop test for the JS320 measurement examples.

Runs the ``statistics`` and ``record`` Python entry points and the
``jsdrv capture`` example against a physically connected JS320, and
checks that the measurements make sense.  See doc/plans/completed/js320_fix.md.

This test requires real hardware and a DUT that draws current, so it is
NOT part of the unit-test (ctest) suite::

    JSDRV_HW_DEVICE=u/js320/31NB python test/hw/test_examples_js320.py

Environment variables:
  * JSDRV_HW_DEVICE: the JS320 device path.  Defaults to the first JS320.
  * JSDRV_HW_MIN_CURRENT: the minimum mean DUT current in A, default 1e-7.
    The JS320 reads about 1e-11 A when the current range is off.
  * JSDRV_EXE: the jsdrv executable.  Defaults to the cmake-build
    Debug or Release build.

Run from the repository root with ``PYTHONPATH`` set to the repository,
and rebuild the binding (``python setup.py build_ext --inplace``) after
changing the C sources.
"""

import math
import os
import subprocess
import sys
import tempfile

import numpy as np

from pyjoulescope_driver import Driver


_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_EXE_DEFAULTS = [
    os.path.join(_ROOT, 'cmake-build', 'example', 'Debug', 'jsdrv.exe'),
    os.path.join(_ROOT, 'cmake-build', 'example', 'Release', 'jsdrv.exe'),
    os.path.join(_ROOT, 'cmake-build', 'example', 'jsdrv'),
]
_TIMEOUT_S = 60
_COUNT_TOLERANCE = 0.01   # current and voltage sample counts differ <= 1%


class Failure(Exception):
    pass


def _check(cond, msg):
    if not cond:
        raise Failure(msg)
    print(f'    ok: {msg}')


def _min_current():
    return float(os.environ.get('JSDRV_HW_MIN_CURRENT', '1e-7'))


def _check_current(i_mean, label):
    _check(math.isfinite(i_mean), f'{label} current is finite ({i_mean})')
    _check(abs(i_mean) >= _min_current(),
           f'{label} current {i_mean:.3g} A >= {_min_current():.3g} A')


def _check_counts(n_i, n_v, label):
    _check(n_i > 0 and n_v > 0, f'{label} has samples (i={n_i}, v={n_v})')
    diff = abs(n_i - n_v) / max(n_i, n_v)
    _check(diff <= _COUNT_TOLERANCE,
           f'{label} current and voltage counts differ by {diff * 100:.3f}%')


def _run(cmd):
    env = dict(os.environ)
    env['PYTHONPATH'] = _ROOT + os.pathsep + env.get('PYTHONPATH', '')
    p = subprocess.run(cmd, cwd=_ROOT, env=env, capture_output=True, text=True,
                       timeout=_TIMEOUT_S)
    if p.returncode:
        print(p.stdout)
        print(p.stderr)
        raise Failure(f'{" ".join(cmd)} returned {p.returncode}')
    return p.stdout


def _jsdrv_exe():
    exe = os.environ.get('JSDRV_EXE')
    if exe is not None:
        return exe
    for exe in _EXE_DEFAULTS:
        if os.path.isfile(exe):
            return exe
    raise Failure('jsdrv executable not found: build jsdrv_exe or set JSDRV_EXE')


def test_statistics(dev):
    print('test: python -m pyjoulescope_driver statistics')
    stdout = _run([sys.executable, '-m', 'pyjoulescope_driver', 'statistics',
                   '--duration', '2.5', '--device', dev])
    rows = [line.split(',') for line in stdout.splitlines()
            if line.startswith(dev + ',')]
    _check(len(rows) >= 2, f'{len(rows)} statistics rows')
    i_avg = np.array([float(row[2]) for row in rows])
    v_avg = np.array([float(row[6]) for row in rows])
    _check(bool(np.all(np.isfinite(v_avg))), 'voltage is finite')
    _check_current(float(np.mean(i_avg)), 'statistics')


def test_record(dev, tmpdir):
    print('test: python -m pyjoulescope_driver record')
    from pyjls import Reader, SignalType
    path = os.path.join(tmpdir, 'record.jls')
    _run([sys.executable, '-m', 'pyjoulescope_driver', 'record',
          '--duration', '1', '--device', dev, path])
    data = {}
    with Reader(path) as r:
        for s in r.signals.values():
            if s.signal_type == SignalType.FSR and s.length:
                data[s.name] = r.fsr(s.signal_id, 0, s.length)
    _check('current' in data and 'voltage' in data,
           f'record has current and voltage: {sorted(data.keys())}')
    i, v = data['current'], data['voltage']
    _check(bool(np.all(np.isfinite(i))), 'record current samples are finite')
    _check_current(float(np.mean(i)), 'record')
    _check_counts(len(i), len(v), 'record')


def test_capture(dev, tmpdir):
    print('test: jsdrv capture')
    path_i = os.path.join(tmpdir, 'capture_i.f32')
    path_v = os.path.join(tmpdir, 'capture_v.f32')
    _run([_jsdrv_exe(), 'capture', '--device', dev, '--duration', '1000',
          '--current', path_i, '--voltage', path_v])
    i = np.fromfile(path_i, dtype=np.float32)
    v = np.fromfile(path_v, dtype=np.float32)
    _check(len(i) > 0, f'capture current has {len(i)} samples')
    _check(bool(np.all(np.isfinite(i))), 'capture current samples are finite')
    _check_current(float(np.mean(i)), 'capture')
    _check_counts(len(i), len(v), 'capture')


def main():
    dev = os.environ.get('JSDRV_HW_DEVICE')
    if dev is None:
        with Driver() as d:
            paths = [p for p in d.device_paths() if '/js320/' in p]
        if not paths:
            print('SKIP: no JS320 connected (set JSDRV_HW_DEVICE)')
            return 0
        dev = paths[0]
    print(f'device: {dev}\n')
    failures = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        tests = [
            lambda: test_statistics(dev),
            lambda: test_record(dev, tmpdir),
            lambda: test_capture(dev, tmpdir),
        ]
        for t in tests:
            try:
                t()
            except Failure as ex:
                failures += 1
                print(f'    FAIL: {ex}')
            except Exception as ex:   # pragma: no cover - hardware faults
                failures += 1
                print(f'    ERROR: {ex!r}')
            print()
    if failures:
        print(f'FAILED ({failures} test(s))')
        return 1
    print('PASSED (all hardware-in-the-loop checks)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
