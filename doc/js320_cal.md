# JS320 Calibration (js320_cal)

## Overview

`js320_cal` is a per-device JS320 driver subsystem that performs
calibration slot operations (read/copy) and offset calibration sweeps
(current, voltage) over the existing open device. It is a sibling of
`js320_fwup` and `js320_jtag`: the same lifecycle hooks, the same
device-scoped topic surface, the same single-threaded event-driven
state machine.

The caller opens the JS320 with `jsdrv_open`, publishes a command to
`<device>/h/cal/!cmd`, and receives a final response on
`<device>/h/cal/!rsp`. Progress is published as JSON on
`<device>/h/cal/!status`. Slot reads return their 1024-byte record on
`<device>/h/cal/!data`.

Because the cal runs on the already-open device, the host can
keep streaming, the UI keeps its session, and offset cal cleanly saves
and restores the streaming-related state it mutates. The handler
captures host- and device-published values for the affected topics
passively via `handle_cmd` / `handle_publish`, so simply running cal
while the UI is in a normal streaming session is non-conflicting:
the UI's prior topic values are snapshotted at op entry and
republished at op completion.

## Topic surface

All topics are device-prefixed (the topic itself encodes the device).

| Topic                         | Direction | Payload                                |
| ----------------------------- | --------- | -------------------------------------- |
| `<device>/h/cal/!cmd`         | publish   | binary `jsdrv_cal_cmd_s` (12 bytes)    |
| `<device>/h/cal/!rsp`         | response  | binary `jsdrv_cal_rsp_s` (8 bytes)     |
| `<device>/h/cal/!status`      | retained  | JSON progress string                   |
| `<device>/h/cal/!data`        | publish   | 1024-byte slot record (slot_read only) |

### Status JSON shape

```json
{
  "op":    "slot_read|slot_copy|current_offset|voltage_offset",
  "state": "read|erase|write|verify|settle|capture",
  "pct":   12.5,
  "msg":   "free-form progress text"
}
```

`pct` is `0.0..100.0` (tenths precision). `state` is the active phase
of the state machine; the handler does not publish `idle` (no status
is emitted while idle). A successful operation ends with a publish on
`h/cal/!rsp` carrying `status = 0`. On error, the last status publish
describes the failure and `h/cal/!rsp` carries a nonzero
`JSDRV_ERROR_*` code.

### `h/cal/!cmd` payload

Defined in `include_private/jsdrv_prv/devices/js320/js320_cal.h`:

```c
struct jsdrv_cal_cmd_s {
    uint32_t transaction_id;
    uint8_t  op;                  // jsdrv_cal_op_e
    uint8_t  src_slot;            // jsdrv_cal_slot_e
    uint8_t  dst_slot;            // jsdrv_cal_slot_e
    uint8_t  flags;               // reserved = 0
    uint32_t samples_per_point;   // 0 = default (200000)
};
```

Pack with `struct.Struct('<IBBBBI')` from Python.

### `h/cal/!rsp` payload

```c
struct jsdrv_cal_rsp_s {
    uint32_t transaction_id;
    int32_t  status;     // 0 = success, JSDRV_ERROR_* on failure
};
```

`struct.Struct('<Ii')`.

## Operations

| op token         | numeric | result                                   |
| ---------------- | ------- | ---------------------------------------- |
| `slot_read`      | 0       | publish 1024-byte record on h/cal/!data  |
| `slot_copy`      | 1       | copy src_slot -> dst_slot                |
| `current_offset` | 2       | sweep current offsets, write ACTIVE      |
| `voltage_offset` | 3       | measure voltage offsets, write ACTIVE    |

### Slot identifiers

| Token   | Value | Notes                  |
| ------- | ----- | ---------------------- |
| ACTIVE  | 0     | runtime cal (writable) |
| TRIM2   | 1     | writable               |
| TRIM1   | 2     | writable               |
| FIELD   | 3     | writable               |
| LAB     | 4     | writable               |
| FACTORY | 5     | read-only              |

