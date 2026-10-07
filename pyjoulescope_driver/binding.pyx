# SPDX-FileCopyrightText: Copyright 2022-2025 Jetperch LLC
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

"""
Python binding for the native Joulescope driver implementation.
"""

# See https://cython.readthedocs.io/en/latest/index.html
# https://cython.readthedocs.io/en/latest/src/userguide/external_C_code.html#acquiring-and-releasing-the-gil


# cython: boundscheck=True, wraparound=True, nonecheck=True, overflowcheck=True, cdivision=True, embedsignature=True

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int8_t, int16_t, int32_t, int64_t
from libc.float cimport DBL_MAX
from libc.math cimport isfinite, NAN
from libc.string cimport memset, strcpy

from collections.abc import Iterable, Mapping
from .device_path import DevicePath
from . import device_filter
from .stdmsg import StdMsg
import json
import logging
import numpy as np
include "module.pxi"
import threading
import time
import warnings
cimport numpy as np
from . cimport c_jsdrv


__all__ = ['Driver', 'calibration_hash']
np.import_array()                           # initialize numpy before use
_log_c_name = 'jsdrv'
_log_c = logging.getLogger(_log_c_name)
_log = logging.getLogger(__name__)


class ElementType:
    """The element type enumeration."""
    UNDEFINED = 0       #: Undefined
    INT = 2             #: signed integer
    UINT = 3            #: unsigned integer
    FLOAT = 4           #: floating point


class Field:
    """The field enumeration."""
    UNDEFINED = 0       #: undefined
    CURRENT   = 1       #: current
    VOLTAGE   = 2       #: voltage
    POWER     = 3       #: power
    RANGE     = 4       #: current or voltage range
    GPI       = 5       #: general purpose input
    UART      = 6       #: UART
    RAW       = 7       #: raw ADC data


_element_type_to_prefix = {
    ElementType.INT: 'i',
    ElementType.UINT: 'u',
    ElementType.FLOAT: 'f',
}


_field_to_meta = {
    #field         [name,            field, units, use_index]
    Field.CURRENT: ['current',       'i',   'A',   False],
    Field.VOLTAGE: ['voltage',       'v',   'V',   False],
    Field.POWER:   ['power',         'p',   'W',   False],
    Field.RANGE:   ['current_range', 'r',   None,  False],
    Field.GPI:     ['gpi',           '',    None,  True],
    Field.UART:    ['uart',          'u',   None,  True],
    Field.RAW:     ['raw',           'z',   None,  True],
}


_TIME_MAP_GET_DTYPE = np.dtype({
    'names': ['offset_time', 'offset_counter', 'counter_rate'],
    'formats': ['i8', 'u8', 'f8']
})


cdef class TimeMap:
    """Python wrapper for jsdrv_tmap_s instance.

    Each instance owns its underlying jsdrv_tmap_s.  TimeMap values
    returned from driver responses are independent snapshots; copy()
    and deepcopy() produce further independent instances.
    """

    cdef c_jsdrv.jsdrv_tmap_s * this

    def __len__(self):
        return c_jsdrv.jsdrv_tmap_length(self.this)

    @staticmethod
    cdef factory(c_jsdrv.jsdrv_tmap_s * this):
        """Wrap an existing jsdrv_tmap_s, taking ownership of it."""
        cdef TimeMap instance = TimeMap.__new__(TimeMap)
        instance.this = this
        return instance

    @staticmethod
    def factory_new(initial_size=0):
        cdef c_jsdrv.jsdrv_tmap_s * this = c_jsdrv.jsdrv_tmap_alloc(initial_size)
        return TimeMap.factory(this)

    def __dealloc__(self):
        c_jsdrv.jsdrv_tmap_free(self.this)
        self.this = NULL

    def __copy__(self):
        return TimeMap.factory(c_jsdrv.jsdrv_tmap_copy(self.this))

    def __deepcopy__(self, memo):
        return TimeMap.factory(c_jsdrv.jsdrv_tmap_copy(self.this))

    def __enter__(self):
        # Kept as a no-op for backward compatibility with the previous
        # reader-locked API.  The instance is already an owned snapshot,
        # so no synchronization is required to query it.
        return self

    def __exit__(self, type, value, traceback):
        return None

    def sample_id_to_timestamp(self, sample_id):
        cdef int64_t r
        if isinstance(sample_id, Iterable):
            sz = len(sample_id)
            result = np.empty(sz, dtype=np.int64)
            for idx, s in enumerate(sample_id):
                if c_jsdrv.jsdrv_tmap_sample_id_to_timestamp(self.this, s, &r):
                    raise ValueError(f'invalid timestamp {s}')
                result[idx] = r
            return result
        else:
            c_jsdrv.jsdrv_tmap_sample_id_to_timestamp(self.this, sample_id, &r)
            return int(r)

    def timestamp_to_sample_id(self, timestamp):
        cdef uint64_t r
        if isinstance(timestamp, Iterable):
            sz = len(timestamp)
            result = np.empty(sz, dtype=np.uint64)
            for idx, t in enumerate(timestamp):
                if c_jsdrv.jsdrv_tmap_timestamp_to_sample_id(self.this, t, &r):
                    raise ValueError(f'invalid timestamp {t}')
                result[idx] = r
            return result
        else:
            c_jsdrv.jsdrv_tmap_timestamp_to_sample_id(self.this, timestamp, &r)
            return int(r)

    def time_map_get(self, index=None):
        """Get the time map data.

        :param index: The index for the time map which is one of:
            - None: return all time maps in the instance.
            - int index: Return only this index.
            - (int start_index, int end_index): Return this range.
              start_index is inclusive, end_index is exclusive.

        :return: Return an np.ndarray with the the named columns
            ['offset_time', 'offset_counter', 'counter_rate'].
            Access as either a[0]['offset_counter'] or a[0][1].
        """
        cdef c_jsdrv.jsdrv_time_map_s time_map
        n = len(self)
        if index is None:
            index = 0, n
        elif isinstance(index, Iterable):
            index = int(index[0]), int(index[1])
        else:
            index = int(index)
            if index < 0:
                index = n + index
            index = index, index + 1
        i0, i1 = index
        if i0 < 0:
            i0 = n + i0
        if i1 < 0:
            i1 = n + i1
        if i0 > i1:
            i0, i1 = 0, 0
        elif i1 > n:
            raise ValueError(f'index out of range: {index}')
        k = i1 - i0
        out = np.empty(k, dtype=_TIME_MAP_GET_DTYPE)
        for i in range(k):
            c_jsdrv.jsdrv_tmap_get(self.this, i + i0, &time_map)
            out[i][0] = time_map.offset_time
            out[i][1] = time_map.offset_counter
            out[i][2] = time_map.counter_rate
        return out


