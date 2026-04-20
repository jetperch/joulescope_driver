# JS320 Firmware Update Subsystem (js320_fwup)

## Overview

The `js320_fwup` subsystem provides host-side firmware update and
FPGA bitstream programming for the JS320 device. It is implemented
as a driver subsystem within `js320_drv`, receiving a complete
image via pubsub and internally driving the page-level device
protocol. The caller publishes a single command with the full
image and receives a single completion response.

### Motivation

Previously, firmware update and FPGA programming were implemented
as standalone example applications (`example/minibitty/firmware.c`
and `example/minibitty/fpga_mem.c`) that directly managed the
page-level device protocol using Windows-specific threading
primitives. Moving this logic into the driver:

- Enables any language binding (Python, Node.js) to perform
  updates with a single publish-and-wait call.
- Eliminates OS-specific threading code from callers.
- Centralizes the protocol implementation, reducing duplication.


## Architecture

### Component Diagram

```
  Application (C, Python, ...)
       |
       |  h/fwup/ctrl/!cmd   or   h/fwup/fpga/!cmd
       v
  +-----------+
  | js320_drv |  (delegates via handle_cmd / handle_publish)
  +-----------+
       |
  +-------------+
  | js320_fwup  |  (state machines, pipelining, verification)
  +-------------+
       |
       |  c/fwup/!cmd  (ctrl)   or   c/jtag/!cmd  (fpga)
       v
    JS320 Device
```

### Subsystem Delegation

`js320_drv.c` owns the `js320_fwup_s` instance and delegates
to it via the standard driver callback pattern:

| Driver callback    | fwup function              |
|--------------------|----------------------------|
| `on_open`          | `js320_fwup_on_open`       |
| `on_close`         | `js320_fwup_on_close`      |
| `handle_cmd`       | `js320_fwup_handle_cmd`    |
| `handle_publish`   | `js320_fwup_handle_publish`|
| `on_timeout`       | `js320_fwup_on_timeout`    |
| `finalize`         | `js320_fwup_free`          |

Each `handle_cmd` and `handle_publish` callback returns `true`
if the topic was consumed, allowing the driver to chain multiple
subsystems (`js320_fwup` is checked before `js320_jtag`).

### Threading Model

The fwup subsystem runs entirely on the driver's backend thread.
No OS synchronization primitives are needed internally. Pipelining
uses an event-driven sliding window tracked by `send_idx` and
`recv_idx` counters. Each device response advances `recv_idx`
and may trigger the next send.

### Large Payload Handling

Firmware images can be several megabytes. The pubsub message
system handles this automatically via `jsdrvp_msg_alloc_value()`,
which heap-allocates payloads exceeding `JSDRV_PAYLOAD_LENGTH_MAX`
(1024 bytes) and sets the `JSDRV_UNION_FLAG_HEAP_MEMORY` flag.
The fwup subsystem copies the image data into its own buffer
on command receipt, so the message is safely freed afterward.


## Topics

| Topic               | Direction         | Payload                |
|---------------------|-------------------|------------------------|
| `h/fwup/ctrl/!cmd`  | App → Driver      | `fwup_ctrl_cmd_s`      |
| `h/fwup/ctrl/!rsp`  | Driver → App      | `fwup_rsp_s`           |
| `h/fwup/fpga/!cmd`  | App → Driver      | `fwup_fpga_cmd_s`      |
| `h/fwup/fpga/!rsp`  | Driver → App      | `fwup_rsp_s`           |

Internal device-level topics (not used by applications):

| Topic               | Direction         | Protocol               |
|---------------------|-------------------|------------------------|
| `c/fwup/!cmd`       | Driver → Device   | `fw_cmd_s` (16 bytes)  |
| `c/fwup/!rsp`       | Device → Driver   | `fw_cmd_s` (16 bytes)  |
| `c/jtag/!cmd`   | Driver → Device   | `jtag_mem_s` (12 bytes)|
| `c/jtag/!rsp`   | Device → Driver   | `jtag_mem_s` (12 bytes)|
| `c/mode`            | Driver → Device   | `uint32` (FPGA only)   |


## Message Formats

All structures are packed with natural alignment (no padding).

### Ctrl Command (`fwup_ctrl_cmd_s`, 8 bytes + data)

```
Offset  Size  Field
  0       4   transaction_id   Caller-assigned ID, echoed in response
  4       1   op               fwup_ctrl_op_e (UPDATE=1, LAUNCH=2, ERASE=3)
  5       1   image_slot       Target image slot (0-1)
  6       1   pipeline_depth   0=default(8), max 16
  7       1   rsv              Reserved, set to 0
  8       N   data[]           Image bytes (UPDATE only)
```

### FPGA Command (`fwup_fpga_cmd_s`, 8 bytes + data)

