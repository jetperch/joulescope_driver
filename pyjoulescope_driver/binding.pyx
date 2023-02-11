# Copyright 2022 Jetperch LLC
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
Python binding for the native Joulescope driver implementation.
"""

# See https://cython.readthedocs.io/en/latest/index.html

# cython: boundscheck=True, wraparound=True, nonecheck=True, overflowcheck=True, cdivision=True, embedsignature=True

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int8_t, int16_t, int32_t, int64_t
from libc.float cimport DBL_MAX
from libc.math cimport isfinite, NAN
from libc.string cimport memset

from collections.abc import Mapping
import json
import logging
import numpy as np
include "module.pxi"
import time
cimport numpy as np
from . cimport c_jsdrv


__all__ = ['Driver']
np.import_array()                           # initialize numpy before use
_log_c_name = 'jsdrv'
_log_c = logging.getLogger(_log_c_name)


cdef object _i128_to_int(uint64_t high, uint64_t low):
    i = int(high) << 64
    i |= int(low)
    if i & 0x8000_0000_0000_0000:
        i = ((~i) & 0xffff_ffff_ffff_ffff) - 1
    return i


cdef object _parse_buffer_info(c_jsdrv.jsdrv_buffer_info_s * info):
    v = {
        'version': info[0].version,
        'field_id': info[0].field_id,
        'index': info[0].index,
        'element_type': info[0].element_type,
        'element_size_bits': info[0].element_size_bits,
        'topic': info[0].topic,
        'size_in_utc': info[0].size_in_utc,
        'size_in_samples': info[0].size_in_samples,
        'time_range_utc': {
            'start': info[0].time_range_utc.start,
            'end': info[0].time_range_utc.end,
            'length': info[0].time_range_utc.length,
        },
        'time_range_samples': {
            'start': info[0].time_range_samples.start,
            'end': info[0].time_range_samples.end,
            'length': info[0].time_range_samples.length,

        },
        'sample_rate': info[0].sample_rate,
    }
    return v


cdef object _parse_buffer_rsp(c_jsdrv.jsdrv_buffer_response_s * r):
    cdef np.npy_intp shape[2]
    v = {
        'version': r[0].version,
        'rsp_id': r[0].rsp_id,
        'info': _parse_buffer_info(&r[0].info),
    }
    length = v['info']['time_range_samples']['length']
    if r[0].response_type == c_jsdrv.JSDRV_BUFFER_RESPONSE_SAMPLES:
        v['response_type'] = 'samples'
        if v['info']['element_type'] == c_jsdrv.JSDRV_DATA_TYPE_FLOAT and ['info']['element_size_bits'] == 32:
            shape[0] = <np.npy_intp> length
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_FLOAT32, <void *> &r[0].data[0])
        elif v['info']['element_type'] == c_jsdrv.JSDRV_DATA_TYPE_UINT and ['info']['element_size_bits'] == 1:
            shape[0] = <np.npy_intp> (length // 8)
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> &r[0].data[0])
        elif v['info']['element_type'] == c_jsdrv.JSDRV_DATA_TYPE_UINT and ['info']['element_size_bits'] == 1:
            shape[0] = <np.npy_intp> (length // 2)
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> &r[0].data[0])
        else:
            _log_c.error('unsupported sample format')
        v['data'] = ndarray.copy()
    elif r[0].response_type == c_jsdrv.JSDRV_BUFFER_RESPONSE_SUMMARY:
        v['response_type'] = 'summary'
        shape[0] = <np.npy_intp> length
        shape[1] = <np.npy_intp> 4
        ndarray = np.PyArray_SimpleNewFromData(2, shape, np.NPY_FLOAT32, <void *> &r[0].data[0])
        v['data'] = ndarray.copy()
    else:
        _log_c.error(f'unsupported response_type {r[0].response_type}')
    return v


cdef object _pack_buffer_req(r):
    cdef c_jsdrv.jsdrv_buffer_request_s s
    cdef uint8_t * u8_ptr

    s.version = 1
    time_type = r['time_type'].lower()
    if time_type == 'utc':
        s.time_type = c_jsdrv.JSDRV_TIME_UTC
        s.time.utc.start = r['start']
        s.time.utc.end = r.get('end', 0)
        s.time.utc.length = r.get('length', 0)
    elif time_type == 'samples':
        s.time_type = c_jsdrv.JSDRV_TIME_SAMPLES
        s.time.samples.start = r['start']
        s.time.samples.end = r.get('end', 0)
        s.time.samples.length = r.get('length', 0)
    else:
        raise ValueError(f'invalid time type: {time_type}')
    s.rsv1_u8 = 0
    s.rsv2_u8 = 0
    s.rsv3_u32 = 0
    s.rsp_topic = r['rsp_topic']
    s.rsp_id = r['rsp_id']

    u8_ptr = <uint8_t *> &s;
    return bytes(u8_ptr[:sizeof(s)])



cdef object _jsdrv_union_to_py(const c_jsdrv.jsdrv_union_s * value):
    cdef c_jsdrv.jsdrv_stream_signal_s * stream;
    cdef np.npy_intp shape[1]
    cdef uint8_t[:] u8_mem
    cdef int32_t idx
    t = value[0].type
    try:
        if t == c_jsdrv.JSDRV_UNION_STR:
            v = value[0].value.str.decode('utf-8')
        elif t == c_jsdrv.JSDRV_UNION_JSON:
            v = value[0].value.str.decode('utf-8')
            try:
                v = json.loads(v)
            except Exception:
                v = None
        elif t == c_jsdrv.JSDRV_UNION_BIN:
            if value[0].app == c_jsdrv.JSDRV_PAYLOAD_TYPE_STREAM:
                stream = <c_jsdrv.jsdrv_stream_signal_s *> &(value[0].value.bin[0])
                el = (stream[0].element_type, stream[0].element_size_bits)
                v = {
                    'sample_id': stream[0].sample_id,
                    'field_id': stream[0].field_id,
                    'index': stream[0].index,
                    'sample_rate': stream[0].sample_rate,
                    'decimate_factor': stream[0].decimate_factor,
                }
                if el == (c_jsdrv.JSDRV_DATA_TYPE_FLOAT, 32):  # float32
                    shape[0] = <np.npy_intp> stream[0].element_count
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_FLOAT32, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_UINT, 1):  # uint1, 8 per uint8
                    shape[0] = <np.npy_intp> ((stream[0].element_count + 7) / 8)
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_INT, 16):  # int16
                    shape[0] = <np.npy_intp> stream[0].element_count
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_INT16, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_UINT, 8):  # uint8
                    shape[0] = <np.npy_intp> stream[0].element_count
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_UINT, 4):  # uint4 -> uint8
                    # unpack to make i/range support easy
                    # future uses may want to leave packed, so may need to check JSDRV_FIELD_RANGE
                    ndarray = np.empty(stream[0].element_count, dtype=np.uint8)
                    u8_mem = ndarray
                    for idx in range(0, stream[0].element_count >> 1):
                        u8_mem[2 * idx] = stream[0].data[idx] & 0x0f
                        u8_mem[2 * idx + 1] = (stream[0].data[idx] >> 4) & 0x0f
                    v['data'] = ndarray
                else:
                    print('jsdrv._jsdrv_union_to_py: unsupported data type')
                    v['data'] = None
            elif value[0].app == c_jsdrv.JSDRV_PAYLOAD_TYPE_STATISTICS:
                stats = <c_jsdrv.jsdrv_statistics_s *> &(value[0].value.bin[0])
                sample_freq = stats[0].sample_freq
                samples_full_rate = stats[0].block_sample_count * stats[0].decimate_factor
                sample_id_start = stats[0].block_sample_id
                sample_id_end = stats[0].block_sample_id + samples_full_rate
                t_start = sample_id_start / sample_freq
                t_delta = samples_full_rate / sample_freq
                charge = _i128_to_int(stats[0].charge_i128[1], stats[0].charge_i128[0])
                energy = _i128_to_int(stats[0].energy_i128[1], stats[0].energy_i128[0])
                v = {
                    'time': {
                        'samples': {'value': [sample_id_start, sample_id_end], 'units': 'samples'},
                        'sample_freq': {'value': sample_freq, 'units': 'Hz'},
                        'range': {'value': [t_start, t_start + t_delta], 'units': 's'},
                        'delta': {'value': t_delta, 'units': 's'},
                        'decimate_factor': {'value': stats[0].decimate_factor, 'units': 'samples'},
                        'decimate_sample_count': {'value': stats[0].block_sample_count, 'units': 'samples'},
                        'accum_samples': {'value': [stats[0].accum_sample_id, sample_id_end], 'units': 'samples'},
                    },
                    'signals': {
                        'current': {
                            'avg': {'value': stats[0].i_avg, 'units': 'A'},
                            'std': {'value': stats[0].i_std, 'units': 'A'},
                            'min': {'value': stats[0].i_min, 'units': 'A'},
                            'max': {'value': stats[0].i_max, 'units': 'A'},
                            'p2p': {'value': stats[0].i_max - stats[0].i_min, 'units': 'A'},
                            'integral': {'value': stats[0].i_avg * t_delta, 'units': 'C'},
                        },
                        'voltage': {
                            'avg': {'value': stats[0].v_avg, 'units': 'V'},
                            'std': {'value': stats[0].v_std, 'units': 'V'},
                            'min': {'value': stats[0].v_min, 'units': 'V'},
                            'max': {'value': stats[0].v_max, 'units': 'V'},
                            'p2p': {'value': stats[0].v_max - stats[0].v_min, 'units': 'V'},
                        },
                        'power': {
                            'avg': {'value': stats[0].p_avg, 'units': 'W'},
                            'std': {'value': stats[0].p_std, 'units': 'W'},
                            'min': {'value': stats[0].p_min, 'units': 'W'},
                            'max': {'value': stats[0].p_max, 'units': 'W'},
                            'p2p': {'value': stats[0].p_max - stats[0].p_min, 'units': 'W'},
                            'integral': {'value': stats[0].p_avg * t_delta, 'units': 'J'},
                        },
                    },
                    'accumulators': {
                        'charge': {
                            'value': stats[0].charge_f64,
                            'int_value': charge,
                            'int_scale': 2 ** -31,
                            'units': 'C',
                        },
                        'energy': {
                            'value': stats[0].energy_f64,
                            'int_value': energy,
                            'int_scale': 2 ** -27,
                            'units': 'J',
                        },
                    },
                    'source': 'sensor',
                }
            elif value[0].app == c_jsdrv.JSDRV_PAYLOAD_TYPE_BUFFER_INFO:
                v = _parse_buffer_info(<c_jsdrv.jsdrv_buffer_info_s *> &(value[0].value.bin[0]))
            elif value[0].app == c_jsdrv.JSDRV_PAYLOAD_TYPE_BUFFER_RSP:
                v = _parse_buffer_rsp(<c_jsdrv.jsdrv_buffer_response_s *> &(value[0].value.bin[0]))
            else:
                v = value[0].value.bin[:value[0].size]
        elif t == c_jsdrv.JSDRV_UNION_F32:
            v = value[0].value.f32
        elif t == c_jsdrv.JSDRV_UNION_F64:
            v = value[0].value.f64
        elif t == c_jsdrv.JSDRV_UNION_U8:
            v = value[0].value.u8
        elif t == c_jsdrv.JSDRV_UNION_U16:
            v = value[0].value.u16
        elif t == c_jsdrv.JSDRV_UNION_U32:
            v = value[0].value.u32
        elif t == c_jsdrv.JSDRV_UNION_U64:
            v = value[0].value.u64
        elif t == c_jsdrv.JSDRV_UNION_I8:
            v = value[0].value.i8
        elif t == c_jsdrv.JSDRV_UNION_I16:
            v = value[0].value.i16
        elif t == c_jsdrv.JSDRV_UNION_I32:
            v = value[0].value.i32
        elif t == c_jsdrv.JSDRV_UNION_I64:
            v = value[0].value.i64
        else:
            v = None
    except Exception as ex:
        print(f'_jsdrv_union_to_py conversion failed: {ex}')
        v = None
    return v


class ErrorCode:
    # automatically maintained by error_code_update.py
    # ENUM_ERROR_CODE_START
    SUCCESS                     = 0
    UNSPECIFIED                 = 1
    NOT_ENOUGH_MEMORY           = 2
    NOT_SUPPORTED               = 3
    IO                          = 4
    PARAMETER_INVALID           = 5
    INVALID_RETURN_CONDITION    = 6
    INVALID_CONTEXT             = 7
    INVALID_MESSAGE_LENGTH      = 8
    MESSAGE_INTEGRITY           = 9
    SYNTAX_ERROR                = 10
    TIMED_OUT                   = 11
    FULL                        = 12
    EMPTY                       = 13
    TOO_SMALL                   = 14
    TOO_BIG                     = 15
    NOT_FOUND                   = 16
    ALREADY_EXISTS              = 17
    PERMISSIONS                 = 18
    BUSY                        = 19
    UNAVAILABLE                 = 20
    IN_USE                      = 21
    CLOSED                      = 22
    SEQUENCE                    = 23
    ABORTED                     = 24
    SYNCHRONIZATION             = 25
    # ENUM_ERROR_CODE_END


_error_code_to_meta = {
    # automatically maintained by error_code_update.py
    # ENUM_ERROR_CODE_META_START
    0: (0, 'SUCCESS', 'Success (no error)'),
    1: (1, 'UNSPECIFIED', 'Unspecified error'),
    2: (2, 'NOT_ENOUGH_MEMORY', 'Insufficient memory to complete the operation'),
    3: (3, 'NOT_SUPPORTED', 'Operation is not supported'),
    4: (4, 'IO', 'Input/output error'),
    5: (5, 'PARAMETER_INVALID', 'The parameter value is invalid'),
    6: (6, 'INVALID_RETURN_CONDITION', 'The function return condition is invalid'),
    7: (7, 'INVALID_CONTEXT', 'The context is invalid'),
    8: (8, 'INVALID_MESSAGE_LENGTH', 'The message length in invalid'),
    9: (9, 'MESSAGE_INTEGRITY', 'The message integrity check failed'),
    10: (10, 'SYNTAX_ERROR', 'A syntax error was detected'),
    11: (11, 'TIMED_OUT', 'The operation did not complete in time'),
    12: (12, 'FULL', 'The target of the operation is full'),
    13: (13, 'EMPTY', 'The target of the operation is empty'),
    14: (14, 'TOO_SMALL', 'The target of the operation is too small'),
    15: (15, 'TOO_BIG', 'The target of the operation is too big'),
    16: (16, 'NOT_FOUND', 'The requested resource was not found'),
    17: (17, 'ALREADY_EXISTS', 'The requested resource already exists'),
    18: (18, 'PERMISSIONS', 'Insufficient permissions to perform the operation.'),
    19: (19, 'BUSY', 'The requested resource is currently busy.'),
    20: (20, 'UNAVAILABLE', 'The requested resource is currently unavailable.'),
    21: (21, 'IN_USE', 'The requested resource is currently in use.'),
    22: (22, 'CLOSED', 'The requested resource is currently closed.'),
    23: (23, 'SEQUENCE', 'The requested operation was out of sequence.'),
    24: (24, 'ABORTED', 'The requested operation was previously aborted.'),
    25: (25, 'SYNCHRONIZATION', 'The target is not synchronized with the originator.'),
    'SUCCESS': (0, 'SUCCESS', 'Success (no error)'),
    'UNSPECIFIED': (1, 'UNSPECIFIED', 'Unspecified error'),
    'NOT_ENOUGH_MEMORY': (2, 'NOT_ENOUGH_MEMORY', 'Insufficient memory to complete the operation'),
    'NOT_SUPPORTED': (3, 'NOT_SUPPORTED', 'Operation is not supported'),
    'IO': (4, 'IO', 'Input/output error'),
    'PARAMETER_INVALID': (5, 'PARAMETER_INVALID', 'The parameter value is invalid'),
    'INVALID_RETURN_CONDITION': (6, 'INVALID_RETURN_CONDITION', 'The function return condition is invalid'),
    'INVALID_CONTEXT': (7, 'INVALID_CONTEXT', 'The context is invalid'),
    'INVALID_MESSAGE_LENGTH': (8, 'INVALID_MESSAGE_LENGTH', 'The message length in invalid'),
    'MESSAGE_INTEGRITY': (9, 'MESSAGE_INTEGRITY', 'The message integrity check failed'),
    'SYNTAX_ERROR': (10, 'SYNTAX_ERROR', 'A syntax error was detected'),
    'TIMED_OUT': (11, 'TIMED_OUT', 'The operation did not complete in time'),
    'FULL': (12, 'FULL', 'The target of the operation is full'),
    'EMPTY': (13, 'EMPTY', 'The target of the operation is empty'),
    'TOO_SMALL': (14, 'TOO_SMALL', 'The target of the operation is too small'),
    'TOO_BIG': (15, 'TOO_BIG', 'The target of the operation is too big'),
    'NOT_FOUND': (16, 'NOT_FOUND', 'The requested resource was not found'),
    'ALREADY_EXISTS': (17, 'ALREADY_EXISTS', 'The requested resource already exists'),
    'PERMISSIONS': (18, 'PERMISSIONS', 'Insufficient permissions to perform the operation.'),
    'BUSY': (19, 'BUSY', 'The requested resource is currently busy.'),
    'UNAVAILABLE': (20, 'UNAVAILABLE', 'The requested resource is currently unavailable.'),
    'IN_USE': (21, 'IN_USE', 'The requested resource is currently in use.'),
    'CLOSED': (22, 'CLOSED', 'The requested resource is currently closed.'),
    'SEQUENCE': (23, 'SEQUENCE', 'The requested operation was out of sequence.'),
    'ABORTED': (24, 'ABORTED', 'The requested operation was previously aborted.'),
    'SYNCHRONIZATION': (25, 'SYNCHRONIZATION', 'The target is not synchronized with the originator.'),
    # ENUM_ERROR_CODE_META_END
}


def error_code_to_str(ec):
    """Get the string representation for an error code.

    :param ec: The error code value [int] or error code name [str].
    :return: The error code's name
    """
    v, name, rv = _error_code_to_meta.get(ec, (-1, "unknown", "unknown"))
    return f'{v} {name} : {rv}'


class LogLevel:
    OFF         = -1
    EMERGENCY   = 0
    ALERT       = 1
    CRITICAL    = 2
    ERROR       = 3
    WARNING     = 4
    NOTICE      = 5
    INFO        = 6
    DEBUG1      = 7
    DEBUG2      = 8
    DEBUG3      = 9
    ALL         = 10


_log_level_str_to_int = {
    'off': LogLevel.OFF,
    None: LogLevel.OFF,
    'emergency': LogLevel.EMERGENCY,
    'e': LogLevel.EMERGENCY,
    'alert': LogLevel.ALERT,
    'a': LogLevel.ALERT,
    'critical': LogLevel.CRITICAL,
    'c': LogLevel.CRITICAL,
    'error': LogLevel.ERROR,
    'e': LogLevel.ERROR,
    'warn': LogLevel.WARNING,
    'warning': LogLevel.WARNING,
    'w': LogLevel.WARNING,
    'notice': LogLevel.NOTICE,
    'n': LogLevel.NOTICE,
    'info': LogLevel.INFO,
    'i': LogLevel.INFO,
    'debug1': LogLevel.DEBUG1,
    'd': LogLevel.DEBUG1,
    'debug': LogLevel.DEBUG1,
    'debug2': LogLevel.DEBUG2,
    'debug3': LogLevel.DEBUG3,
    'all': LogLevel.ALL,
}


_log_level_c_to_py = {
    LogLevel.OFF: None,
    LogLevel.EMERGENCY: logging.CRITICAL,
    LogLevel.ALERT: logging.CRITICAL,
    LogLevel.CRITICAL: logging.CRITICAL,
    LogLevel.ERROR: logging.ERROR,
    LogLevel.WARNING: logging.WARNING,
    LogLevel.NOTICE: logging.INFO,
    LogLevel.INFO: logging.INFO,
    LogLevel.DEBUG1: logging.DEBUG,
    LogLevel.DEBUG2: logging.DEBUG,
    LogLevel.DEBUG3: logging.DEBUG,
    LogLevel.ALL: logging.NOTSET,
}


def log_level_c_to_py(lvl):
    return _log_level_c_to_py.get(int(lvl))


class SubscribeFlags:
    NONE = 0                ## No flags (always 0).
    RETAIN = (1 << 0)       ## Immediately forward retained PUB and/or METADATA, depending upon JSDRV_PUBSUB_SFLAG_PUB and JSDRV_PUBSUB_SFLAG_METADATA_RSP.
    PUB = (1 << 1)          ## Do not receive normal topic publish.
    METADATA_REQ = (1 << 2) ## Subscribe to receive metadata requests like "%", "a/b/%", and "a/b/c%".
    METADATA_RSP = (1 << 3) ## Subscribe to receive metadata responses like "a/b/c$.
    QUERY_REQ = (1 << 4)    ## Subscribe to receive query requests like "&", "a/b/&", and "a/b/c&".
    QUERY_RSP = (1 << 5)    ## Subscribe to receive query responses like "a/b/c?".
    RETURN_CODE = (1 << 6)  ## Subscribe to receive return code messages like "a/b/c#".


_SUBSCRIBE_FLAG_LOOKUP = {
    None: SubscribeFlags.PUB | SubscribeFlags.RETAIN,
    'pub': SubscribeFlags.PUB,
    'pub_retain': SubscribeFlags.PUB | SubscribeFlags.RETAIN,
    'metadata_req': SubscribeFlags.METADATA_REQ,
    'metadata_rsp': SubscribeFlags.METADATA_RSP,
    'metadata_rsp_retain': SubscribeFlags.METADATA_RSP | SubscribeFlags.RETAIN,
    'metadata': SubscribeFlags.METADATA_RSP | SubscribeFlags.RETAIN,
    'query_req': SubscribeFlags.QUERY_REQ,
    'query_rsp': SubscribeFlags.QUERY_RSP,
    'return_code': SubscribeFlags.RETURN_CODE,
}


_DEVICE_OPEN_MODES = {
    0: 0,
    'defaults': 0,
    None: 0,
    1: 1,
    'restore': 1,
    0xff: 0xff,
    'raw': 0xff
}


cdef int32_t _driver_count = 0
_TIMEOUT_MS_DEFAULT = 1000
_TIMEOUT_MS_INIT = 5000


cdef int32_t _timeout_validate(value, default=None):
    if default is None:
        default = _TIMEOUT_MS_DEFAULT
    if value is None:
        return default
    return <int32_t> (float(value) * 1000)


def _handle_rc(rc, src):
    if rc:
        if rc == ErrorCode.TIMED_OUT:
            raise TimeoutError(f'{src} timed out')
        else:
            raise RuntimeError(f'{src} failed {rc}')


cdef class Driver:
    cdef c_jsdrv.jsdrv_context_s * _context
    cdef object _subscribers

    def __init__(self, timeout=None):
        global _driver_count
        self._context = NULL
        cdef int32_t rc
        timeout_ms = _timeout_validate(timeout, _TIMEOUT_MS_INIT)
        with nogil:
            rc = c_jsdrv.jsdrv_initialize(&self._context, NULL, timeout_ms)
        _handle_rc(rc, 'jsdrv_initialize')
        self._subscribers = set()  # (topic, fn)
        if _driver_count == 0:
            c_jsdrv.jsdrv_log_initialize()
            c_jsdrv.jsdrv_log_register(_on_log_recv, NULL)
        _driver_count += 1

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.finalize()

    @property
    def log_level(self):
        """Get the current log level."""
        return c_jsdrv.jsdrv_log_level_get()

    @log_level.setter
    def log_level(self, level):
        """Set the current log level.

        :param level: The log level.
        """
        if isinstance(level, str):
            level = _log_level_str_to_int.get(level.lower(), LogLevel.OFF)
        c_jsdrv.jsdrv_log_level_set(level)

    def finalize(self, timeout=None):
        """Finalize the driver.

        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        """
        global _driver_count
        cdef c_jsdrv.jsdrv_context_s * context = self._context
        timeout_ms = _timeout_validate(timeout)
        with nogil:
            c_jsdrv.jsdrv_finalize(context, timeout_ms)
            c_jsdrv.jsdrv_log_finalize()
        _driver_count -= 1

    def publish(self, topic: str, value, timeout=None):
        """Publish a value to a topic.

        :param topic: The topic string.
        :param value: The value, which must pass validation for the topic.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :raise: On error.
        """
        cdef c_jsdrv.jsdrv_union_s v
        cdef char * byte_str
        cdef const uint8_t[:] topic_str = topic.encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)

        memset(&v, 0, sizeof(v))
        if isinstance(value, str):
            py_byte_str = value.encode('utf-8')
            byte_str = py_byte_str
            v.type = c_jsdrv.JSDRV_UNION_STR
            v.value.str = &byte_str[0]
        elif isinstance(value, int):
            if (value >= 0) and (value < 4294967296LL):
                v.type = c_jsdrv.JSDRV_UNION_U32
                v.value.u64 = value
            elif value >= 0:
                v.type = c_jsdrv.JSDRV_UNION_U64
                v.value.u64 = value
            elif value >= -2147483648LL:
                v.type = c_jsdrv.JSDRV_UNION_I32
                v.value.i64 = value
            else:
                v.type = c_jsdrv.JSDRV_UNION_I64
                v.value.i64 = value
        elif topic.startswith('m/') and topic.endswith('/!req'):
            value = _pack_buffer_req(value)
            byte_str = value
            v.type = c_jsdrv.JSDRV_UNION_BIN
            v.value.bin = <const uint8_t *> byte_str
            v.app = c_jsdrv.JSDRV_PAYLOAD_TYPE_BUFFER_REQ
            v.size = <uint32_t> len(value)
        elif isinstance(value, bytes):
            byte_str = value
            v.type = c_jsdrv.JSDRV_UNION_BIN
            v.value.bin = <const uint8_t *> byte_str
            v.size = <uint32_t> len(value)
        else:
            raise ValueError(f'Unsupported value type: {type(value)}')
        if '!' not in topic:
            v.flags = c_jsdrv.JSDRV_UNION_FLAG_RETAIN
        with nogil:
            rc = c_jsdrv.jsdrv_publish(self._context, <char *> &topic_str[0], &v, timeout_ms)
        _handle_rc(rc, 'jsdrv_publish')

    def query(self, topic: str, timeout=None):
        """Query the value for a topic.

        :param topic: The topic name.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The value for the topic.
        :raise: On error.
        """
        cdef c_jsdrv.jsdrv_union_s v
        cdef char byte_str[1024]
        cdef const uint8_t[:] topic_str = topic.encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)

        v.type = c_jsdrv.JSDRV_UNION_BIN
        v.size = 1024
        v.value.str = byte_str
        with nogil:
            rc = c_jsdrv.jsdrv_query(self._context, <char *> &topic_str[0], &v, timeout_ms)
        _handle_rc(rc, 'jsdrv_query')
        return _jsdrv_union_to_py(&v)

    def device_paths(self, timeout=None):
        """List the currently connected devices.

        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The list of device path strings.
        """
        s = self.query('@/list', timeout)
        if not len(s):
            return []
        return sorted(s.split(','))

    def subscribe(self, topic: str, flags, fn, timeout=None):
        """Subscribe to receive topic updates.

        :param self: The driver instance.
        :param topic: Subscribe to this topic string.
        :param flags: The flags or list of flags for this subscription.
            The flags can be int32 jsdrv_subscribe_flag_e or string
            mnemonics, which are:

            - pub: Subscribe to normal values
            - pub_retain: Subscribe to normal values and immediately publish
              all matching retained values.  With timeout, this function does
              not return successfully until all retained values have been
              published.
            - metadata_req: Subscribe to metadata requests (not normally useful).
            - metadata_rsp: Subscribe to metadata updates.
            - metadata_rsp_retain: Subscribe to metadata updates and immediately
              publish all matching retained metadata values.
            - query_req: Subscribe to all query requests (not normally useful).
            - query_rsp: Subscribe to all query responses.
            - return_code: Subscribe to all return code responses.

        :param fn: The function to call on each publish.  Note that python
            dynamically constructs bound methods.  To unsubscribe a method,
            provide the exact  same bound method instance to unsubscribe.
        :param timeout: The timeout in float seconds to wait for this operation
            to complete.  None waits the default amount.
            0 does not wait and subscription will occur asynchronously.
        :raise RuntimeError: on subscribe failure.
        """
        cdef const uint8_t[:] topic_str = topic.encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef int32_t c_flags = 0
        cdef void * fn_ptr = <void *> fn

        if isinstance(flags, str):
            c_flags = _SUBSCRIBE_FLAG_LOOKUP[flags.lower()]
        elif isinstance(flags, (list, tuple)) and isinstance(flags[0], str):
            for f in flags:
                c_flags |= _SUBSCRIBE_FLAG_LOOKUP[f.lower()]
        else:
            c_flags = <int32_t> int(flags)
        self._subscribers.add((topic, fn))
        with nogil:
            rc = c_jsdrv.jsdrv_subscribe(self._context, <char *> &topic_str[0], c_flags, _on_cmd_publish1_cbk, fn_ptr, timeout_ms)
        _handle_rc(rc, 'jsdrv_subscribe')

    def unsubscribe(self, topic, fn, timeout=None):
        """Unsubscribe from a topic.

        :param topic: The topic name string.
        :param fn: The function previously provided to :func:`subscribe`.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :raise: On error.
        """
        cdef const uint8_t[:] topic_str = topic.encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef void * fn_ptr = <void *> fn

        self._subscribers.discard((topic, fn))
        with nogil:
            rc = c_jsdrv.jsdrv_unsubscribe(self._context, <char *> &topic_str[0], _on_cmd_publish1_cbk, fn_ptr, timeout_ms)
        _handle_rc(rc, 'jsdrv_unsubscribe')

    def unsubscribe_all(self, fn, timeout=None):
        """Unsubscribe a callback from all topics.

        :param fn: The function previously provided to :func:`subscribe`.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :raise: On error.
        """
        cdef int32_t timeout_ms = _timeout_validate(timeout)

        remove_list = [(t, f) for t, f in self._subscribers if f == fn]
        for item in remove_list:
            self._subscribers.discard(item)
        with nogil:
            rc = c_jsdrv.jsdrv_unsubscribe_all(self._context, _on_cmd_publish1_cbk, <void *> fn, timeout_ms)
        _handle_rc(rc, 'jsdrv_unsubscribe_all')

    def open(self, device_prefix, mode=None, timeout=None):
        """Open an attached device.

        :param device_prefix: The prefix name for the device.
        :param mode: The open mode which is one of:
            * 'defaults': Reconfigure the device for default operation.
            * 'restore': Update our state with the current device state.
            * 'raw': Open the device in raw mode for development or firmware update.
            * None: equivalent to 'defaults'.
        :param timeout: The timeout in seconds.  None uses the default timeout.
        """

        cdef const uint8_t[:] topic_str
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_union_s v
        memset(&v, 0, sizeof(v))

        if isinstance(mode, str):
            mode = mode.lower()
        mode = _DEVICE_OPEN_MODES[mode]

        while device_prefix[-1] == '/':
            device_prefix = device_prefix[:-1]
        topic = device_prefix + "/@/!open"
        topic_str = topic.encode('utf-8')

        v.type = c_jsdrv.JSDRV_UNION_I32
        v.value.i32 = mode

        with nogil:
            rc = c_jsdrv.jsdrv_publish(self._context, <char *> &topic_str[0], &v, timeout_ms);
        _handle_rc(rc, 'jsdrv_open')

    def close(self, device_prefix, timeout=None):
        """Close an attached device.

        :param device_prefix: The prefix name for the device.
        :param timeout: The timeout in seconds.  None uses the default timeout.
        """
        cdef const uint8_t[:] topic_str
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_union_s v

        while device_prefix[-1] == '/':
            device_prefix = device_prefix[:-1]
        topic = device_prefix + "/@/!close"
        topic_str = topic.encode('utf-8')
        v.type = c_jsdrv.JSDRV_UNION_I32
        v.value.i32 = 0
        with nogil:
            rc = c_jsdrv.jsdrv_publish(self._context, <char *> &topic_str[0], &v, timeout_ms);
        _handle_rc(rc, 'jsdrv_close')


cdef void _on_cmd_publish2_cbk(void * user_data, const char * topic, const c_jsdrv.jsdrv_union_s * value) with gil:
    cdef object fn = <object> user_data
    topic_str = topic.decode('utf-8')
    v = _jsdrv_union_to_py(value)
    # print(f'{topic_str} = {v}')
    fn(topic_str, v)


cdef void _on_cmd_publish1_cbk(void * user_data, const char * topic, const c_jsdrv.jsdrv_union_s * value) nogil:
    _on_cmd_publish2_cbk(user_data, topic, value)

# https://cython.readthedocs.io/en/latest/src/userguide/external_C_code.html#acquiring-and-releasing-the-gil
cdef void _on_log_recv(void * user_data, const c_jsdrv.jsdrv_log_header_s * header,
                       const char * filename, const char * message) nogil:
    with gil:
        fname = filename.decode('utf-8')
        msg = message.decode('utf-8')
        lvl = _log_level_c_to_py[header[0].level]
        if _log_c.isEnabledFor(lvl):
            record = _log_c.makeRecord(_log_c_name, lvl, fname, header[0].line, msg, [], None)
            _log_c.handle(record)
