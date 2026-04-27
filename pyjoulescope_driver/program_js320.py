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

"""JS320 firmware update via the in-driver fwup manager.

Mirrors the C definitions in
``include_private/jsdrv_prv/devices/js320/js320_fwup_mgr.h``.
The full update sequence runs in a worker thread inside the driver;
this module just publishes the request and watches the status topic.
"""

import json
import logging
import struct
import threading

from pyjoulescope_driver import Driver


_log = logging.getLogger(__name__)


FWUP_TOPIC_ADD = 'fwup/@/!add'
FWUP_FLAG_SKIP_CTRL = 1 << 0
FWUP_FLAG_SKIP_FPGA = 1 << 1
FWUP_FLAG_SKIP_RESOURCES = 1 << 2

# struct jsdrv_fwup_add_header_s: char[32] device_prefix, u32 flags, u32 zip_size
_FWUP_ADD_HEADER = struct.Struct('<32sII')


def program_js320(driver: Driver, device_path: str,
                  package_path: str = None, flags: int = 0,
                  progress=None, timeout: float = 600.0):
    """Program a JS320 device using the firmware update manager.

    Publishes the request to ``fwup/@/!add`` and monitors the
    per-worker ``fwup/NNN/status`` topic until the worker reports a
    terminal ``DONE`` or ``ERROR`` state.

    The driver must be initialized but the device must NOT already be
    open: the worker opens the device internally in raw mode.

    :param driver: The driver instance.
    :param device_path: The target device path (e.g., ``u/js320/8w2a``).
    :param package_path: The path to the manufacturing ZIP package.
        ``None`` (default) uses the firmware embedded in the driver build.
    :param flags: Bitmask of ``FWUP_FLAG_SKIP_*`` flags.
    :param progress: An optional callable(completion: float, msg: str).
    :param timeout: Maximum total time to wait, in seconds.
    :raise RuntimeError: If the firmware update fails.
    :raise TimeoutError: If the firmware update does not complete in time.
    """
    if progress is None:
        progress = lambda x, y: None

    if package_path is None:
        zip_data = b''
    else:
        with open(package_path, 'rb') as f:
            zip_data = f.read()

    device_prefix = device_path.encode('utf-8')
    if len(device_prefix) >= 32:
        raise ValueError(f'device path too long: {device_path}')
    payload = _FWUP_ADD_HEADER.pack(device_prefix, flags, len(zip_data)) + zip_data

    done = threading.Event()
    last = {'state': '', 'msg': '', 'rc': 0}

    def on_status(topic, value):
        if not isinstance(value, str) or not topic.endswith('/status'):
            return
        try:
            entry = json.loads(value)
        except (ValueError, TypeError):
            return
        state = str(entry.get('state', '?'))
        msg = str(entry.get('msg', ''))
        try:
            rc = int(entry.get('rc', 0))
        except (TypeError, ValueError):
            rc = 0
        try:
            pct = float(entry.get('pct', 0.0))
        except (TypeError, ValueError):
            pct = 0.0
        last['state'] = state
        last['msg'] = msg
        last['rc'] = rc
        _log.info('fwup %s %.1f%%: %s (rc=%d) [%s]',
                  state, pct, msg, rc, topic)
        progress(pct / 100.0, f'{state}: {msg}')
        if state in ('DONE', 'ERROR'):
            done.set()

    progress(0.0, 'Starting JS320 firmware update')
    driver.subscribe('fwup/', 'pub', on_status)
    try:
        driver.publish(FWUP_TOPIC_ADD, payload)
        if not done.wait(timeout):
            raise TimeoutError(
                f'JS320 firmware update timed out after {timeout:.0f} s '
                f'(last state {last["state"]}: {last["msg"]})')
    finally:
        driver.unsubscribe('fwup/', on_status)

    if last['state'] != 'DONE' or last['rc'] != 0:
        raise RuntimeError(
            f'JS320 firmware update failed: {last["msg"]} '
            f'(state={last["state"]}, rc={last["rc"]})')
    progress(1.0, 'Complete')
