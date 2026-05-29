# JS320 Calibration Manager (js320_cal_mgr)

Status: implemented in driver `2.2.0` (commit `984a7fc`). No Python wrapper
ships yet — this document is the integration guide for adding one to
`pyjoulescope_driver` and consuming it from the Joulescope UI.

## Audience

You are integrating offset calibration into the Joulescope UI. You will
talk to the new `js320_cal_mgr` over the standard pyjoulescope_driver
`Driver` pubsub API. There is no high-level Python class yet; you may add
one as you go.

## Why this exists

The JS220 offset cal lives inside `pyjoulescope_ui` and can only run from
the GUI. That blocks scripts and non-Python bindings from refreshing
offsets. For the JS320 we moved the entire flow into `joulescope_driver`
so every binding — and customer-support shell scripts — gets it for free.
Your UI integration should mirror the JS220 widget's user flow but call
into the driver, not reimplement the algorithm.

## What it does

Four operations, all on the JS320:

| op token         | numeric | result                                      |
| ---------------- | ------- | ------------------------------------------- |
| `slot_read`      | 0       | dump any cal slot back to the host          |
| `slot_copy`      | 1       | copy `src_slot` → `dst_slot` (policy-gated) |
| `current_offset` | 2       | sweep current ADC offsets, write `ACTIVE`   |
| `voltage_offset` | 3       | measure voltage ADC offsets, write `ACTIVE` |

The JS320 stores cal in six flash slots, in search order
`ACTIVE → TRIM2 → TRIM1 → FIELD → LAB → FACTORY`. The fpga_mcu loads
the first valid record (by `mb_check32_xxhash`) at boot and copies it
into the FPGA's runtime cal memory.

`slot_copy` is policy-gated to prevent foot-shooting:

| dst       | allowed src                          |
| --------- | ------------------------------------ |
| `ACTIVE`  | `TRIM2`, `TRIM1`, `FIELD`, `LAB`, `FACTORY` |
| `TRIM1`   | `ACTIVE` only                        |
| `TRIM2`   | `ACTIVE` only                        |
| `FIELD`, `LAB`, `FACTORY` | rejected (read-only from the driver) |

Both offset ops touch only the `offsets[]` array. `gains[]`,
`source_info`, `cal_source_version` and the rest of the record are
preserved. `create_time` is refreshed. The 64-byte signature is
overwritten with the driver tag (see below) and `check32` is recomputed.

## Topic surface

The cal manager subscribes once at driver init and lives at the top of
the pubsub tree (not under a device prefix):

| Topic              | Direction | Value                                     |
| ------------------ | --------- | ----------------------------------------- |
| `cal/@/!add`       | publish   | binary `jsdrv_cal_add_header_s` (40 B)    |
| `cal/@/!add#`      | response  | binary `jsdrv_cal_add_rsp_s` (8 B)        |
| `cal/@/list`       | retained  | comma-separated active worker IDs ("001,002") |
| `cal/NNN/status`   | retained  | JSON status string                        |
| `cal/NNN/data`     | publish   | binary 1024-byte record (slot_read only)  |

`NNN` is a 3-digit, zero-padded worker ID assigned by the manager. Up to
4 workers (`JSDRV_CAL_INSTANCE_MAX`) can run in parallel.

### Status JSON shape

```json
{
  "state": "INIT|OPEN|READ_SLOT|SWEEP|WRITE_SLOT|DONE|ERROR",
  "pct": 0.0,
  "msg": "human-readable progress message",
  "rc": 0,
  "op": "slot_read|slot_copy|current_offset|voltage_offset"
}
```

`pct` is `0.0..100.0` (`tenths` precision). Treat any `state` in
`{"DONE", "ERROR"}` as terminal. `rc=0` on `DONE`; nonzero
`JSDRV_ERROR_*` code on `ERROR`. The `msg` field is suitable for showing
in a UI status line; it gets surprisingly chatty during the current cal
sweep — useful for a progress widget.

### Add header layout

Defined in `include_private/jsdrv_prv/devices/js320/js320_cal_mgr.h`:

```c
struct jsdrv_cal_add_header_s {
    char     device_prefix[32];   //  0..31   e.g., "u/js320/8W2A"
    uint8_t  op;                  // 32
    uint8_t  src_slot;            // 33
    uint8_t  dst_slot;            // 34
    uint8_t  flags;               // 35       reserved, 0
    uint32_t samples_per_point;   // 36..39   0 = default (100000)
};                                // 40 bytes total
```

`struct.Struct('<32sBBBBI')` matches it byte-for-byte. The C side
verifies `sizeof == 40` at compile time and the test suite pins the
field offsets, so this layout is stable.

### Add response layout

