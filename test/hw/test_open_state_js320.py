#!/usr/bin/env python3
# Copyright 2025 Jetperch LLC
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

"""Hardware-in-the-loop test for device-open state management.

Validates the open-restore semantics implemented in
``src/devices/mb_device/mb_device.c`` and the JS320 sensor stopgap in
``src/devices/js320/js320_drv.c`` against a physically connected JS320.

This test requires real hardware and is therefore NOT part of the
unit-test (ctest) suite.  It is gated on an environment variable so it
never runs accidentally in CI::

    JSDRV_HW_DEVICE=u/js320/X2VJ python test/hw/test_open_state_js320.py

If JSDRV_HW_DEVICE is unset, the first device returned by
``device_paths()`` is used.  The binding under test must be rebuilt
(``python setup.py build_ext --inplace``) after changing the C sources.

Open-mode semantics under test:
  * DEFAULTS: each writable topic becomes the host's retained value if
    present, else the metadata default; read-only topics are never
    pushed (this is what fixes the firmware-update loop).
  * RESUME: the host adopts the device's current pubsub values.
  * RAW: link-only; no state push; returns immediately.
"""

import contextlib
import os
import sys
import time

from pyjoulescope_driver import Driver


# A writable ctrl-instance topic with a known default of 0 (an LED blink
# pattern -- benign to toggle), and the read-only ctrl firmware version.
CTRL_WRITABLE = 'c/led/red'
CTRL_RO = 'c/fw/version'
# A writable sensor-instance topic with a known default of 0, used to
# exercise the best-effort sensor stopgap sync.
SENSOR_WRITABLE = 's/led/red'

OPEN_FAST_S = 0.25      # RAW open: immediate
# Non-RAW open now BLOCKS until the sensor 's' instance is synced (or the
# sensor-ready timeout), so 's' topics and c/comm/sensor/state are present
# the moment open() returns.  Typical ~0.2-0.4 s; bound well under the
# 1.5 s sensor-ready timeout / 2 s open timeout.
OPEN_OK_S = 1.0
SETTLE_S = 0.3          # let a publish land before query


class Failure(Exception):
    pass


def _check(cond, msg):
    if not cond:
        raise Failure(msg)
    print(f'    ok: {msg}')


@contextlib.contextmanager
def session():
    """A fresh Driver with an empty host pubsub cache."""
    d = Driver()
    try:
        yield d
    finally:
        try:
            d.finalize()
        except Exception:
            pass
        time.sleep(0.3)


def _set_device(dev, topic, value, settle=0.4):
    """Force the device's retained value via a fresh session."""
    with session() as d:
        d.open(dev, mode='defaults')   # empty host -> pushes defaults first
        time.sleep(settle)
        d.publish(f'{dev}/{topic}', value)
        time.sleep(0.3)
        d.close(dev)


def _open_query(dev, mode, topic, settle=0.5):
    """Fresh session: open in mode, return (open_seconds, queried value)."""
    with session() as d:
        t0 = time.time()
        d.open(dev, mode=mode)
        dt = time.time() - t0
        time.sleep(settle)
        v = d.query(f'{dev}/{topic}')
        d.close(dev)
        return dt, v


def test_resume_adopts_and_defaults_resets(dev):
    """DEFAULTS vs RESUME on the ctrl instance (fresh host each open)."""
    print('test: ctrl DEFAULTS vs RESUME')
    _set_device(dev, CTRL_WRITABLE, 1)
    _, v = _open_query(dev, 'restore', CTRL_WRITABLE)
    _check(v == 1, 'RESUME adopts device value (1)')
    # Empty host cache -> DEFAULTS must push the metadata default (0).
    _, v = _open_query(dev, 'defaults', CTRL_WRITABLE)
    _check(v == 0, 'DEFAULTS pushes metadata default (0) when host empty')
    _, v = _open_query(dev, 'restore', CTRL_WRITABLE)
    _check(v == 0, 'device was reset by DEFAULTS (RESUME sees 0)')


def test_defaults_preserves_host_value(dev):
    """DEFAULTS uses the host's retained value when present (gather)."""
    print('test: DEFAULTS preserves host value across reopen')
    T = f'{dev}/{CTRL_WRITABLE}'
    with session() as d:           # one session: host cache persists
        d.open(dev, mode='defaults')
        time.sleep(0.5)
        d.publish(T, 1)            # host now retains 1
        time.sleep(0.3)
        d.close(dev)
        dt = time.time()
        d.open(dev, mode='defaults')
        dt = time.time() - dt
        time.sleep(SETTLE_S)
        _check(d.query(T) == 1, 'host value (1) preserved through DEFAULTS')
        _check(dt < OPEN_OK_S, f'DEFAULTS open ok ({dt*1000:.0f} ms, no timeouts)')
        d.close(dev)


