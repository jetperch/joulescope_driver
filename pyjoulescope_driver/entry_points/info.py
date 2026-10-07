# SPDX-FileCopyrightText: Copyright 2022 Jetperch LLC
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

from pyjoulescope_driver import Driver, __version__
from .device_arg import add_brand_argument, add_device_argument
from .metadata import metadata_load
import numpy as np
import os
import platform
import psutil
import sys


def parser_config(p):
    """Display system, package and device information.

    Use the "values" and "metadata" commands for device details.
    """
    add_device_argument(p, 'Defaults to all connected devices.')
    add_brand_argument(p)
    return on_cmd


def version_to_str(version):
    if isinstance(version, str):
        return version
    v_patch = version & 0xffff
    v_minor = (version >> 16) & 0xff
    v_major = (version >> 24) & 0xff
    return f'{v_major}.{v_minor}.{v_patch}'


def format_value(meta, value):
    """Format a value for display using its metadata.

    :param meta: The topic metadata dict, or None.
    :param value: The topic value.
    :return: The value to display.  Only u32 values with metadata
        format "version" are converted to "major.minor.patch".
    """
    if meta is not None and meta.get('format', None) == 'version':
        return version_to_str(value)
    return value


def _sys_info():
    try:
        from pyjls import __version__ as jls_version
    except ImportError:
        jls_version = 'uninstalled'
    try:
        os.environ['JOULESCOPE_BACKEND'] = 'none'
        from joulescope import __version__ as joulescope_version
    except ImportError:
        joulescope_version = 'uninstalled'

    cpufreq = psutil.cpu_freq()
    vm = psutil.virtual_memory()
    vm_available = (vm.total - vm.used) / (1024 ** 3)
    vm_total = vm.total / (1024 ** 3)
    return f"""\

    SYSTEM INFORMATION
    ------------------
    python               {sys.version}
    python impl          {platform.python_implementation()}
    platform             {platform.platform()}
    processor            {platform.processor()}
    CPU cores            {psutil.cpu_count(logical=False)} physical, {psutil.cpu_count(logical=True)} total
    CPU frequency        {cpufreq.current:.0f} MHz ({cpufreq.min:.0f} MHz min to {cpufreq.max:.0f} MHz max)   
    RAM                  {vm_available:.1f} GB available, {vm_total:.1f} GB total ({vm_available/vm_total *100:.1f}%)
    
    PYTHON PACKAGE INFORMATION
    --------------------------
    jls                  {jls_version}
    joulescope           {joulescope_version}
    numpy                {np.__version__}
    pyjoulescope_driver  {__version__}
    """


_JOULESCOPE_INFORMATION = """
    JOULESCOPE INFORMATION
    ----------------------"""


def _list_devices(driver, specs=None, brand=None):
    txt = []
    device_paths = driver.find_devices(specs, brand)
    if len(device_paths) == 0:
        txt.append('No connected devices found')
    else:
        for device_path in device_paths:
            try:
                driver.open(device_path, mode='restore')
            except Exception:
                txt.append(f'    {device_path}: could not open')
                continue
            try:
                if device_path.model in ('js220', 'js320'):
                    meta = metadata_load(driver, device_path)
                    v = {}
                    for name, subtopic in [('hw', 'c/hw/version'), ('fw', 'c/fw/version'),
                                           ('fpga', 's/fpga/version')]:
                        value = driver.query(f'{device_path}/{subtopic}')
                        v[name] = format_value(meta.get(subtopic), value)
                    txt.append(f'    {device_path}: hw={v["hw"]}, fw={v["fw"]}, fpga={v["fpga"]}')
                else:
                    txt.append(f'    {device_path}')
            except Exception:
                txt.append(f'    {device_path}: could not retrieve details')
            finally:
                driver.close(device_path)
    txt.append('')
    return '\n'.join(txt)


def on_cmd(args):
    print(_sys_info())
    print(_JOULESCOPE_INFORMATION)
    with Driver() as d:
        d.log_level = args.jsdrv_log_level
        print(_list_devices(d, args.device, args.brand))
    return 0