```c
struct jsdrv_cal_add_rsp_s {
    int32_t  rc;          // 0 = accepted; JSDRV_ERROR_* on rejection
    uint32_t worker_id;   // valid iff rc == 0
};                        // 8 bytes total
```

You can either read this off `cal/@/!add#` yourself, or just observe
that `pyjoulescope_driver.Driver.publish(...)` returns the `rc` directly
(it waits for the return-code reply). On accept, your worker ID is in
the very next status publish — `cal/NNN/status` carries `NNN` in the
topic, so you don't strictly need the response. The fwup wrapper
(`program_js320.py`) ignores the response for this reason.

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
| 32..39     | create_time            | i64 LE — jsdrv time64                  |
| 40..71     | source_info            | UTF-8 NUL-padded; "JETPERCH:US:MD:01:01" for factory |
| 72..75     | cal_source_version     | u32 LE                                 |
| 88..375    | `offsets[72]`          | int32_t LE, 1Q31                       |
| 376..951   | `gains[72]`            | int64_t LE, 12Q52 — left untouched by offset cal |
| 952..1015  | signature              | see below                              |
| 1020..1023 | check32                | `mb_check32_xxhash` over first 1020 bytes |

The 72-entry offsets and gains arrays are laid out as
`channel * 24 + point_index`, where `channel ∈ {0,1,2}` are current
channels and the point index follows the upper-triangular
`(i_select, mux_select)` sweep:

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

The 1Q31 offset matches the left-aligned i32 ADC sample directly — the
driver just stores the i32 mean of the captured samples.

### Driver-generated signature pattern

After offset cal, bytes 952..1015 are:

| Bytes   | Value                                  |
| ------- | -------------------------------------- |
| 0..15   | `"JSDRV_OFFSET_CAL"` (no NUL)          |
| 16..19  | `JSDRV_VERSION_U32` (u32 LE)           |
| 20..63  | 0x00                                   |