def test_readonly_not_corrupted(dev):
    """The read-only firmware version is never pushed/altered on open."""
    print('test: read-only topic untouched (no firmware-update loop)')
    _, fw = _open_query(dev, 'restore', CTRL_RO)
    for _ in range(3):
        _, v = _open_query(dev, 'defaults', CTRL_RO, settle=0.4)
        _check(v == fw, f'fw/version stable across DEFAULTS opens ({fw})')


def test_sensor_synced_at_open(dev):
    """Open blocks until the sensor 's' instance is synced (or times out).

    So 's' topics are present the moment open() returns -- one-shot tools
    (jsdrv info) see them again -- and the sync respects the open mode.
    """
    print('test: sensor synced as part of open; respects open mode')
    T = SENSOR_WRITABLE
    with session() as d:
        topics = {}
        d.subscribe(dev, 'pub', lambda t, v: topics.__setitem__(t, v))
        t0 = time.time()
        d.open(dev, mode='restore')
        dt = time.time() - t0
        # No settle: 's' must already be present right after open() returns.
        n_s = len([t for t in topics if t.startswith(f'{dev}/s/')])
        _check(n_s > 10, f's/ topics present at open ({n_s})')
        _check(dt < OPEN_OK_S, f'open waited for sensor and completed ({dt*1000:.0f} ms)')
        d.close(dev)

    _set_device(dev, SENSOR_WRITABLE, 1)
    _, v = _open_query(dev, 'restore', T)
    _check(v == 1, 'sensor RESUME adopts device value (1)')
    _, v = _open_query(dev, 'defaults', T)
    _check(v == 0, 'sensor DEFAULTS pushes default (0)')


def test_sensor_state_retained(dev):
    """c/comm/sensor/state is 1 right after open (closed-loop + retain).

    Open blocks until the sensor link is up, and the MiniBitty retain
    convention (leaf 'state' has no '!' prefix) keeps the value cached, so
    a query immediately after open() reflects the live device state.
    """
    print('test: c/comm/sensor/state == 1 at open')
    _, v = _open_query(dev, 'restore', 'c/comm/sensor/state')
    _check(v == 1, 'c/comm/sensor/state retained as 1 at open')


def test_host_instance_restored(dev):
    """Host-side 'h' topics are restored from the host cache on open.

    h/fp, h/fs, h/i_scale, h/v_scale are owned by the JS320 driver's
    handle_cmd (not a device pubsub instance).  After the sensor 's' sync
    the driver replays the host-cached 'h' values (jsdrvp_mb_dev_host_replay)
    so they survive an open/close cycle.
    """
    print('test: host-side h/ topics restored on open')
    with session() as d:
        d.open(dev, mode='restore')
        time.sleep(SETTLE_S)
        d.publish(f'{dev}/h/fp', 50)          # 50 Hz: a valid non-default option
        d.publish(f'{dev}/h/i_scale', 2.0)
        time.sleep(SETTLE_S)
        d.close(dev)
        d.open(dev, mode='restore')           # h-replay restores h/*
        time.sleep(SETTLE_S)
        _check(d.query(f'{dev}/h/fp') == 50, 'h/fp restored (50)')
        _check(abs(d.query(f'{dev}/h/i_scale') - 2.0) < 1e-9, 'h/i_scale restored (2.0)')
        d.close(dev)


def test_raw_immediate(dev):
    """RAW open returns immediately and does not push state."""
    print('test: RAW open immediate')
    with session() as d:
        t0 = time.time()
        d.open(dev, mode='raw')
        dt = time.time() - t0
        _check(dt < OPEN_FAST_S, f'RAW open immediate ({dt*1000:.0f} ms)')
        d.close(dev)


def test_stability(dev):
    """Repeated open/close across mixed modes is stable."""
    print('test: open/close stability (mixed modes)')
    modes = ['defaults', 'restore', 'raw'] * 4
    with session() as d:
        for m in modes:
            d.open(dev, mode=m)
            time.sleep(0.05)
            d.close(dev)
    _check(True, f'{len(modes)} open/close cycles completed without error')


def main():
    dev = os.environ.get('JSDRV_HW_DEVICE')
    if dev is None:
        with session() as d:
            paths = d.device_paths()
        if not paths:
            print('SKIP: no device connected (set JSDRV_HW_DEVICE)')
            return 0
        dev = paths[0]
    print(f'device: {dev}\n')
    tests = [
        test_raw_immediate,
        test_readonly_not_corrupted,
        test_resume_adopts_and_defaults_resets,
        test_defaults_preserves_host_value,
        test_sensor_synced_at_open,
        test_sensor_state_retained,
        test_host_instance_restored,
        test_stability,
    ]
    failures = 0
    for t in tests:
        try:
            t(dev)
        except Failure as ex:
            failures += 1
            print(f'    FAIL: {ex}')
        except Exception as ex:   # pragma: no cover - hardware faults
            failures += 1
            print(f'    ERROR: {ex}')
        print()
    if failures:
        print(f'FAILED ({failures} test(s))')
        return 1
    print('PASSED (all hardware-in-the-loop checks)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