#: The scale for the statistics charge and energy int_value fields.
_STATISTICS_I128_SCALE = 2.0 ** -c_jsdrv.JSDRV_STATISTICS_I128_Q


def _i128_to_int(high, low):
    """Convert a two's complement 128-bit integer to a Python int.

    :param high: The most significant 64 bits as an unsigned integer.
    :param low: The least significant 64 bits as an unsigned integer.
    :return: The signed Python int.
    """
    i = (int(high) << 64) | int(low)
    if int(high) & (1 << 63):
        i -= 1 << 128
    return i


cdef object _parse_buffer_info(c_jsdrv.jsdrv_buffer_info_s * info):
    element_prefix = _element_type_to_prefix[info[0].element_type]
    name, field_name, units, use_index = _field_to_meta[info[0].field_id]
    if use_index:
        name = f'{name}[{info[0].index:d}]'
        if not field_name:
            field_name = f'{info[0].index:d}'
        else:
            field_name = f'{field_name}[{info[0].index:d}]'
    v = {
        'version': info[0].version,
        'name': name,
        'field': field_name,
        'field_id': info[0].field_id,
        'index': info[0].index,
        'element_type': f'{element_prefix}{info[0].element_size_bits:d}',
        'element_type_enum': info[0].element_type,
        'element_size_bits': info[0].element_size_bits,
        'units': units,
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
        'time_map': {
            'offset_time': info[0].time_map.offset_time,
            'offset_counter': info[0].time_map.offset_counter,
            'counter_rate': info[0].time_map.counter_rate,
        },
        'decimate_factor': info[0].decimate_factor,
        'tmap': None,
    }
    if (info[0].tmap):
        # C owns info[0].tmap and frees it when the message is torn down.
        # Copy so the Python TimeMap has an independent instance.
        v['tmap'] = TimeMap.factory(c_jsdrv.jsdrv_tmap_copy(info[0].tmap))
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
        info = v['info']
        element_type = info['element_type']
        if element_type == 'f32':
            shape[0] = <np.npy_intp> length
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_FLOAT32, <void *> &r[0].data[0])
        elif element_type == 'u1':
            shape[0] = <np.npy_intp> ((length + 7) // 8)
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> &r[0].data[0])
        elif element_type == 'u4':
            shape[0] = <np.npy_intp> ((length + 1) // 2)
            ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> &r[0].data[0])
        else:
            _log_c.error('unsupported sample format')
            return
        v['data_type'] = info['element_type']
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
    rsp_topic = _topic_validate(r['rsp_topic'], 'rsp_topic')
    cdef const uint8_t[:] rsp_topic_str = rsp_topic.encode('utf-8')
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
    strcpy(s.rsp_topic, <const char *> &rsp_topic_str[0])
    s.rsp_id = int(r['rsp_id'])

    u8_ptr = <uint8_t *> &s;
    return bytes(u8_ptr[:sizeof(s)])



