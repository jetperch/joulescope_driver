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


# Demonstration showing power cycling then open can fail
# May be an issue with minibitty_host (joulescope_driver) or the JS320.

"""
Process to build the JS320 firmware
cd C:\repos\Jetperch\js320 && python -m pyminibitty product build

Process to update the JS320 firmware (note that since open fails, these commands are not reliable)
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe firmware update "C:\repos\Jetperch\js320\mbbuild\js320_p1_0_0_app\js320_p1_0_0_app.mbfw"
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe fpga_mem program "C:\repos\Jetperch\js320\gateware\impl1\js320_impl1.bit"
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe mem c/xspi/!cmd 0 erase 0x080000 0x20000
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe mem -p 1 c/xspi/!cmd 0 write 0x080000 "C:\repos\Jetperch\js320\mbbuild\js320_p1_0_0_app\pubsub_metadata.bin" --verify
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe mem s/flash/!cmd 0 erase 0x140000 0x20000
C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe mem -p 1 s/flash/!cmd 0 write 0x140000 "C:\repos\Jetperch\js320\mbbuild\js320_p1_1_0_app\pubsub_metadata.bin" --verify

You also have a jlink attached to troubleshoot the ctrl firmware.
See example python scripts using pylink in C:\repos\Jetperch\minibitty\tools

To build the minibitty_host:
cmake --build cmake-build && ctest --test-dir cmake-build

To build the minibitty_host pyjoulescope_driver:
python setup.py build_ext --inplace
"""

from pyjoulescope_driver import Driver
import sys
import time


POWER_DEVICE_PATH = 'u/js220/002557'
TARGET_DEVICE_PATH = 'u/js320/8W2A'


def run():
    with Driver() as d:
        while True:
            d.open(POWER_DEVICE_PATH, mode='restore')
            d.publish(POWER_DEVICE_PATH + '/s/i/range/mode', 'off')
            print('power off')
            time.sleep(1.0)
            d.publish(POWER_DEVICE_PATH + '/s/i/range/select', '10 A')
            d.publish(POWER_DEVICE_PATH + '/s/i/range/mode', 'manual')
            print('power on')
            time.sleep(2.0)
            d.close(POWER_DEVICE_PATH)

            d.open(TARGET_DEVICE_PATH, mode='restore')
            d.publish(TARGET_DEVICE_PATH + '/s/i/range/mode', 'auto')
            d.close(TARGET_DEVICE_PATH)


if __name__ == '__main__':
    sys.exit(run())