The fpga_mcu scans `ACTIVE → TRIM2 → TRIM1 → FIELD → LAB → FACTORY`
at boot and uses the first valid record (by `mb_check32_xxhash`).

### slot_copy policy

| dst       | allowed src                                 |
| --------- | ------------------------------------------- |
| `ACTIVE`  | `TRIM2`, `TRIM1`, `FIELD`, `LAB`, `FACTORY` |
| `TRIM1`   | `ACTIVE` only                               |
| `TRIM2`   | `ACTIVE` only                               |
| `FIELD`, `LAB`, `FACTORY` | rejected (read-only from the driver) |

Rejected combinations return `JSDRV_ERROR_PARAMETER_INVALID` on
`h/cal/!rsp` without touching flash.

### Offset cal mechanics

Both offset ops only touch the `offsets[]` array of the record.
`gains[]` and other fields are preserved. `create_time` is refreshed.
The 64-byte signature is overwritten with the driver tag (see below)
and `check32` is recomputed.

`current_offset` sweeps the 21-point upper-triangular `(i_select,
i_mux_select)` matrix for each of the three current ADCs. Each
`(i_select, i_mux_select)` point captures samples from ADC0 and ADC1
in one pass and ADC2 in a second pass (only two of the three ADC
channels can stream at once).

`voltage_offset` measures the voltage ADC at both ranges (15 V, then
2 V) and updates `offsets[70]` and `offsets[71]`.

The caller is responsible for arranging the physical input state
before triggering each op:

- **`current_offset`**: open the current jacks (zero current).
- **`voltage_offset`**: short v+ to v- (zero voltage).
- **`slot_copy` / `slot_read`**: no setup required.

`samples_per_point` overrides the default `200000` samples per
measurement point (`6 power-line cycles at 1 MHz`).

### Streaming state save / restore

During offset cal the handler mutates these device topics:

```
s/i/range/mode    s/i/range/select
s/i/i0_sel        s/i/i1_sel       s/i/i2_sel
s/v/range/mode    s/v/range/select
s/adc/0/sel       s/adc/0/ctrl
s/adc/1/sel       s/adc/1/ctrl
```

The handler observes host-published and device-published values for
these topics passively (via `handle_cmd` and `handle_publish`), takes
a snapshot at offset-cal entry, and re-publishes the snapshot before
sending the final `h/cal/!rsp`. Any of these topics whose values were
never observed in the current session are left at their cal-time
state; the host can re-establish them by republishing.

### Driver-generated signature pattern

The 64-byte signature field of records produced by the driver is:

| Bytes  | Value                                  |
| ------ | -------------------------------------- |
| 0..15  | `"JSDRV_OFFSET_CAL"` (no NUL)          |
| 16..19 | `JSDRV_VERSION_U32` (u32 LE)           |
| 20..63 | 0x00                                   |

The FPGA does not verify the ECDSA signature - only `check32` is
enforced. The magic exists so customer support can grep a record
dump and immediately recognise driver-generated cal vs PCTS factory
cal.

## Calibration record layout

The 1024-byte record is `struct js320_calibration_s` in
`../js320/firmware/fpga_common/include/calibration.h`. Key offsets:

| Bytes      | Field                  | Notes                                  |
| ---------- | ---------------------- | -------------------------------------- |
| 0..15      | header magic           | `"JS320cal\\x0D\\x0A \\x0A\\x1A\\xB2\\x1C"` |
| 16         | format_version         | 1                                      |
| 18..19     | vendor_id              | u16 LE                                 |
| 20..21     | product_id             | u16 LE                                 |
| 24..31     | serial_number          | UTF-8 NUL-padded                       |
| 32..39     | create_time            | i64 LE - jsdrv time64                  |
| 40..71     | source_info            | UTF-8 NUL-padded                       |
| 72..75     | cal_source_version     | u32 LE                                 |
| 88..375    | `offsets[72]`          | int32_t LE, 1Q31                       |
| 376..951   | `gains[72]`            | int64_t LE, 12Q52 (untouched by offset cal) |
| 952..1015  | signature              | see above                              |
| 1020..1023 | check32                | `mb_check32_xxhash` over first 1020 bytes |