cdef object _jsdrv_union_to_py(const c_jsdrv.jsdrv_union_s * value):
    cdef c_jsdrv.jsdrv_stream_signal_s * stream;
    cdef np.npy_intp shape[1]
    cdef uint8_t[:] u8_mem
    cdef int32_t idx
    cdef uint8_t * u8_ptr;
    cdef uint16_t * u16_ptr;
    cdef uint32_t * u32_ptr;
    cdef uint64_t * u64_ptr;
    t = value[0].type
    try:
        if t == c_jsdrv.JSDRV_UNION_STR:
            v = value[0].value.str.decode('utf-8')
        elif t == c_jsdrv.JSDRV_UNION_JSON:
            v = value[0].value.str.decode('utf-8')
            try:
                v = json.loads(v)
            except Exception:
                _log.warning('invalid JSON payload replaced with None: %r', v[:256])
                v = None
        elif t == c_jsdrv.JSDRV_UNION_BIN:
            if value[0].app == c_jsdrv.JSDRV_PAYLOAD_TYPE_STREAM:
                stream = <c_jsdrv.jsdrv_stream_signal_s *> &(value[0].value.bin[0])
                el = (stream[0].element_type, stream[0].element_size_bits)
                v = {
                    'sample_id': stream[0].sample_id,
                    'utc': c_jsdrv.jsdrv_time_from_counter(&stream[0].time_map, stream[0].sample_id),
                    'field_id': stream[0].field_id,
                    'index': stream[0].index,
                    'sample_rate': stream[0].sample_rate,
                    'decimate_factor': stream[0].decimate_factor,
                    'time_map': {
                        'offset_time': stream[0].time_map.offset_time,
                        'offset_counter': stream[0].time_map.offset_counter,
                        'counter_rate': stream[0].time_map.counter_rate,
                    }
                }
                if el == (c_jsdrv.JSDRV_DATA_TYPE_FLOAT, 32):  # float32
                    shape[0] = <np.npy_intp> stream[0].element_count
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_FLOAT32, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_UINT, 1):  # uint1, 8 per uint8
                    shape[0] = <np.npy_intp> ((stream[0].element_count + 7) / 8)
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_UINT8, <void *> stream[0].data)
                    v['data'] = ndarray.copy()
                elif el == (c_jsdrv.JSDRV_DATA_TYPE_INT, 32):  # int32
                    shape[0] = <np.npy_intp> stream[0].element_count
                    ndarray = np.PyArray_SimpleNewFromData(1, shape, np.NPY_INT32, <void *> stream[0].data)
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
                # u8 decimate_factor saturates at 255; decimate_factor32 has the
                # exact value (0 from older producers - fall back)
                decimate_factor = stats[0].decimate_factor32
                if decimate_factor == 0:
                    decimate_factor = stats[0].decimate_factor
                samples_full_rate = stats[0].block_sample_count * decimate_factor
                sample_id_start = stats[0].block_sample_id
                sample_id_end = stats[0].block_sample_id + samples_full_rate
                t_start = sample_id_start / sample_freq
                t_delta = samples_full_rate / sample_freq
                charge = _i128_to_int(stats[0].charge_i128[1], stats[0].charge_i128[0])
                energy = _i128_to_int(stats[0].energy_i128[1], stats[0].energy_i128[0])
                v = {
                    'time': {
                        'samples': {'value': [sample_id_start, sample_id_end], 'units': 'samples'},
                        'utc': {
                            'value': [
                                c_jsdrv.jsdrv_time_from_counter(&stats[0].time_map, sample_id_start),
                                c_jsdrv.jsdrv_time_from_counter(&stats[0].time_map, sample_id_end),
                            ],
                            'units': 'time64',
                        },
                        'sample_freq': {'value': sample_freq, 'units': 'Hz'},
                        'range': {'value': [t_start, t_start + t_delta], 'units': 's'},
                        'delta': {'value': t_delta, 'units': 's'},
                        'decimate_factor': {'value': decimate_factor, 'units': 'samples'},
                        'decimate_sample_count': {'value': stats[0].block_sample_count, 'units': 'samples'},
                        'accum_samples': {'value': [stats[0].accum_sample_id, sample_id_end], 'units': 'samples'},
                        'time_map': {
                            'offset_time': stats[0].time_map.offset_time,
                            'offset_counter': stats[0].time_map.offset_counter,
                            'counter_rate': stats[0].time_map.counter_rate,
                        }
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
                            'int_scale': _STATISTICS_I128_SCALE,
                            'units': 'C',
                        },
                        'energy': {
                            'value': stats[0].energy_f64,
                            'int_value': energy,
                            'int_scale': _STATISTICS_I128_SCALE,
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
        elif t == c_jsdrv.JSDRV_UNION_STDMSG:
            if value[0].size < 8:
                _log_c.warning('STDMSG malformed: size %d < 8 byte header', value[0].size)
                v = None
            else:
                u8_ptr = <uint8_t *> &value[0].value.bin[0]
                u16_ptr = <uint16_t *> &value[0].value.bin[0]
                u32_ptr = <uint32_t *> &value[0].value.bin[0]
                u64_ptr = <uint64_t *> &value[0].value.bin[0]
                v = {
                    'dtype': 'stdmsg',
                    'version': u16_ptr[0],
                    'version_major': value[0].value.bin[0],
                    'version_minor': value[0].value.bin[1],
                    'metadata': u32_ptr[1],
                }
                if value[0].value.bin[2] == 0x05: # MB_STDMSG_COMM_STATS
                    if value[0].size < 44:
                        _log_c.warning('MB_STDMSG_COMM_STATS malformed: size %d < 44', value[0].size)
                        v = None
                    else:
                        v['stdmsg_type'] = 'comm_stats'
                        v['counter_start'] = u64_ptr[1]
                        v['counter_end'] = u64_ptr[2]
                        v['frames'] = u32_ptr[6]
                        v['bytes'] = u32_ptr[7]
                        v['frame_error'] = u32_ptr[8]
                        v['link_error'] = u32_ptr[9]
                        v['app_error'] = u32_ptr[10]
                elif value[0].value.bin[2] == 0x07:  # MB_STDMSG_MEM
                    if value[0].size < 32:
                        _log_c.warning('MB_STDMSG_MEM malformed: size %d < 32 byte header', value[0].size)
                        v = None
                    else:
                        v['transaction_id'] = u32_ptr[2]
                        v['target'] = u8_ptr[12]
                        v['operation'] = u8_ptr[13]
                        v['flags'] = u8_ptr[14]
                        v['status'] = u8_ptr[15]
                        v['timeout_ms'] = u16_ptr[8]
                        v['offset'] = u32_ptr[5]
                        v['length'] = u32_ptr[6]
                        v['delay_us'] = u32_ptr[7]
                        data_size = value[0].size - 32
                        v['data'] = bytes(u8_ptr[32:32 + data_size])
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
        _log_c.exception(ex)
        v = None
    return v


class ErrorCode:
    """The error code enumeration."""
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
    8: (8, 'INVALID_MESSAGE_LENGTH', 'The message length is invalid'),
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
    'INVALID_MESSAGE_LENGTH': (8, 'INVALID_MESSAGE_LENGTH', 'The message length is invalid'),
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
    """The log level enumeration."""
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
    """The available subscribe flags."""
    NONE = 0                #: No flags (always 0).
    RETAIN = (1 << 0)       #: Immediately forward retained PUB and/or METADATA, depending upon JSDRV_PUBSUB_SFLAG_PUB and JSDRV_PUBSUB_SFLAG_METADATA_RSP.
    PUB = (1 << 1)          #: Do not receive normal topic publish.
    METADATA_REQ = (1 << 2) #: Subscribe to receive metadata requests like "%", "a/b/%", and "a/b/c%".
    METADATA_RSP = (1 << 3) #: Subscribe to receive metadata responses like "a/b/c$.
    QUERY_REQ = (1 << 4)    #: Subscribe to receive query requests like "&", "a/b/&", and "a/b/c&".
    QUERY_RSP = (1 << 5)    #: Subscribe to receive query responses like "a/b/c?".
    RETURN_CODE = (1 << 6)  #: Subscribe to receive return code messages like "a/b/c#".


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
cdef bint _log_level_defaulted = False
_TIMEOUT_MS_DEFAULT = 1000
_TIMEOUT_MS_INIT = 5000


cdef int32_t _timeout_validate(value, default=None):
    if default is None:
        default = _TIMEOUT_MS_DEFAULT
    if value is None:
        return default
    return <int32_t> (float(value) * 1000)


cdef object _topic_validate(topic, name='topic'):
    # Accept str subclasses, such as DevicePath, which a Cython str
    # annotation would reject.
    if not isinstance(topic, str):
        raise TypeError(f'{name} must be str, not {type(topic).__name__}')
    return topic


cdef object _device_prefix_validate(device_prefix):
    device_prefix = _topic_validate(device_prefix, 'device_prefix').rstrip('/')
    if not device_prefix:
        raise ValueError('device_prefix must not be empty')
    return device_prefix


def _handle_rc(rc, src, cause=None):
    if rc:
        name = c_jsdrv.jsdrv_error_code_name(rc).decode('utf-8')
        description = c_jsdrv.jsdrv_error_code_description(rc).decode('utf-8')
        if cause:
            cause = f' | {cause}'
        else:
            cause = ''
        if rc == ErrorCode.TIMED_OUT:
            raise TimeoutError(f'{src} timed out{cause}')
        else:
            raise RuntimeError(f'{src} failed {rc} {name} | {description}{cause}')


cdef _log_level_default():
    """Default the native log level to ERROR, once per process.

    Errors that no caller receives, such as a publish with timeout=0 that
    the device rejects, only surface in the log.  Respect any level that the
    application set before the first Driver.
    """
    global _log_level_defaulted
    if not _log_level_defaulted:
        _log_level_defaulted = True
        if c_jsdrv.jsdrv_log_level_get() == LogLevel.OFF:
            c_jsdrv.jsdrv_log_level_set(LogLevel.ERROR)


cdef class Driver:
    """The Joulescope driver class.

    :param timeout: The optional timeout for open.
        None (default) uses the default timeout.

    The native driver log forwards to the Python "jsdrv" logger at
    :attr:`log_level`, which defaults to "error".

    The application must finalize each Driver instance, either with a
    context manager or by calling :meth:`finalize` directly.  Finalize
    stops the driver threads and closes the devices that the instance
    opened.  A Driver that is never finalized keeps its threads running
    and its devices open, which is a program bug.  Typical usage::

        from pyjoulescope_driver import Driver

        with Driver() as d:
            device_path = d.find_one_device('js320', brand='joulescope')
            with d.open(device_path, mode='restore') as device:
                device.publish('s/i/range/mode', 'auto')
                with device.subscribe('s/stats/value', 'pub', on_statistics):
                    time.sleep(1.0)

    Use :meth:`device_paths`, :meth:`find_devices` and
    :meth:`find_one_device` to select devices, and :meth:`device_watch`
    to track device additions and removals.  :meth:`open` returns a
    :class:`DeviceContext`, and :meth:`subscribe` returns a
    :class:`SubscribeContext`.
    """
    cdef c_jsdrv.jsdrv_context_s * _context
    # The C library holds a borrowed pointer to each callback, so these
    # lists hold the reference that keeps each callback alive.
    cdef object _subscribers  # list of (topic, fn) registered with C
    cdef object _released     # fn objects that C may still call
    cdef object _opened       # set of device paths opened by this instance

    def __init__(self, timeout=None):
        global _driver_count
        self._context = NULL
        cdef int32_t rc
        timeout_ms = _timeout_validate(timeout, _TIMEOUT_MS_INIT)
        with nogil:
            rc = c_jsdrv.jsdrv_initialize(&self._context, NULL, timeout_ms)
        _handle_rc(rc, 'jsdrv_initialize')
        self._subscribers = []
        self._released = []
        self._opened = set()
        if _driver_count == 0:
            c_jsdrv.jsdrv_log_initialize()
            c_jsdrv.jsdrv_log_register(_on_log_recv, NULL)
        _log_level_default()
        _driver_count += 1

    cdef c_jsdrv.jsdrv_context_s * _context_get(self) except NULL:
        if self._context == NULL:
            raise RuntimeError('Driver is finalized')
        return self._context

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.finalize()

    @property
    def log_level(self):
        """The current log level.

        See :class:`LogLevel`.
        """
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
        """Finalize the driver and release all resources.

        Finalize first closes each device that this instance opened and
        that is still connected, ignoring close errors.  It then stops the
        driver, which stops all subscriptions.  Afterwards, close,
        unsubscribe and unsubscribe_all do nothing, and the other methods
        raise RuntimeError.  Exiting a ``with Driver() as d:`` block calls
        finalize.

        :param timeout: The timeout in seconds.  None (default) uses
            the driver's finalize timeout, which allows time for every
            device and backend thread to exit.
        """
        global _driver_count
        if self._context == NULL:
            return  # already finalized
        self._close_opened()
        cdef c_jsdrv.jsdrv_context_s * context = self._context
        self._context = NULL
        timeout_ms = _timeout_validate(timeout, 0)  # 0 = C default
        with nogil:
            c_jsdrv.jsdrv_finalize(context, timeout_ms)
        _driver_count -= 1
        if _driver_count == 0:
            with nogil:
                c_jsdrv.jsdrv_log_finalize()
        # The C library no longer calls any callback.
        self._subscribers.clear()
        self._released.clear()

    def _close_opened(self):
        """Close the devices opened by this instance, ignoring errors."""
        if not self._opened:
            return
        try:
            connected = set(self.device_paths())
        except Exception:
            _log.exception('finalize: could not list devices')
            connected = set()
        for device_path in sorted(self._opened):
            if device_path not in connected:
                continue  # removed, so close would only time out
            try:
                self.close(device_path)
            except Exception:
                _log.warning('finalize: could not close %s', device_path, exc_info=True)
        self._opened.clear()

    def publish(self, topic, value, timeout=None):
        """Publish a value to a topic.

        :param topic: The topic string.
        :param value: The value, which must pass validation for the topic.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout and waits for the return code.  0 does
            not wait, so publish cannot raise for an invalid topic or value.
            The driver logs the error at ERROR level to the "jsdrv" logger
            instead, such as "publish u/js320/8W2A/s/nope failed: 16 NOT_FOUND".
            Use a blocking publish when the caller must handle the error.
        :raise: On error.
        """
        cdef c_jsdrv.jsdrv_union_s v
        cdef char * byte_str
        cdef const uint8_t[:] topic_str = _topic_validate(topic).encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_context_s * context = self._context_get()

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
        elif isinstance(value, StdMsg):
            byte_str = value
            v.type = c_jsdrv.JSDRV_UNION_STDMSG
            v.value.bin = <const uint8_t *> byte_str
            v.size = <uint32_t> len(value)
        elif isinstance(value, bytes):
            byte_str = value
            v.type = c_jsdrv.JSDRV_UNION_BIN
            v.value.bin = <const uint8_t *> byte_str
            v.size = <uint32_t> len(value)
        elif isinstance(value, float):
            v.type = c_jsdrv.JSDRV_UNION_F64
            v.value.f64 = value
        else:
            raise ValueError(f'Unsupported value type: {type(value)}')
        if '!' not in topic:
            v.flags = c_jsdrv.JSDRV_UNION_FLAG_RETAIN
        with nogil:
            rc = c_jsdrv.jsdrv_publish(context, <char *> &topic_str[0], &v, timeout_ms)
        _handle_rc(rc, 'jsdrv_publish', topic)

    def query(self, topic, timeout=None):
        """Query the value for a topic.

        :param topic: The topic name.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The value for the topic.
        :raise: On error.
        """
        cdef c_jsdrv.jsdrv_union_s v
        cdef char byte_str[1024]
        cdef const uint8_t[:] topic_str = _topic_validate(topic).encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_context_s * context = self._context_get()

        memset(&v, 0, sizeof(v))
        v.type = c_jsdrv.JSDRV_UNION_BIN
        v.size = 1024
        v.value.str = byte_str
        with nogil:
            rc = c_jsdrv.jsdrv_query(context, <char *> &topic_str[0], &v, timeout_ms)
        _handle_rc(rc, 'jsdrv_query', topic)
        return _jsdrv_union_to_py(&v)

    def publish_and_wait(self, publish_topic, publish_value,
                         response_topic, timeout=None, match=None):
        """Publish a value and wait for a response on another topic.

        :param publish_topic: The topic to publish to.
        :param publish_value: The value to publish.
        :param response_topic: The topic to subscribe to for
            the response.
        :param timeout: The timeout in float seconds.
            None (default) uses the default timeout.
        :param match: The optional callable(value) that returns True
            for the expected response.  Other responses, such as those
            for another transaction, are ignored.  None (default)
            accepts the first response.
        :return: The value received on response_topic.
        :raises TimeoutError: If no response arrives in time.
        """
        _topic_validate(publish_topic, 'publish_topic')
        _topic_validate(response_topic, 'response_topic')
        if timeout is None:
            timeout = _TIMEOUT_MS_DEFAULT / 1000.0
        event = threading.Event()
        result = [None]

        def on_response(topic, value):
            if event.is_set():
                return
            if match is not None and not match(value):
                return
            result[0] = value
            event.set()

        self.subscribe(response_topic, 'pub', on_response)
        try:
            self.publish(publish_topic, publish_value, timeout=0)
            if not event.wait(timeout):
                raise TimeoutError(
                    f'publish_and_wait timed out: '
                    f'publish={publish_topic}, '
                    f'response={response_topic}'
                )
            return result[0]
        finally:
            self.unsubscribe(response_topic, on_response)

    def device_paths(self, specs=None, brand=None, timeout=None):
        """List the currently connected devices.

        :param specs: The device specifications.  None (default) lists
            all devices.  See :func:`device_filter.find`.
        :param brand: The case-insensitive brand, such as "Joulescope".
            None (default) lists all brands.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The sorted list of matching :class:`DevicePath`
            instances, which are str instances.
        :raise TypeError: If specs or brand has an invalid type.
        :raise ValueError: If brand is not supported.

        Before 2.5.0, the first positional argument was timeout.  A numeric
        specs is still treated as timeout, with a DeprecationWarning.
        """
        if isinstance(specs, (int, float)) and not isinstance(specs, bool):
            warnings.warn('device_paths(timeout) is deprecated, use device_paths(timeout=...)',
                          DeprecationWarning, stacklevel=2)
            specs, timeout = None, specs
        s = self.query('@/list', timeout)
        paths = sorted(DevicePath(p) for p in s.split(',')) if len(s) else []
        return device_filter.find(paths, specs, brand)

    def find_one_device(self, specs=None, brand=None, timeout=None):
        """Find exactly one connected device.

        :param specs: The device specifications.  None (default) matches
            all devices.  See :func:`device_filter.find`.
        :param brand: The case-insensitive brand, such as "Joulescope".
            None (default) matches all brands.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The matching :class:`DevicePath`.
        :raise device_filter.DeviceFilterError: If zero or multiple
            devices match.
        """
        return device_filter.find_one(self.device_paths(timeout=timeout), specs, brand)

    def find_devices(self, specs=None, brand=None, timeout=None):
        """Find the connected devices.

        This method is an alias for :meth:`device_paths`, which reads
        better next to :meth:`find_one_device`.
        """
        return self.device_paths(specs, brand, timeout)

    def device_watch(self, on_add, on_remove, specs=None, brand=None, timeout=None):
        """Watch for matching devices.

        :param on_add: The callable(device_path) called with the
            :class:`DevicePath` for each added device.  Before returning,
            this method calls on_add for each connected device.
        :param on_remove: The callable(device_path) called with the
            :class:`DevicePath` for each removed device.
        :param specs: The device specifications.  None (default) matches
            all devices.  See :func:`device_filter.find`.
        :param brand: The case-insensitive brand, such as "Joulescope".
            None (default) matches all brands.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :return: The :class:`SubscribeContext`.  Call unsubscribe() or
            exit its context to stop watching.

        This method calls on_add for the connected devices from the
        calling thread, so these on_add calls may use any Driver method.
        Afterwards, the driver thread calls on_add and on_remove, once for
        each device addition and removal, in order.  Callbacks from the
        driver thread must not block on the driver, so use timeout=0 or
        defer blocking work, such as :meth:`open`, to another thread.
        """
        device_filter.find([], specs, brand)  # validate specs and brand
        lock = threading.Lock()
        pending = []  # events received while listing the connected devices
        listing = True
        known = set()

        def process(is_add, device_path):
            device_path = DevicePath(device_path)
            if is_add:
                if device_path in known or not device_filter.find([device_path], specs, brand):
                    return
                known.add(device_path)
                on_add(device_path)
            elif device_path in known:
                known.discard(device_path)
                on_remove(device_path)

        def on_event(is_add, value):
            with lock:
                if listing:
                    pending.append((is_add, value))
                    return
            process(is_add, value)

        def on_add_event(topic, value):
            on_event(True, value)

        def on_remove_event(topic, value):
            on_event(False, value)

        # Subscribe before listing, so no device is missed.  Hold events
        # until listing completes, then process them in order.
        watch = self.subscribe('@/!add', 'pub', on_add_event, timeout)
        try:
            watch.extend(self.subscribe('@/!remove', 'pub', on_remove_event, timeout))
            for device_path in self.device_paths(timeout=timeout):
                process(True, device_path)
            while True:
                with lock:
                    if not pending:
                        listing = False
                        break
                    event = pending.pop(0)
                process(*event)
        except BaseException:
            watch.unsubscribe()
            raise
        return watch

    def subscribe(self, topic, flags, fn, timeout=None):
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
        :return: The :class:`SubscribeContext`, which unsubscribes on
            unsubscribe() or when used as a context manager::

                with d.subscribe(topic, 'pub', fn):
                    ...

        :raise RuntimeError: on subscribe failure.
        """
        cdef const uint8_t[:] topic_str = _topic_validate(topic).encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef int32_t c_flags = 0
        cdef void * fn_ptr = <void *> fn
        cdef c_jsdrv.jsdrv_context_s * context = self._context_get()

        if isinstance(flags, str):
            c_flags = _SUBSCRIBE_FLAG_LOOKUP[flags.lower()]
        elif isinstance(flags, (list, tuple)) and isinstance(flags[0], str):
            for f in flags:
                c_flags |= _SUBSCRIBE_FLAG_LOOKUP[f.lower()]
        else:
            c_flags = <int32_t> int(flags)
        self._subscribers.append((topic, fn))
        with nogil:
            rc = c_jsdrv.jsdrv_subscribe(context, <char *> &topic_str[0], c_flags, _on_cmd_publish_cbk, fn_ptr, timeout_ms)
        _handle_rc(rc, 'jsdrv_subscribe', topic)
        return SubscribeContext(self, [(topic, fn)])

    def unsubscribe(self, topic, fn, timeout=None):
        """Unsubscribe from a topic.

        :param topic: The topic name string.
        :param fn: The function previously provided to :func:`subscribe`.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :raise: On error.
        """
        cdef const uint8_t[:] topic_str = _topic_validate(topic).encode('utf-8')
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        if self._context == NULL:
            return  # finalize stopped all subscriptions
        targets = self._registered(fn, topic)
        cdef void * fn_ptr = <void *> targets[0]

        with nogil:
            rc = c_jsdrv.jsdrv_unsubscribe(self._context, <char *> &topic_str[0], _on_cmd_publish_cbk, fn_ptr, timeout_ms)
        self._release(targets[:1], rc, timeout_ms, topic)
        _handle_rc(rc, 'jsdrv_unsubscribe', topic)

    def unsubscribe_all(self, fn, timeout=None):
        """Unsubscribe a callback from all topics.

        :param fn: The function previously provided to :func:`subscribe`.
        :param timeout: The timeout in seconds.  None (default) uses
            the default timeout.
        :raise: On error.
        """
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef int32_t rc = 0
        cdef int32_t rc_target
        cdef void * fn_ptr

        if self._context == NULL:
            return  # finalize stopped all subscriptions
        targets = self._registered(fn)
        for target in targets:
            fn_ptr = <void *> target
            with nogil:
                rc_target = c_jsdrv.jsdrv_unsubscribe_all(self._context, _on_cmd_publish_cbk, fn_ptr, timeout_ms)
            rc = rc or rc_target
        self._release(targets, rc, timeout_ms)
        _handle_rc(rc, 'jsdrv_unsubscribe_all')

    def _registered(self, fn, topic=None):
        """Find the callback objects registered with C that match fn.

        Python creates a new bound method object on each attribute access,
        but C matches callbacks by pointer.  Unsubscribe must use the
        object passed to subscribe, which is identical or equal to fn.

        :param fn: The callback function.
        :param topic: The topic to match, or None for all topics.
        :return: The list of distinct matching objects, identical objects
            first.  When nothing matches, the list is [fn].
        """
        entries = [f for t, f in self._subscribers if topic is None or t == topic]
        targets = []
        for f in [f for f in entries if f is fn] + [f for f in entries if f is not fn and f == fn]:
            if not any(f is x for x in targets):
                targets.append(f)
        return targets if len(targets) else [fn]

    def _release(self, targets, rc, timeout_ms, topic=None):
        """Release the references held for unsubscribed callbacks.

        :param targets: The callback objects passed to C unsubscribe.
        :param rc: The C unsubscribe return code.
        :param timeout_ms: The unsubscribe timeout.
        :param topic: The unsubscribed topic, or None for all topics.
        """
        def match(t, f):
            return (topic is None or t == topic) and any(f is x for x in targets)
        removed = [f for t, f in self._subscribers if match(t, f)]
        self._subscribers[:] = [(t, f) for t, f in self._subscribers if not match(t, f)]
        if timeout_ms == 0 or rc:
            # C has not confirmed the unsubscribe, so it may still call
            # these callbacks.  Hold them until C confirms a later one.
            self._released.extend(removed)
        else:
            # C processes commands in order, so it also completed every
            # earlier unconfirmed unsubscribe.
            self._released.clear()

    def open(self, device_prefix, mode=None, timeout=None):
        """Open an attached device.

        :param device_prefix: The prefix name for the device.
        :param mode: The open mode which is one of:

            * 'defaults': Push state to the device: the host's retained
              value for each writable topic when present, else the
              metadata default.
            * 'restore': Push nothing; adopt the device's current state
              into the host cache.
            * 'raw': Open the device in raw mode for development or firmware update.
            * None: equivalent to 'defaults'.

        :param timeout: The timeout in seconds.  None uses the default timeout.
        :return: The :class:`DeviceContext` for the open device, which
            closes the device on close() or when used as a context manager::

                with d.open(device_path) as device:
                    device.publish('s/i/range/mode', 'auto')

            Finalize closes all devices that remain open.
        """

        cdef const uint8_t[:] topic_str
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_union_s v
        cdef c_jsdrv.jsdrv_context_s * context = self._context_get()
        memset(&v, 0, sizeof(v))

        if isinstance(mode, str):
            mode = mode.lower()
        mode = _DEVICE_OPEN_MODES[mode]

        device_prefix = _device_prefix_validate(device_prefix)
        topic = device_prefix + "/@/!open"
        topic_str = topic.encode('utf-8')

        v.type = c_jsdrv.JSDRV_UNION_I32
        v.value.i32 = mode

        with nogil:
            rc = c_jsdrv.jsdrv_publish(context, <char *> &topic_str[0], &v, timeout_ms);
        _handle_rc(rc, 'jsdrv_open', device_prefix)
        self._opened.add(device_prefix)
        return DeviceContext(self, device_prefix)

    def close(self, device_prefix, timeout=None):
        """Close an attached device.

        :param device_prefix: The prefix name for the device.
        :param timeout: The timeout in seconds.  None uses the default timeout.
        """
        cdef const uint8_t[:] topic_str
        cdef int32_t timeout_ms = _timeout_validate(timeout)
        cdef c_jsdrv.jsdrv_union_s v

        device_prefix = _device_prefix_validate(device_prefix)
        if self._context == NULL:
            return  # finalize closed all devices
        self._opened.discard(device_prefix)
        topic = device_prefix + "/@/!close"
        topic_str = topic.encode('utf-8')
        # Zero flags and app: jsdrvp_msg_free interprets app.
        memset(&v, 0, sizeof(v))
        v.type = c_jsdrv.JSDRV_UNION_I32
        v.value.i32 = 0
        with nogil:
            rc = c_jsdrv.jsdrv_publish(self._context, <char *> &topic_str[0], &v, timeout_ms);
        _handle_rc(rc, 'jsdrv_close', device_prefix)


def _exit_cleanup(fn, exc_type, name):
    """Call a context manager's cleanup on exit.

    When the block raised, log a cleanup error instead of raising it, so
    that it does not replace the block's exception.
    """
    if exc_type is None:
        fn()
        return
    try:
        fn()
    except Exception:
        _log.warning('%s failed while handling %s', name, exc_type.__name__,
                     exc_info=True)


class SubscribeContext:
    """The subscriptions returned by :meth:`Driver.subscribe`.

    Call :meth:`unsubscribe` or exit the context to unsubscribe.  After
    :meth:`Driver.finalize`, unsubscribe does nothing.
    """

    def __init__(self, driver, subscriptions):
        self._driver = driver
        self._subscriptions = list(subscriptions)  # list of (topic, fn)

    def extend(self, other):
        """Move the subscriptions from another SubscribeContext into this one."""
        self._subscriptions.extend(other._subscriptions)
        other._subscriptions.clear()

    def _discard(self, topic, fn):
        """Forget one subscription that was unsubscribed elsewhere.

        :return: True if found, False otherwise.
        """
        for idx, (t, f) in enumerate(self._subscriptions):
            if t == topic and (f is fn or f == fn):
                del self._subscriptions[idx]
                return True
        return False

    def unsubscribe(self, timeout=None):
        """Unsubscribe.  Calling unsubscribe again does nothing."""
        subscriptions, self._subscriptions = self._subscriptions, []
        for topic, fn in subscriptions:
            self._driver.unsubscribe(topic, fn, timeout)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        _exit_cleanup(self.unsubscribe, exc_type, 'unsubscribe')


class DeviceContext:
    """An open device returned by :meth:`Driver.open`.

    The topic arguments are relative to the device path, such as
    "s/i/range/mode".  Call :meth:`close` or exit the context to
    unsubscribe all subscriptions made through this instance and then
    close the device.
    """

    def __init__(self, driver, device_path):
        self._driver = driver
        self.device_path = DevicePath(device_path)
        self._subscriptions = []  # SubscribeContext instances from subscribe
        self._closed = False

    def __repr__(self):
        return f'DeviceContext({self.device_path!r})'

    def topic(self, topic):
        """Get the absolute topic.

        :param topic: The topic relative to the device path.
        :return: The absolute topic, "{device_path}/{topic}".
        """
        return f'{self.device_path}/{_topic_validate(topic)}'

    def publish(self, topic, value, timeout=None):
        """Publish a value.  See :meth:`Driver.publish`."""
        return self._driver.publish(self.topic(topic), value, timeout)

    def query(self, topic, timeout=None):
        """Query a value.  See :meth:`Driver.query`."""
        return self._driver.query(self.topic(topic), timeout)

    def publish_and_wait(self, publish_topic, publish_value, response_topic,
                         timeout=None, match=None):
        """Publish and wait for a response.  See :meth:`Driver.publish_and_wait`."""
        publish_topic = self.topic(_topic_validate(publish_topic, 'publish_topic'))
        response_topic = self.topic(_topic_validate(response_topic, 'response_topic'))
        return self._driver.publish_and_wait(publish_topic, publish_value, response_topic,
                                             timeout=timeout, match=match)

    def subscribe(self, topic, flags, fn, timeout=None):
        """Subscribe to a topic.  See :meth:`Driver.subscribe`.

        :meth:`close` unsubscribes the subscription, if still active.

        :return: The :class:`SubscribeContext`.
        """
        subscription = self._driver.subscribe(self.topic(topic), flags, fn, timeout)
        self._subscriptions = [x for x in self._subscriptions if x._subscriptions]
        self._subscriptions.append(subscription)
        return subscription

    def unsubscribe(self, topic, fn, timeout=None):
        """Unsubscribe from a topic.  See :meth:`Driver.unsubscribe`."""
        topic = self.topic(topic)
        for subscription in self._subscriptions:
            if subscription._discard(topic, fn):
                break
        return self._driver.unsubscribe(topic, fn, timeout)

    def close(self, timeout=None):
        """Unsubscribe all subscriptions made through this instance, and
        then close the device.  Calling close again does nothing.

        See :meth:`Driver.close`.
        """
        if self._closed:
            return
        self._closed = True
        subscriptions, self._subscriptions = self._subscriptions, []
        try:
            for subscription in subscriptions:
                subscription.unsubscribe(timeout)
        finally:
            self._driver.close(self.device_path, timeout)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        _exit_cleanup(self.close, exc_type, f'{self.device_path} close')


cdef void _on_cmd_publish_cbk(void * user_data, const char * topic,
                              const c_jsdrv.jsdrv_union_s * value) noexcept with gil:
    cdef object fn = <object> user_data
    try:
        topic_str = topic.decode('utf-8')
    except:
        _log_c.exception('_on_cmd_publish_cbk could not convert topic to utf-8')
        return
    try:
        v = _jsdrv_union_to_py(value)
        # print(f'{topic_str} = {v}')
        fn(topic_str, v)
    except:
        _log_c.exception(f'_on_cmd_publish_cbk({topic_str})')


cdef void _on_log_recv(void * user_data, const c_jsdrv.jsdrv_log_header_s * header,
                       const char * filename, const char * message) noexcept with gil:
    lvl = _log_level_c_to_py[header[0].level]
    if _log_c.isEnabledFor(lvl):
        fname = filename.decode('utf-8')
        msg = message.decode('utf-8')
        record = _log_c.makeRecord(_log_c_name, lvl, fname, header[0].line, msg, [], None)
        _log_c.handle(record)


def calibration_hash(msg):
    cdef const uint32_t[:] msg_u32
    cdef uint32_t[:] hash_u32

    if (len(msg) % 32) != 0:
        raise ValueError('msg length must be multiple of 32 bytes')

    msg_np = np.empty(len(msg) // 4, dtype=np.uint32)
    msg_np.view(dtype=np.uint8)[:] = msg
    msg_u32 = msg_np

    hash = np.zeros(16, dtype=np.uint32)
    hash_u32 = hash
    rc = c_jsdrv.jsdrv_calibration_hash(&msg_u32[0], len(msg), &hash_u32[0])
    if rc:
        raise ValueError(f'jsdrv_calibration_hash failed: {rc}')
    return hash
