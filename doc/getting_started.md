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

# Getting started

This guide shows the basic lifecycle for the Joulescope driver in Python
(pyjoulescope_driver) and in C (joulescope_driver): find a device, open it,
configure it, receive statistics and streaming samples, and shut down
cleanly.  It applies to the JS110, JS220 and JS320.  The driver also has
Node.js bindings in `node_api/`, which this guide does not cover.

The payload formats for statistics and streaming samples are in
[streaming_topics.md](streaming_topics.md).  The per-model topic references
are [js220.txt](js220.txt) and [js320.json](js320.json).


## Install

Python:

    python -m pip install pyjoulescope_driver
    python -m pyjoulescope_driver scan

On Linux, also install the udev rules described in the
[README](../README.md).  `scan` lists the connected device paths.

C: build the static library, the `jsdrv` command-line tool and the examples
with CMake as described in the README.  `example/quickstart.c` is the C
version of this guide and builds as `jsdrv_quickstart`.


## Concepts

* **Publish-subscribe**: every setting, command and measurement is a topic
  with a value.  Set a value with publish, read a retained value with
  query, and receive updates with subscribe.
* **Device path**: `{backend}/{model}/{serial_number}`, such as
  `u/js320/8W2A`.  Device topics are `{device_path}/{topic}`, such as
  `u/js320/8W2A/s/i/range/mode`.  The driver lists every USB device it
  recognizes, including non-Joulescope devices such as `u/mb/93NP`, so
  select by brand or model.
* **Retained and command topics**: a topic whose last segment starts with
  `!` is a command or a data stream and is not retained.  All other topics
  retain their last value.  Subscribing with the retain flag delivers the
  retained value immediately.
* **Topic prefixes**: `h/` host-side settings such as the sample rate,
  `s/` sensor streaming and ranging, `c/` controller, and `@/` driver and
  device lifecycle.  The full list for a connected device:
  `python -m pyjoulescope_driver metadata`.
* **Open modes**: `defaults` pushes the host's retained values, else the
  metadata defaults, to the device.  `restore` reads the device's current
  state into the host and leaves it unchanged.  `raw` is for firmware
  update only.
* **Callbacks run on the driver thread**.  Keep them short, copy the data
  out, and never call a blocking driver method from inside one.  Publish
  with `timeout=0` from a callback.
* **Finalize**: each Python `Driver` must be finalized, by the context
  manager or `finalize()`, and each C context must be passed to
  `jsdrv_finalize()`.  Finalize stops the driver threads and closes the
  devices that are still open.


## Python

The program below measures for two seconds on the first Joulescope.  It
works on all three models.

```python
import time
from pyjoulescope_driver import Driver


def on_statistics(topic, value):
    # value is a nested dict, see doc/streaming_topics.md
    i = value['signals']['current']['avg']['value']
    v = value['signals']['voltage']['avg']['value']
    print(f'{i:.6f} A, {v:.3f} V')


def on_current(topic, value):
    # value['data'] is a numpy float32 array in amperes
    print(f"{len(value['data'])} samples from sample_id {value['sample_id']}")


with Driver() as d:
    # Select by brand, or by spec such as 'js320', '8W2A' or 'js320-8W2A'.
    device_path = d.find_one_device(brand='joulescope')
    print(f'Using {device_path}: model={device_path.model}')

    with d.open(device_path, mode='defaults') as device:
        # Topics passed to device are relative to device_path.
        if device_path.model == 'js110':
            device.publish('s/i/range/select', 'auto')
        else:
            device.publish('s/i/range/mode', 'auto')
        for signal in ['i', 'v', 'p']:
            device.publish(f's/{signal}/ctrl', 1)      # enable streaming
        device.publish('s/stats/scnt', 500_000)         # samples per block
        device.publish('s/stats/ctrl', 1)               # enable statistics
        device.subscribe('s/stats/value', 'pub', on_statistics)
        device.subscribe('s/i/!data', 'pub', on_current)
        time.sleep(2.0)
    # Leaving the block unsubscribes and closes the device.
# Leaving the block finalizes the driver.
```

The same operations with absolute topics:

```python
d.publish(f'{device_path}/s/i/range/mode', 'auto')
d.subscribe(f'{device_path}/s/stats/value', 'pub', on_statistics)
value = d.query(f'{device_path}/s/i/range/mode')
```

Topic values accept the metadata option names, such as `'auto'`, or the
underlying numeric value.  Boolean topics accept `1` and `0`.
`python -m pyjoulescope_driver values --device js320` prints every retained
value for a device, and `metadata` prints each topic's type, default and
options.

See the `Driver`, `DeviceContext` and `SubscribeContext` docstrings for the
full API, and `pyjoulescope_driver/entry_points/` for complete programs.


## C

`example/quickstart.c` is the complete, buildable program.  It uses only
the public headers and performs these steps:

```c
#include "jsdrv.h"
#include "jsdrv/union.h"
#include "jsdrv/topic.h"
#include "jsdrv/os_thread.h"

struct jsdrv_context_s * context = NULL;
jsdrv_initialize(&context, NULL, JSDRV_TIMEOUT_MS_INIT);

// List the devices: a comma-separated string of device paths.
char devices[1024];
struct jsdrv_union_s list = jsdrv_union_str(devices);
list.size = sizeof(devices);
jsdrv_query(context, JSDRV_MSG_DEVICE_LIST, &list, JSDRV_TIMEOUT_MS_DEFAULT);

// Open, configure and subscribe.  Build each topic from the device path.
jsdrv_open(context, device, JSDRV_DEVICE_OPEN_MODE_DEFAULTS, JSDRV_TIMEOUT_MS_DEFAULT);
struct jsdrv_topic_s t;
jsdrv_topic_set(&t, device);
jsdrv_topic_append(&t, "s/i/range/mode");
jsdrv_publish(context, t.topic, &jsdrv_union_cstr_r("auto"), JSDRV_TIMEOUT_MS_DEFAULT);
jsdrv_topic_set(&t, device);
jsdrv_topic_append(&t, "s/stats/ctrl");
jsdrv_publish(context, t.topic, &jsdrv_union_u8_r(1), JSDRV_TIMEOUT_MS_DEFAULT);
jsdrv_topic_set(&t, device);
jsdrv_topic_append(&t, "s/stats/value");
jsdrv_subscribe(context, t.topic, JSDRV_SFLAG_PUB, on_statistics, user_data,
                JSDRV_TIMEOUT_MS_DEFAULT);

jsdrv_thread_sleep_ms(2000);

jsdrv_close(context, device, JSDRV_TIMEOUT_MS_DEFAULT);
jsdrv_finalize(context, 0);
```

Subscriber callbacks receive a `struct jsdrv_union_s`.  Statistics and
streaming payloads are binary with `value->app` set to the payload type.
Cast `value->value.bin` to `struct jsdrv_statistics_s` or
`struct jsdrv_stream_signal_s` as described in
[streaming_topics.md](streaming_topics.md).

Link against the `jsdrv` CMake target.  `include/jsdrv.h` documents the
API, and `example/jsdrv/` contains the larger `jsdrv` tool.


## Command-line tools

| Command | Purpose |
| ------- | ------- |
| `python -m pyjoulescope_driver scan` | List the connected device paths. |
| `python -m pyjoulescope_driver info` | Driver, package and device summary. |
| `python -m pyjoulescope_driver values -d js320` | Every retained value for one device. |
| `python -m pyjoulescope_driver metadata` | Topic types, defaults and options. |
| `python -m pyjoulescope_driver set -d js320 s/i/range/mode=auto` | Set a value. |
| `python -m pyjoulescope_driver statistics --duration 2` | Print statistics. |
| `python -m pyjoulescope_driver record --duration 10s out.jls` | Capture to a JLS file. |
| `python -m pyjoulescope_driver program` | Update JS220 or JS320 firmware. |
| `jsdrv scan`, `jsdrv statistics` | The C equivalents. |

Each command accepts `--help`.  Most take `--device` (`-d`) with a device
path, `backend/model` such as `u/js320/`, model, serial number or
`model-serial_number`, and the generic
commands also take `--brand`.


## Model differences

| | JS110 | JS220 | JS320 |
| --- | --- | --- | --- |
| Native sample rate | 2 MHz | 1 MHz | 1 MHz |
| Sample rate topic | `h/fs` | `h/fs` | `h/fs` |
| Current range topic | `s/i/range/select` | `s/i/range/mode` | `s/i/range/mode` |
| Range after `defaults` open | auto | **off** | **off** |
| Statistics | `s/sstats/value`, or host-side (see below) | `s/stats/value` | `s/stats/value` |
| `s/stats/scnt` units | 2 Msps samples | 1 Msps samples | 1 Msps samples |
| `measure` entry point | yes | yes | no |

The JS220 and JS320 open with the current range off, so publish
`s/i/range/mode` `auto` (or a fixed range) before expecting current
readings.  The JS220 driver does not yet push metadata defaults on a
`defaults` open, so the instrument keeps its power-on state.

The JS220 and JS320 compute statistics on the instrument.  The JS110
`s/sstats/value` is on-instrument at a fixed 2 Hz without standard
deviation.  For host-side JS110 statistics, enable `s/i/ctrl`, `s/v/ctrl`
and `s/p/ctrl`, then `s/stats/ctrl`, as the example above does.


## Pitfalls

* **Blocking in a callback deadlocks or times out.**  Callbacks run on the
  driver thread.  Queue the data to another thread, and publish with
  `timeout=0` (`JSDRV_TIMEOUT_MS_ASYNC`) if you must publish from one.
* **A C timeout of 0 means asynchronous** for `jsdrv_publish`,
  `jsdrv_subscribe`, `jsdrv_unsubscribe`, `jsdrv_open` and `jsdrv_close`,
  but means the default timeout for `jsdrv_initialize`, `jsdrv_query` and
  `jsdrv_finalize`.  Use `JSDRV_TIMEOUT_MS_DEFAULT` to block.
* **Finalize is mandatory.**  A `Driver` that is never finalized leaves
  threads running and devices open.  Use `with Driver() as d:`.
* **The JS220 and JS320 current range defaults to off.**  See the table
  above.
* **`defaults` vs `restore`.**  `defaults` overwrites the device
  configuration.  Use `restore` to observe a device that another process
  or the user configured, then set only what you need.
* **Device paths are not only Joulescopes.**  `device_paths()` returns
  every recognized device.  Pass `brand='joulescope'` or a model.
* **`record` needs pyjls**, which is not a declared dependency:
  `python -m pip install pyjls`.
* **Time is time64**: a signed 64-bit integer with 2^30 ticks per second
  since 2018-01-01 00:00:00 UTC.  Convert with `pyjoulescope_driver.time64`
  or `include/jsdrv/time.h`.
* **Linux permissions.**  Without the udev rules, `scan` lists nothing.
* **Multiple Joulescopes** make `find_one_device(brand='joulescope')` raise
  `DeviceFilterError`.  Pass a serial number or model.
* **Closing a removed device** raises in Python and returns an error in C.
  Subscribe to `@/!remove`, or use `Driver.device_watch`, to track removal.
