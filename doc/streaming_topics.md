<!--
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
-->

# Streaming and statistics topics

This document describes the data that subscribers receive from the
measurement topics, in both the Python and C bindings.  See
[getting_started.md](getting_started.md) for the lifecycle around them.
The authoritative C definitions are in `include/jsdrv.h`, and the Python
conversion is `_jsdrv_union_to_py` in `pyjoulescope_driver/binding.pyx`.

All topics below are relative to the device path, such as
`u/js320/8W2A/s/i/!data`.  Subscribe with the `pub` flag.  The callbacks
run on the driver thread, so copy what you need and return quickly.


## Topics

Each stream has an enable topic and a data topic.  Publish `1` to the
enable topic to start the stream and `0` to stop it.

| Enable | Data | Field | Element | Units |
| ------ | ---- | ----- | ------- | ----- |
| `s/i/ctrl` | `s/i/!data` | current | f32 | A |
| `s/v/ctrl` | `s/v/!data` | voltage | f32 | V |
| `s/p/ctrl` | `s/p/!data` | power | f32 | W |
| `s/i/range/ctrl` | `s/i/range/!data` | current range | u4 | range index |
| `s/gpi/{n}/ctrl` | `s/gpi/{n}/!data` | general-purpose input n | u1 | 0 or 1 |
| `s/stats/ctrl` | `s/stats/value` | statistics | struct | see below |

The JS220 and JS320 also stream the trigger input as `s/gpi/7/!data`.
The JS320 also streams the UART (`s/uart/!data`, u8) and the raw ADCs
(`s/adc/{n}/!data`, i32) for development.  The JS110 also provides
`s/sstats/value`, the on-instrument statistics without standard deviation.
The per-model topic references are [js220.txt](js220.txt) and
[js320.json](js320.json), and `python -m pyjoulescope_driver metadata`
prints the reference for a connected device.

Related settings:

* `h/fs`: the sample rate in Hz for the `!data` streams.  The native rate
  is 2 MHz on the JS110 and 1 MHz on the JS220 and JS320.  Lower rates
  decimate on the host.
* `s/stats/scnt`: the number of native-rate samples per statistics block.
  500000 gives 2 blocks per second on the JS220 and JS320 and 4 per
  second on the JS110.
* `s/stats/!clear`: restart the charge and energy accumulators.


## Streaming samples

Every `!data` message carries a contiguous block of samples for one
signal.  Blocks arrive in order with increasing `sample_id`.  `sample_id`
counts at `sample_rate`, the native rate, and increments by
`decimate_factor` for each element in the block, so the sample id of
element `k` is `sample_id + k * decimate_factor`.

### Python

The callback receives `(topic, value)` where `value` is a dict:

| Key | Type | Meaning |
| --- | ---- | ------- |
| `sample_id` | int | Native-rate sample id of the first element. |
| `utc` | int | time64 timestamp of `sample_id`, from `time_map`. |
| `field_id` | int | `Field` enum: 1 current, 2 voltage, 3 power, 4 range, 5 gpi, 6 uart, 7 raw. |
| `index` | int | Channel index within the field, such as the GPI number. |
| `sample_rate` | int | The native sample rate in Hz for `sample_id`. |
| `decimate_factor` | int | Native samples per element. |
| `time_map` | dict | `offset_time`, `offset_counter`, `counter_rate`: sample id to UTC. |
| `data` | numpy array | The samples, see below. |

`data` depends on the element type:

| Element | numpy dtype | Layout |
| ------- | ----------- | ------ |
| f32 | float32 | One value per sample. |
| u4 (current range) | uint8 | One value per sample, unpacked by the binding. |
| u1 (GPI) | uint8 | Packed, 8 samples per byte, `(count + 7) // 8` bytes. |
| i32 (raw ADC) | int32 | One value per sample. |
| u8 (UART) | uint8 | One byte per element. |

The binding copies `data`, so the callback may keep the array.

### C

The callback receives `const struct jsdrv_union_s * value` with
`value->type == JSDRV_UNION_BIN` and
`value->app == JSDRV_PAYLOAD_TYPE_STREAM`.  Cast the payload:

```c
const struct jsdrv_stream_signal_s * s =
    (const struct jsdrv_stream_signal_s *) value->value.bin;
if (s->version != 1) {
    return;  // read only the v1 fields, or reject
}
if ((s->element_type == JSDRV_DATA_TYPE_FLOAT) && (s->element_size_bits == 32)) {
    const float * samples = (const float *) s->data;
    for (uint32_t k = 0; k < s->element_count; ++k) {
        // samples[k] is at sample id s->sample_id + k * s->decimate_factor
    }
}
```

