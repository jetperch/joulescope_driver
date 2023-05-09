# Copyright 2022-2023 Jetperch LLC
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
import numpy as np
import os
import platform
import psutil
import sys


def parser_config(p):
    """Display system information and connected Joulescope information."""
    return on_cmd


def version_to_str(version):
    if isinstance(version, str):
        return version
    v_patch = version & 0xffff
    v_minor = (version >> 16) & 0xff
    v_major = (version >> 24) & 0xff
    return f'{v_major}.{v_minor}.{v_patch}'


def on_cmd(args):
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
    sys_info = f"""
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
    print(sys_info)

    print(f"""
    JOULESCOPE INFORMATION
    ----------------------""")
    with Driver() as d:
        device_paths = d.device_paths()
        if len(device_paths) is None:
            print('No connected Joulescopes found')
        else:
            for device_path in d.device_paths():
                try:
                    d.open(device_path, mode='restore')
                except Exception:
                    print(f'    {device_path}: could not open')
                    continue
                try:
                    if '/js220/' in device_path:
                        fw = version_to_str(d.query(f'{device_path}/c/fw/version'))
                        hw = version_to_str(d.query(f'{device_path}/c/hw/version'))
                        fpga = version_to_str(d.query(f'{device_path}/s/fpga/version'))
                        print(f'    {device_path}: hw={hw}, fw={fw}, fpga={fpga}')
                    else:
                        print(f'    {device_path}')
                except Exception:
                    print(f'    {device_path}: could not retrieve details')
                finally:
                    d.close(device_path)
    print("")

    return 0