The 72-entry offsets and gains arrays are laid out as
`channel * 24 + point_index`, where `channel ∈ {0,1,2}` are current
channels and the point index follows the upper-triangular sweep:

```
       mux=0  mux=1  mux=2  mux=3  mux=4  mux=5
i=0     0      1      2      3      4      5
i=1            6      7      8      9     10
i=2                  11     12     13     14
i=3                         15     16     17
i=4                                18     19
i=5                                       20
```

Voltage offsets live at `offsets[70]` (15 V range) and `offsets[71]`
(2 V range). Indices 21..23, 45..47, 69 are reserved (zero).

The 1Q31 offset matches the left-aligned i32 ADC sample directly: the
driver just stores the i32 mean of the captured samples.

## Caller flow

```c
// Caller maintains a monotonically increasing txn_id; the handler
// echoes it back on h/cal/!rsp so the caller can correlate.
static uint32_t txn_id = 0;

ROE(jsdrv_open(ctx, device_path, JSDRV_DEVICE_OPEN_MODE_RESUME,
               JSDRV_TIMEOUT_MS_DEFAULT));

jsdrv_subscribe(ctx, "<device>/h/cal/!status", JSDRV_SFLAG_PUB,
                on_status, NULL, 0);
jsdrv_subscribe(ctx, "<device>/h/cal/!data",   JSDRV_SFLAG_PUB,
                on_data, NULL, 0);
jsdrv_subscribe(ctx, "<device>/h/cal/!rsp",    JSDRV_SFLAG_PUB,
                on_rsp, NULL, 0);

struct jsdrv_cal_cmd_s cmd = {
    .transaction_id = ++txn_id,
    .op = JSDRV_CAL_OP_SLOT_READ,
    .src_slot = JSDRV_CAL_SLOT_FACTORY,
};
jsdrv_publish(ctx, "<device>/h/cal/!cmd",
              &jsdrv_union_bin((uint8_t *) &cmd, sizeof(cmd)), 0);

// wait for h/cal/!rsp callback
```

The publish to `h/cal/!cmd` is fire-and-forget (use `timeout=0`); the
`h/cal/!rsp` callback carries the final outcome. Do not pass a
nonzero timeout to `jsdrv_publish` for `h/cal/!cmd` - the handler
does not emit a return-code reply on `h/cal/!cmd#`, so the publish
would always block to its timeout.

`example/minibitty/cal.c` is the canonical C consumer. The same
shape applies to a Python wrapper using `pyjoulescope_driver.Driver`.

### Reference Python wrapper