```
Offset  Size  Field
  0       4   transaction_id   Caller-assigned ID, echoed in response
  4       1   op               fwup_fpga_op_e (PROGRAM=1)
  5       1   pipeline_depth   0=default(8), max 16
  6       2   rsv              Reserved, set to 0
  8       N   data[]           FPGA bitstream bytes
```

### Response (`fwup_rsp_s`, 8 bytes)

```
Offset  Size  Field
  0       4   transaction_id   Echoed from command
  4       4   status           0=success, JSDRV_ERROR_* on failure
```


## Ctrl Firmware Update

The ctrl path manages the JS320 microcontroller firmware stored
in flash image slots. It supports three operations.

### Operations

**UPDATE** (`FWUP_CTRL_OP_UPDATE = 1`):
Write a firmware image to the device staging buffer, verify
it, and commit to the specified image slot.

**LAUNCH** (`FWUP_CTRL_OP_LAUNCH = 2`):
Boot the device from the specified image slot. The device
resets after accepting this command.

**ERASE** (`FWUP_CTRL_OP_ERASE = 3`):
Erase the specified image slot.

### UPDATE State Machine

```
IDLE ──cmd──> ALLOCATE ──rsp──> WRITE ──all done──> VERIFY ──all done──>
UPDATE ──rsp──> respond to app, return to IDLE
```

| State      | Action                                        |
|------------|-----------------------------------------------|
| ALLOCATE   | Send `FW_CMD_OP_ALLOCATE` with image size     |
| WRITE      | Pipelined 256-byte page writes                |
| VERIFY     | Pipelined 256-byte page reads + compare       |
| UPDATE     | Send `FW_CMD_OP_UPDATE` with target image slot |

### UPDATE Process Detail

1. **ALLOCATE**: The driver sends a single `FW_CMD_OP_ALLOCATE`
   command to the device with the total image size. The device
   prepares a staging buffer and responds.

2. **WRITE**: Pages are written using a sliding window pipeline.
   The driver sends up to `pipeline_depth` concurrent write
   commands. Each page is 256 bytes. The last page is padded
   with `0xFF` to a full page boundary. As each response
   arrives, `recv_idx` advances and a new write may be sent
   if the window has room.

3. **VERIFY**: Pages are read back and compared against the
   original image using the same pipelining strategy. Each
   response carries 256 bytes of read data which is compared
   byte-for-byte against the expected page content (including
   `0xFF` padding on the last page). A mismatch sets
   `JSDRV_ERROR_IO`.

4. **UPDATE**: After successful verification, the driver sends
   `FW_CMD_OP_UPDATE` with the target `image_slot`. The device
   commits the staging buffer to flash and responds. The driver
   then sends the completion response to the application.

### LAUNCH and ERASE

These are single-command operations. The driver sends the
corresponding `FW_CMD_OP_LAUNCH` or `FW_CMD_OP_ERASE` to the
device and forwards the response to the application.

### Device Protocol (c/fwup)

The ctrl path uses the `fw_cmd_s` device protocol (16-byte
header, optionally followed by page data):

```
Offset  Size  Field
  0       4   id        Sequence ID
  4       1   op        fw_cmd_op_e
  5       1   status    0=ok (responses only)
  6       1   image     Image slot
  7       1   rsv
  8       4   offset    Byte offset within staging buffer
 12       4   length    Data length
 16       N   data[]    Page data (write cmd, read rsp)
```


## FPGA Programming

The FPGA path programs the ECP5 SPI configuration flash via the
JTAG interface. It manages the entire lifecycle: mode switch,
JTAG open, flash erase, pipelined write, pipelined verify,
JTAG close, and mode restore.

### PROGRAM State Machine

```
IDLE ──cmd──> MODE_SWITCH ──timeout 100ms──> OPEN ──rsp──>
ERASE ──all blocks──> WRITE ──all pages──> VERIFY ──all pages──>
CLOSE ──rsp──> MODE_RESTORE ──timeout 100ms──> respond to app,
return to IDLE
```

| State         | Action                                     |
|---------------|--------------------------------------------|
| MODE_SWITCH   | Publish `c/mode=2` (JTAG mode), wait 100ms |
| OPEN          | Send `JTAG_MEM_OP_OPEN`                    |
| ERASE         | Send `JTAG_MEM_OP_ERASE_64K` per block     |
| WRITE         | Pipelined 256-byte page writes             |
| VERIFY        | Pipelined 256-byte page reads + compare    |
| CLOSE         | Send `JTAG_MEM_OP_CLOSE`                   |
| MODE_RESTORE  | Publish `c/mode=0` (normal mode), wait 100ms|

### PROGRAM Process Detail

1. **MODE_SWITCH**: The device must be in JTAG mode (mode 2)
   for SPI flash access. The driver publishes `c/mode` with
   value 2 and waits 100ms for the mode switch to take effect.

2. **OPEN**: Sends `JTAG_MEM_OP_OPEN` to enter SPI background
   access mode via the JTAG interface.

