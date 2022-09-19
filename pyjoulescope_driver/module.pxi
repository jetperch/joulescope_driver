# Copyright 2020-2022 Jetperch LLC
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
Joulescope driver initialization for a python module.
"""

from libc.stdint cimport intptr_t
from libc.stdlib cimport malloc, free
import logging
log = logging.getLogger(__name__)


LOG_LEVEL_MAP = {
    '!': logging.CRITICAL,
    'A': logging.CRITICAL,
    'C': logging.CRITICAL,
    'E': logging.ERROR,
    'W': logging.WARNING,
    'N': logging.INFO,
    'I': logging.INFO,
    'D': logging.DEBUG,
    '.': 0,
}

cdef extern from "stdarg.h":
    ctypedef struct va_list:
        pass
    ctypedef struct fake_type:
        pass
    void va_start(va_list, void* arg) nogil
    void* va_arg(va_list, fake_type) nogil
    void va_end(va_list) nogil
    fake_type int_type "int"

cdef extern from "stdio.h":
    int vsnprintf (char * s, size_t n, const char * format, va_list arg ) nogil