```python
# pyjoulescope_driver/cal_js320.py
"""JS320 calibration via the per-device cal handler."""

import itertools
import json
import logging
import struct
import threading

from pyjoulescope_driver import Driver


_log = logging.getLogger(__name__)

OP_SLOT_READ      = 0
OP_SLOT_COPY      = 1
OP_CURRENT_OFFSET = 2
OP_VOLTAGE_OFFSET = 3

SLOT_ACTIVE  = 0
SLOT_TRIM2   = 1
SLOT_TRIM1   = 2
SLOT_FIELD   = 3
SLOT_LAB     = 4
SLOT_FACTORY = 5

SLOT_NAMES = {
    SLOT_ACTIVE:  'ACTIVE',  SLOT_TRIM2: 'TRIM2',  SLOT_TRIM1: 'TRIM1',
    SLOT_FIELD:   'FIELD',   SLOT_LAB:   'LAB',    SLOT_FACTORY: 'FACTORY',
}
SLOT_IDS = {v: k for k, v in SLOT_NAMES.items()}

CAL_RECORD_SIZE = 1024
CAL_SIGNATURE_MAGIC = b'JSDRV_OFFSET_CAL'

# 12-byte command, 8-byte response (matches struct sizes asserted in
# js320_cal.c).
_CMD = struct.Struct('<IBBBBI')
_RSP = struct.Struct('<Ii')

# Per-process monotonic transaction id; wraps within u32.
_TXN_COUNTER = itertools.count(1)


def _next_txn_id() -> int:
    return next(_TXN_COUNTER) & 0xFFFFFFFF


def _run_op(driver: Driver, device_path: str, op: int,
            src_slot: int = 0, dst_slot: int = 0,
            samples_per_point: int = 0,
            progress=None, timeout: float = 60.0,
            collect_data: bool = False):
    if progress is None:
        progress = lambda x, y: None

    txn_id = _next_txn_id()
    payload = _CMD.pack(
        txn_id, op, src_slot, dst_slot, 0, int(samples_per_point))

    done = threading.Event()
    state = {'status': None, 'last_status': ''}
    data_buf = {'bytes': None}

    def on_status(topic, value):
        if isinstance(value, str):
            state['last_status'] = value
            try:
                entry = json.loads(value)
                pct = float(entry.get('pct', 0.0))
                progress(pct / 100.0,
                         f"{entry.get('state','?')}: {entry.get('msg','')}")
            except (ValueError, TypeError):
                pass

    def on_data(topic, value):
        if collect_data and isinstance(value, (bytes, bytearray)):
            data_buf['bytes'] = bytes(value)

    def on_rsp(topic, value):
        if isinstance(value, (bytes, bytearray)) and len(value) >= 8:
            rsp_txn, status = _RSP.unpack_from(bytes(value), 0)
            if rsp_txn != txn_id:
                return  # stale response from a prior op
            state['status'] = int(status)
        else:
            state['status'] = -1
        done.set()

    progress(0.0, 'Starting JS320 calibration')
    driver.subscribe(f'{device_path}/h/cal/!status', 'pub', on_status)
    driver.subscribe(f'{device_path}/h/cal/!data',   'pub', on_data)
    driver.subscribe(f'{device_path}/h/cal/!rsp',    'pub', on_rsp)
    try:
        driver.publish(f'{device_path}/h/cal/!cmd', payload)
        if not done.wait(timeout):
            raise TimeoutError(
                f'JS320 cal timed out (last status: {state["last_status"]})')
    finally:
        driver.unsubscribe(f'{device_path}/h/cal/!status', on_status)
        driver.unsubscribe(f'{device_path}/h/cal/!data',   on_data)
        driver.unsubscribe(f'{device_path}/h/cal/!rsp',    on_rsp)

    if state['status']:
        raise RuntimeError(
            f'JS320 cal failed: status={state["status"]} '
            f'(last status: {state["last_status"]})')
    progress(1.0, 'Complete')
    return data_buf['bytes']


def slot_read(driver, device_path, slot, **kwargs):
    if isinstance(slot, str):
        slot = SLOT_IDS[slot.upper()]
    data = _run_op(driver, device_path, OP_SLOT_READ,
                   src_slot=slot, collect_data=True, **kwargs)
    if data is None or len(data) != CAL_RECORD_SIZE:
        raise RuntimeError(
            f'slot_read returned {len(data) if data else 0} bytes')
    return data


def slot_copy(driver, device_path, src_slot, dst_slot, **kwargs):
    if isinstance(src_slot, str):
        src_slot = SLOT_IDS[src_slot.upper()]
    if isinstance(dst_slot, str):
        dst_slot = SLOT_IDS[dst_slot.upper()]
    _run_op(driver, device_path, OP_SLOT_COPY,
            src_slot=src_slot, dst_slot=dst_slot, **kwargs)


def current_offset_cal(driver, device_path, samples_per_point=0, **kwargs):
    # ~50 s with default 200k samples; ~12 s with 5000 samples on HIL.
    kwargs.setdefault('timeout', 90.0)
    _run_op(driver, device_path, OP_CURRENT_OFFSET,
            samples_per_point=samples_per_point, **kwargs)


def voltage_offset_cal(driver, device_path, samples_per_point=0, **kwargs):
    # ~5 s with default 200k samples; ~3 s with 5000 samples on HIL.
    kwargs.setdefault('timeout', 30.0)
    _run_op(driver, device_path, OP_VOLTAGE_OFFSET,
            samples_per_point=samples_per_point, **kwargs)
```

### Decoding a record