3. **ERASE**: Erases 64KB blocks covering the image size. Blocks
   are erased sequentially (one at a time) since flash
   serializes erase operations internally. Block count =
   `ceil(image_size / 65536)`.

4. **WRITE**: Same sliding window pipeline as ctrl. Pages are
   256 bytes, last page padded with `0xFF`.

5. **VERIFY**: Same pipelined read-and-compare as ctrl. Read
   data is compared byte-for-byte against the expected page
   content.

6. **CLOSE**: Sends `JTAG_MEM_OP_CLOSE` to exit SPI background
   mode.

7. **MODE_RESTORE**: Publishes `c/mode` with value 0 to return
   the device to normal operation mode and waits 100ms.

### Error Cleanup

If an error occurs during OPEN, ERASE, WRITE, or VERIFY, the
state machine transitions through CLOSE and MODE_RESTORE before
responding to the application. This ensures the JTAG interface
is properly closed and the device mode is restored regardless
of where the failure occurred.

### Device Protocol (c/jtag)

The FPGA path uses the `jtag_mem_s` device protocol (12-byte
header, optionally followed by page data):

```
Offset  Size  Field
  0       4   transaction_id
  4       1   operation      jtag_mem_op_e
  5       1   status         0=ok (responses only)
  6       2   timeout_ms
  8       4   offset         Flash byte offset
 12       N   data[]         Page data (write cmd, read rsp)
```


## Pipelining

Both ctrl and FPGA paths use the same pipelining strategy for
write and verify (read) operations. This significantly improves
throughput by overlapping USB round-trips.

### Sliding Window

The pipeline is a fixed-size sliding window controlled by two
counters:

- `send_idx`: next page index to send
- `recv_idx`: next page index expected in a response

The window size is `send_idx - recv_idx`, bounded by
`pipeline_depth` (default 8, max 16).

### Event Flow

```
1. Enter WRITE/VERIFY: send_idx=0, recv_idx=0
2. Send up to pipeline_depth commands (send_idx advances)
3. On each response:
   a. recv_idx++
   b. If error: wait for outstanding responses, then fail
   c. If recv_idx == page_count: transition to next state
   d. Else if window has room: send next command
```

### Error Handling

On a device error response, the error is recorded and no new
commands are sent. The driver continues to receive responses for
already-sent commands (draining the pipeline). Once
`recv_idx >= send_idx` (all outstanding responses received),
the operation fails with `JSDRV_ERROR_IO`.


## Usage

### C Example (ctrl firmware update)

```c
// Open device and subscribe to response
jsdrv_open(ctx, device, JSDRV_DEVICE_OPEN_MODE_RESUME, timeout);
jsdrv_subscribe(ctx, "{device}/h/fwup/ctrl/!rsp",
                JSDRV_SFLAG_PUB, on_rsp, NULL, 0);

// Build command: 8-byte header + image
uint32_t cmd_size = 8 + image_size;
uint8_t * buf = malloc(cmd_size);
struct fwup_ctrl_cmd_s * cmd = (void *) buf;
cmd->transaction_id = 1;
cmd->op = FWUP_CTRL_OP_UPDATE;  // 1
cmd->image_slot = 0;
cmd->pipeline_depth = 8;
cmd->rsv = 0;
memcpy(cmd->data, image, image_size);

jsdrv_publish(ctx, "{device}/h/fwup/ctrl/!cmd",
              &jsdrv_union_bin(buf, cmd_size), 0);
free(buf);
// Wait for on_rsp callback with fwup_rsp_s
```

### Python Example (ctrl firmware update)

```python
d = Driver()
d.open(device, 'restore')

image = open('firmware.mbfw', 'rb').read()
cmd = struct.pack('<IBBBB', 1, 1, 0, 8, 0) + image
rsp = d.publish_and_wait(
    f'{device}/h/fwup/ctrl/!cmd', cmd,
    f'{device}/h/fwup/ctrl/!rsp', timeout=60.0)
status = struct.unpack('<Ii', rsp[:8])
```

### Python Example (FPGA programming)

```python
bitstream = open('design.bit', 'rb').read()
cmd = struct.pack('<IBBH', 1, 1, 8, 0) + bitstream
rsp = d.publish_and_wait(
    f'{device}/h/fwup/fpga/!cmd', cmd,
    f'{device}/h/fwup/fpga/!rsp', timeout=120.0)
status = struct.unpack('<Ii', rsp[:8])
```


## Files

| File                                    | Description              |
|-----------------------------------------|--------------------------|
| `include_private/jsdrv_prv/js320_fwup.h`| Public header            |
| `src/js320_fwup.c`                      | State machines           |
| `src/js320_drv.c`                       | Driver integration       |
| `example/minibitty/firmware.c`          | CLI example (ctrl)       |
| `example/minibitty/fpga_mem.c`          | CLI example (FPGA)       |