`struct jsdrv_stream_signal_s` has the same fields as the Python dict,
plus `element_type` (`jsdrv_element_type_e`), `element_size_bits` and
`element_count`.  The u4 current range data is packed two samples per
byte, low nibble first; the Python binding unpacks it.  The payload is
only valid during the callback, so copy the samples out.


## Statistics

`s/stats/value` publishes one message per block of `s/stats/scnt` samples.
Each block has the average, standard deviation, minimum and maximum of
current, voltage and power, plus charge and energy accumulated since the
last `s/stats/!clear` (or open).

### Python

The callback receives a nested dict.  Every leaf is
`{'value': ..., 'units': ...}`:

```python
value['time']['samples']['value']        # [first, last) native sample ids
value['time']['utc']['value']            # [first, last) time64
value['time']['sample_freq']['value']    # native rate, Hz
value['time']['range']['value']          # [start, end] seconds since sample 0
value['time']['delta']['value']          # block duration, seconds
value['time']['decimate_factor']['value']
value['time']['decimate_sample_count']['value']
value['time']['accum_samples']['value']  # [first, last) ids for charge, energy
value['signals']['current']['avg' | 'std' | 'min' | 'max' | 'p2p']['value']  # A
value['signals']['current']['integral']['value']   # C over this block
value['signals']['voltage']['avg' | 'std' | 'min' | 'max' | 'p2p']['value']  # V
value['signals']['power']['avg' | 'std' | 'min' | 'max' | 'p2p']['value']    # W
value['signals']['power']['integral']['value']     # J over this block
value['accumulators']['charge']['value']  # C since accum start, float
value['accumulators']['energy']['value']  # J since accum start, float
value['source']                            # 'sensor'
```

The accumulators also carry `int_value` and `int_scale`, the exact 128-bit
integer accumulator and its scale, 2^-52 on every model, for applications
that must avoid floating-point drift over long runs.  Prefer `value`
unless you need exact integration.

### C

`value->type == JSDRV_UNION_BIN` and
`value->app == JSDRV_PAYLOAD_TYPE_STATISTICS`:

```c
const struct jsdrv_statistics_s * s =
    (const struct jsdrv_statistics_s *) value->value.bin;
if (s->version != 1) {
    return;
}
printf("%g A, %g V, %g W, %g C, %g J\n",
       s->i_avg, s->v_avg, s->p_avg, s->charge_f64, s->energy_f64);
```

Fields: `block_sample_id` and `block_sample_count` locate the block,
`accum_sample_id` is where the accumulators started, `sample_freq` is the
native rate, and `decimate_factor32` is the exact decimation (the u8
`decimate_factor` saturates at 255).  `i_avg`, `i_std`, `i_min`, `i_max`
and the `v_` and `p_` equivalents are doubles.  `charge_f64` and
`energy_f64` are the accumulators as doubles; `charge_i128` and
`energy_i128` are the exact signed 128-bit two's complement integers as
little-endian u64 words with `JSDRV_STATISTICS_I128_Q` (52) fractional
bits on every model.  `time_map` converts sample ids to UTC.


## Time

* **time64**: a signed 64-bit integer with 2^30 ticks per second since
  2018-01-01 00:00:00 UTC.  Python: `pyjoulescope_driver.time64` has
  `SECOND`, `as_datetime`, `as_timestamp` and `now`.  C: `jsdrv/time.h`.
* **time_map**: `utc = offset_time + (sample_id - offset_counter) / counter_rate`
  in time64 units, computed by `jsdrv_time_from_counter()` in C and
  supplied as `utc` in the Python stream dict.  The driver updates the map
  as it tracks the instrument clock against the host clock.


## Buffers

The `m/` topics are the optional host-side sample buffer manager, which
stores streams in memory and answers time-range requests with samples or
summaries.  Its topic metadata is in `pyjoulescope_driver/host_params.json`
and `python -m pyjoulescope_driver metadata`.  The payloads are
`struct jsdrv_buffer_info_s`, `struct jsdrv_buffer_request_s` and
`struct jsdrv_buffer_response_s` in `include/jsdrv.h`, converted to dicts
by `_parse_buffer_info` and `_parse_buffer_rsp` in the binding.  It is
not needed for basic use.