```python
import struct

CAL_HEADER_MAGIC = b'JS320cal\x0D\x0A \x0A\x1A\xB2\x1C\x00'
CAL_SIGNATURE_MAGIC = b'JSDRV_OFFSET_CAL'

def parse_record(rec: bytes) -> dict:
    assert len(rec) == 1024
    header_ok = (rec[:16] == CAL_HEADER_MAGIC)
    format_version = rec[16]
    vendor_id, product_id = struct.unpack_from('<HH', rec, 18)
    serial = rec[24:32].split(b'\x00', 1)[0].decode('utf-8', 'replace')
    create_time = struct.unpack_from('<q', rec, 32)[0]
    source_info = rec[40:72].split(b'\x00', 1)[0].decode('utf-8', 'replace')
    cal_source_ver = struct.unpack_from('<I', rec, 72)[0]
    offsets = list(struct.unpack_from('<72i', rec, 88))
    gains   = list(struct.unpack_from('<72q', rec, 376))
    sig_magic = rec[952:968]
    sig_jsdrv_version = struct.unpack_from('<I', rec, 968)[0]
    check32 = struct.unpack_from('<I', rec, 1020)[0]
    return {
        'header_ok': header_ok,
        'format_version': format_version,
        'vendor_id': vendor_id,
        'product_id': product_id,
        'serial_number': serial,
        'create_time': create_time,
        'source_info': source_info,
        'cal_source_version': cal_source_ver,
        'offsets': offsets,
        'gains': gains,
        'signature_magic': sig_magic,
        'is_driver_generated': (sig_magic == CAL_SIGNATURE_MAGIC),
        'driver_version_u32': sig_jsdrv_version,  # meaningful when driver-generated
        'check32': check32,
    }
```

## When does new cal take effect?

Immediately. After a successful `current_offset`, `voltage_offset`,
or `slot_copy` into `ACTIVE`, the handler publishes `s/cal/!reload`
before its final `h/cal/!rsp`, and the fpga_mcu reloads `ACTIVE`
from flash into the FPGA cal RAM. The next sample the FPGA emits
already uses the new coefficients. No power cycle is needed.

`slot_copy` into a non-`ACTIVE` slot (e.g. `ACTIVE → TRIM1`) does
not change the FPGA cal RAM and skips the reload.

## Recommended pre-flight: snapshot before mutating

Offset cal overwrites `ACTIVE`. The safe pattern is:

1. `slot_copy(ACTIVE → TRIM1)` - snapshot the current ACTIVE.
2. Run the offset cal.
3. If the user rejects the result, `slot_copy(TRIM1 → ACTIVE)` to
   roll back.

## Timing characteristics

Observed on HIL with the reference JS320:

| Operation                       | samples_per_point | Wall time |
| ------------------------------- | ----------------- | --------- |
| `slot_read`                     | n/a               | ~0.5 s    |
| `slot_copy`                     | n/a               | ~1 s      |
| `current_offset`                | 5000              | ~12 s     |
| `current_offset`                | 200000 (default)  | ~50 s     |
| `voltage_offset`                | 5000              | ~3 s      |
| `voltage_offset`                | 200000 (default)  | ~5 s      |

`slot_read` time is dominated by `jsdrv_open` rather than the flash
read itself. Set UI progress timeouts at roughly 2x the values above
to absorb host scheduling jitter.

## Reference: `minibitty cal`

`example/minibitty/cal.c` is the canonical C consumer. Subcommands:

```
minibitty cal read   <slot> [--out path]              <device>
minibitty cal copy   <src_slot> <dst_slot>            <device>
minibitty cal offset_current [--samples N]            <device>
minibitty cal offset_voltage [--samples N]            <device>
```

If your Python integration behaves differently from the matching
`minibitty cal` invocation, the bug is on the Python side.

## Test coverage

- Unit tests for layout / enum stability:
  `test/devices/js320/js320_cal_test.c`.
- `mb_check32_xxhash` algorithm parity: `test/check32_test.c`.
- The full state machine for each op is exercised by HIL bench runs
  through `minibitty cal` against a real JS320.