The FPGA does not verify the ECDSA signature — only `check32` is
enforced. The magic exists purely so customer support can grep a record
dump and immediately recognise driver-written cal vs PCTS factory cal.
The UI may want to surface this to the user ("Calibration source:
driver offset cal, jsdrv 2.2.0") when displaying a record.

## Operational model

### Lifecycle

The cal manager runs in a worker thread. The host caller:

1. Publishes the add header.
2. Subscribes to `cal/` (or `cal/NNN/status` and `cal/NNN/data`).
3. Reads status updates until `state ∈ {"DONE", "ERROR"}`.
4. Unsubscribes.

### Device open mode

The worker calls `jsdrv_open(... JSDRV_DEVICE_OPEN_MODE_RESUME)`
internally — same mode the `minibitty mem` command uses. **The device
must not already be open by your UI** when you publish the !add — the
driver opens it, runs the cal, and closes it. (Specifically: the driver
keeps a single open handle per device. If your UI has it open in
"default" streaming mode and you trigger a cal, things will conflict.)
The cleanest UI pattern is:

1. Close the device from the UI.
2. Trigger the cal.
3. After `DONE`, reopen in your normal mode.

### When does new cal take effect?

The fpga_mcu loads `ACTIVE` into FPGA cal memory **once, at boot**. A
freshly-written `ACTIVE` is on flash but the FPGA datapath is still
using the old coefficients. To apply the new cal you must either:

- Power-cycle the JS320 (cleanest); or
- Force the device to reinitialise (re-open might suffice depending on
  state — verify against your specific UI flow).

If your UI doesn't currently power-cycle, the simplest UX is to tell
the user "Disconnect and reconnect the device to apply the new
calibration" and confirm by re-reading `ACTIVE` after they reconnect.
The cal mgr itself does not trigger a reboot — that policy belongs to
the UI.

### What the UI needs the user to set up

The driver doesn't prompt for fixturing. The caller is responsible for
arranging the physical input state before triggering each op:

- **`current_offset`**: drive zero current through the current inputs.
  Practically this means **open** at the banana plugs (no load) — the
  default PCTS "zero open" case. The shorted variant is only used in
  manufacturing to characterise charge injection.
- **`voltage_offset`**: short `v+` to `v-` (typically a shorting plug or
  jumper). With open inputs the cal will run but the result is garbage
  because the input is high-impedance.
- **`slot_copy` / `slot_read`**: no setup required.

### Timing

| Op                  | Wall time (default 100k samples) | Notes                       |
| ------------------- | -------------------------------- | --------------------------- |
| `slot_read`         | ~1 s                             | 4 page reads + open/close   |
| `slot_copy`         | ~1.2 s                           | erase + write + verify      |
| `current_offset`    | ~30 s                            | 21 points × 2 passes × ~700 ms |
| `voltage_offset`    | ~3 s                             | 2 ranges × ~1 s             |

Tighten by passing a smaller `samples_per_point` (the driver also
accepts `10000` — ~10 ms per point at 1 MHz — at the cost of higher
noise on the offset estimate). The HIL bench used `--samples 10000` for
faster iteration: current cal ran in **4.7 s**.

### Recommended pre-flight: snapshot before mutating

Offset cal overwrites `ACTIVE`. The safe pattern is:

1. `slot_copy(ACTIVE → TRIM1)` — snapshot the current ACTIVE.
2. Run the offset cal.
3. If the user rejects the result, `slot_copy(TRIM1 → ACTIVE)` to roll
   back.

Or in a more deluxe UI: `slot_copy(FACTORY → ACTIVE)` first to give the
user a known starting point. The HIL test sequence in
`doc/plans/completed/check32_xxhash_dedup.md` (and the original
implementation plan) follows this pattern.

## Reference Python wrapper

Drop this into `pyjoulescope_driver/cal_js320.py`. Modeled on the
existing `program_js320.py`. It is intentionally low-level — the UI can
layer a `QObject`/signals shell on top.

```python
# pyjoulescope_driver/cal_js320.py
"""JS320 calibration via the in-driver cal manager.

Mirrors the C definitions in
``include_private/jsdrv_prv/devices/js320/js320_cal_mgr.h``.
"""

import json
import logging
import struct
import threading

from pyjoulescope_driver import Driver


_log = logging.getLogger(__name__)

CAL_TOPIC_ADD = 'cal/@/!add'

# Operations
OP_SLOT_READ      = 0
OP_SLOT_COPY      = 1
OP_CURRENT_OFFSET = 2
OP_VOLTAGE_OFFSET = 3

# Slot IDs (match enum jsdrv_cal_slot_e + FLASH_BLOCK_CAL_* order)
SLOT_ACTIVE  = 0
SLOT_TRIM2   = 1
SLOT_TRIM1   = 2
SLOT_FIELD   = 3
SLOT_LAB     = 4
SLOT_FACTORY = 5

SLOT_NAMES = {
    SLOT_ACTIVE:  'ACTIVE',
    SLOT_TRIM2:   'TRIM2',
    SLOT_TRIM1:   'TRIM1',
    SLOT_FIELD:   'FIELD',
    SLOT_LAB:     'LAB',
    SLOT_FACTORY: 'FACTORY',
}
SLOT_IDS = {v: k for k, v in SLOT_NAMES.items()}

CAL_RECORD_SIZE = 1024
CAL_SIGNATURE_MAGIC = b'JSDRV_OFFSET_CAL'

# struct jsdrv_cal_add_header_s: char[32], u8 op, u8 src, u8 dst, u8 flags, u32 samples
_CAL_ADD_HEADER = struct.Struct('<32sBBBBI')


def _run_op(driver: Driver, device_path: str, op: int,
            src_slot: int = 0, dst_slot: int = 0,
            samples_per_point: int = 0,
            progress=None, timeout: float = 60.0,
            collect_data: bool = False):
    """Publish a cal/@/!add and wait for the worker to finish.

    Returns (last_status_dict, data_bytes_or_None).
    """
    if progress is None:
        progress = lambda x, y: None

    device_prefix = device_path.encode('utf-8')
    if len(device_prefix) >= 32:
        raise ValueError(f'device path too long: {device_path}')
    payload = _CAL_ADD_HEADER.pack(
        device_prefix, op, src_slot, dst_slot, 0, int(samples_per_point))

    done = threading.Event()
    last = {'state': '', 'msg': '', 'rc': 0, 'pct': 0.0, 'op': ''}
    data_buf = {'bytes': None}

    def on_pub(topic, value):
        # cal/NNN/status arrives as a JSON string; cal/NNN/data as bytes.
        if isinstance(value, str) and topic.endswith('/status'):
            try:
                entry = json.loads(value)
            except (ValueError, TypeError):
                return
            last['state'] = str(entry.get('state', '?'))
            last['msg']   = str(entry.get('msg', ''))
            last['op']    = str(entry.get('op', ''))
            try:
                last['rc'] = int(entry.get('rc', 0))
            except (TypeError, ValueError):
                pass
            try:
                last['pct'] = float(entry.get('pct', 0.0))
            except (TypeError, ValueError):
                pass
            _log.info('cal %s %.1f%%: %s (rc=%d) [%s]',
                      last['state'], last['pct'], last['msg'],
                      last['rc'], topic)
            progress(last['pct'] / 100.0,
                     f'{last["state"]}: {last["msg"]}')
            if last['state'] in ('DONE', 'ERROR'):
                done.set()
        elif collect_data and topic.endswith('/data'):
            if isinstance(value, (bytes, bytearray)):
                data_buf['bytes'] = bytes(value)

    progress(0.0, 'Starting JS320 calibration')
    driver.subscribe('cal/', 'pub', on_pub)
    try:
        rc = driver.publish(CAL_TOPIC_ADD, payload)
        if rc:
            raise RuntimeError(f'cal add rejected: rc={rc}')
        if not done.wait(timeout):
            raise TimeoutError(
                f'JS320 cal timed out after {timeout:.0f} s '
                f'(last state {last["state"]}: {last["msg"]})')
    finally:
        driver.unsubscribe('cal/', on_pub)

    if last['state'] != 'DONE' or last['rc'] != 0:
        raise RuntimeError(
            f'JS320 cal failed: {last["msg"]} '
            f'(state={last["state"]}, rc={last["rc"]})')
    progress(1.0, 'Complete')
    return last, data_buf['bytes']


def slot_read(driver, device_path, slot, **kwargs):
    """Read a calibration slot.  Returns the 1024-byte record."""
    if isinstance(slot, str):
        slot = SLOT_IDS[slot.upper()]
    _, data = _run_op(driver, device_path, OP_SLOT_READ,
                      src_slot=slot, collect_data=True, **kwargs)
    if data is None or len(data) != CAL_RECORD_SIZE:
        raise RuntimeError(
            f'slot_read returned {len(data) if data else 0} bytes')
    return data


def slot_copy(driver, device_path, src_slot, dst_slot, **kwargs):
    """Copy one slot to another (policy-enforced inside the driver)."""
    if isinstance(src_slot, str):
        src_slot = SLOT_IDS[src_slot.upper()]
    if isinstance(dst_slot, str):
        dst_slot = SLOT_IDS[dst_slot.upper()]
    _run_op(driver, device_path, OP_SLOT_COPY,
            src_slot=src_slot, dst_slot=dst_slot, **kwargs)


def current_offset_cal(driver, device_path, samples_per_point=0, **kwargs):
    """Sweep upper-triangular shunt/mux, write ACTIVE.

    The caller must ensure a zero-current input state (open at the
    current terminals).
    """
    kwargs.setdefault('timeout', 60.0)
    _run_op(driver, device_path, OP_CURRENT_OFFSET,
            samples_per_point=samples_per_point, **kwargs)


def voltage_offset_cal(driver, device_path, samples_per_point=0, **kwargs):
    """Measure voltage ADC offsets at both ranges, write ACTIVE.

    The caller must arrange a zero-voltage input state (short v+/v-).
    """
    kwargs.setdefault('timeout', 15.0)
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
        'driver_version_u32': sig_jsdrv_version,  # only meaningful if driver-generated
        'check32': check32,
    }
```

The driver doesn't expose a Python helper to verify `check32`; use the
extracted C helper if you need to validate (or just trust that the
driver wrote a valid record — it always recomputes before writing).

## UI patterns to consider

Things the JS220 widget got right that should carry over:

- A "before / after" view of the ADC noise floor (stream the raw
  `s/adc/{0,1,2}/!data` and `s/adc/3/!data` for a few seconds before and
  after; show RMS or peak-to-peak). After a successful offset cal, the
  DC mean should drop into the noise floor.
- A confirmation screen describing the fixturing the user must perform
  (open current inputs / shorted voltage inputs) with an image. The
  driver gives you zero protection against running cal in the wrong
  state.
- Power-cycle prompt after writing `ACTIVE` (see "When does new cal
  take effect?").

Things to do differently from JS220:

- Show the calibration source clearly. With the new
  `is_driver_generated` flag the UI can distinguish factory cal from
  driver-generated offset cal and surface "Last trimmed by:
  driver / jsdrv 2.2.0 / 2026-05-29 14:44".
- Expose `slot_copy` so the user can snapshot/restore. A backup step
  before any cal write is the cheapest possible undo.

## Reference: `minibitty cal`

`example/minibitty/cal.c` is the canonical C consumer of this API. It
ran every code path against `u/js320/8W2A` during stage 6 of the
implementation. Subcommands map directly:

```
minibitty cal read   <slot> [--out path]              <device>
minibitty cal copy   <src_slot> <dst_slot>            <device>
minibitty cal offset_current [--samples N]            <device>
minibitty cal offset_voltage [--samples N]            <device>
```

If your Python integration behaves differently from the matching
`minibitty cal` invocation, the bug is on the Python side.

## Test coverage

- Unit tests for the layout/policy: `test/devices/js320/js320_cal_mgr_test.c`
- Check32 algorithm parity: `test/check32_test.c`
- The full state machine for each op is exercised by the HIL bench
  invocations in `minibitty cal` against a real JS320 — see commit
  `984a7fc` for the validated sequence.

If you add a Python wrapper, please add a small `pyjoulescope_driver`
test that mocks the `Driver` and verifies the header packing is
byte-identical to the C struct (re-pack via `_CAL_ADD_HEADER` and
compare to a captured payload).
