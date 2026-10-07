#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2025 Jetperch LLC
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
"""Hardware-in-the-loop test for publish error reporting.

Validates against a physically connected JS320 that:

* A blocking publish raises for an invalid value or an unknown topic,
  every time, including when repeated.
* A publish with timeout=0 logs the device's error at ERROR level to the
  "jsdrv" logger, for host-side validation and device-side rejection.
* A rejected value does not remain retained by the host.
* Normal open, publish, query, streaming and close log no ERROR.

This test requires real hardware and is therefore NOT part of the
unit-test suite::

    JSDRV_HW_DEVICE=u/js320/8W2A python test/hw/test_publish_errors_js320.py

If JSDRV_HW_DEVICE is unset, the first JS320 is used.  Rebuild the binding
(``python setup.py build_ext --inplace``) after changing the C sources.
"""

import logging
import os
import sys
import time

from pyjoulescope_driver import Driver


UNKNOWN = 's/nope/xyz'      # not a JS320 topic
VALID = 's/i/range/mode'    # with host metadata
LOG_WAIT_S = 1.0


class Failure(Exception):
    pass


def _check(cond, msg):
    if not cond:
        raise Failure(msg)
    print(f'    ok: {msg}')


class _Errors(logging.Handler):

    def __init__(self):
        super().__init__(logging.ERROR)
        self.messages = []

    def emit(self, record):
        self.messages.append(record.getMessage())

    def wait(self, count):
        t_end = time.time() + LOG_WAIT_S
        while len(self.messages) < count and time.time() < t_end:
            time.sleep(0.01)
        return list(self.messages)


def _raises(fn):
    try:
        fn()
    except Exception as ex:
        return str(ex)
    return None


def test_blocking(dev, errors):
    print('test: blocking publish errors')
    for idx in range(2):
        ex = _raises(lambda: dev.publish(UNKNOWN, 1))
        _check(ex is not None and 'NOT_FOUND' in ex,
               f'unknown topic publish {idx} raises NOT_FOUND')
    ex = _raises(lambda: dev.publish(VALID, 'bogus'))
    _check(ex is not None and 'PARAMETER_INVALID' in ex,
           'invalid value raises PARAMETER_INVALID')
    _check(dev.query(UNKNOWN) is None, 'rejected value is not retained')


def test_nonblocking(dev, errors):
    print('test: timeout=0 publish errors log at ERROR')
    for idx, (topic, value, rc) in enumerate([
            (UNKNOWN, 1, '16 NOT_FOUND'),
            (UNKNOWN, 1, '16 NOT_FOUND'),      # repeated: not deduplicated
            (VALID, 'bogus', '5 PARAMETER_INVALID')]):
        errors.messages.clear()
        dev.publish(topic, value, timeout=0)
        expect = f'publish {dev.topic(topic)} failed: {rc}'
        _check(expect in errors.wait(1), f'{idx}: logged "{expect}"')


def test_normal_no_errors(d, path, errors):
    print('test: normal operation logs no ERROR')
    errors.messages.clear()
    with d.open(path, mode='restore') as dev:
        mode = dev.query(VALID)
        dev.publish(VALID, 'auto')
        dev.publish(VALID, 'auto', timeout=0)
        dev.publish(VALID, mode)
        values = []
        with dev.subscribe('s/stats/value', 'pub', lambda t, v: values.append(v)):
            dev.publish('s/stats/ctrl', 1)
            time.sleep(1.5)
            dev.publish('s/stats/ctrl', 0)
        _check(len(values) > 0, f'{len(values)} statistics values')
    time.sleep(LOG_WAIT_S)
    _check(errors.messages == [], f'no ERROR logs: {errors.messages}')


def main():
    errors = _Errors()
    logging.getLogger('jsdrv').addHandler(errors)
    failures = 0
    with Driver() as d:
        path = os.environ.get('JSDRV_HW_DEVICE')
        if path is None:
            paths = d.find_devices('js320')
            if not paths:
                print('SKIP: no JS320 connected (set JSDRV_HW_DEVICE)')
                return 0
            path = paths[0]
        print(f'device: {path}\n')
        tests = [
            lambda: test_normal_no_errors(d, path, errors),
            lambda: _with_device(d, path, lambda dev: test_blocking(dev, errors)),
            lambda: _with_device(d, path, lambda dev: test_nonblocking(dev, errors)),
        ]
        for t in tests:
            try:
                t()
            except Failure as ex:
                failures += 1
                print(f'    FAIL: {ex}')
            except Exception as ex:  # pragma: no cover - hardware faults
                failures += 1
                print(f'    ERROR: {ex!r}')
            print()
    if failures:
        print(f'FAILED ({failures} test(s))')
        return 1
    print('PASSED (all publish error checks)')
    return 0


def _with_device(d, path, fn):
    # A fresh open, so earlier tests do not leave retained values behind.
    with d.open(path, mode='restore') as dev:
        fn(dev)


if __name__ == '__main__':
    sys.exit(main())
