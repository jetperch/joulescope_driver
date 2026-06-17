# Deduplicate MiniBitty memory-transaction handling into mb_device

## Context

The MiniBitty memory protocol (`MB_STDMSG_MEM` / `struct mb_stdmsg_mem_s`,
`minibitty/include/mb/stdmsg.h`) is built and parsed independently in
several places:

| Location | Refs | Use |
|---|---|---|
| `src/devices/js320/js320_fwup.c` | ~27 | firmware READ/WRITE/ERASE/ALLOCATE |
| `src/devices/js320/js320_jtag.c` | ~14 | JTAG memory access |
| `src/devices/js320/js320_cal.c`  | ~11 | calibration READ/WRITE |
| `src/devices/mb_device/mb_device.c` | 2 | metadata blob read (hand-rolled `payload[24]`) |

Each site repeats the same framing: an `mb_stdmsg_header_s` (type
`MB_STDMSG_MEM`, `origin_prefix='h'`) followed by an `mb_stdmsg_mem_s`
(transaction_id, target, operation, timeout_ms, offset, length, optional
data), published to a `!cmd`-style topic; and the inverse on the
response (skip outer header, cast `mb_stdmsg_mem_s`, check `status`, read
`offset`/`length`/`data`).  `mb_device.c`'s metadata read is the worst
offender — it hand-rolls the struct with magic byte offsets
(`payload[4]=target`, `payload[12]=offset`, …) instead of using
`mb_stdmsg_mem_s`.

This duplication is error-prone (each site re-derives offsets) and makes
protocol changes require N edits.

## Goal

A single build/parse implementation in `mb_device.c`, exposed to
device-specific drivers via `mb_drv.h`, used by `state_fetch` metadata
reads and by the js320 `fwup`/`jtag`/`cal` modules.

## Proposed API (mb_drv.h, implemented in mb_device.c)

```c
struct jsdrvp_mb_dev_mem_cmd_s {
    const char * topic;        // device topic, e.g. "c/fwup/!cmd"
    uint32_t transaction_id;
    uint8_t  target;
    uint8_t  operation;        // mb_stdmsg_mem_op_e or custom (>= CUSTOM_START)
    uint8_t  flags;
    uint16_t timeout_ms;
    uint32_t offset;
    uint32_t length;
    uint32_t delay_us;
    const uint8_t * data;      // WRITE payload, or NULL
    uint32_t data_size;
};
// Build [stdmsg_header(MEM)][mb_stdmsg_mem_s][data] and publish_to_device.
void jsdrvp_mb_dev_mem_cmd(struct jsdrvp_mb_dev_s * dev,
                           const struct jsdrvp_mb_dev_mem_cmd_s * cmd);

struct jsdrvp_mb_dev_mem_rsp_s {
    uint32_t transaction_id;
    uint8_t  target;
    uint8_t  operation;
    uint8_t  status;
    uint32_t offset;
    uint32_t length;
    const uint8_t * data;      // points into the response buffer
    uint32_t data_size;        // bytes actually present
};
// Parse a JSDRV_UNION_STDMSG response value; false if too short / not MEM.
bool jsdrvp_mb_dev_mem_rsp_parse(const struct jsdrv_union_s * value,
                                 struct jsdrvp_mb_dev_mem_rsp_s * rsp);
```

`data_size` (cmd) is bounded by the caller's page size; the builder
asserts/clamps against the frame payload max.

## Refactor steps (incremental, each builds + tests green)

1. **Add the helpers** in `mb_device.c` + declarations in `mb_drv.h`.
   Reuse `mb_stdmsg_mem_s` for the layout (no magic offsets).
2. **mb_device metadata read**: replace the hand-rolled `payload[24]` in
   `state_fetch_send_meta_read` with `jsdrvp_mb_dev_mem_cmd`, and the
   manual parse in `state_fetch_on_meta_rsp` with
   `jsdrvp_mb_dev_mem_rsp_parse`.  Verify against hardware (open still
   reads metadata; `info` shows `$` topics).
3. **js320_cal.c** (smallest): switch build/parse to the helpers; run
   `js320_cal_test`.
4. **js320_jtag.c**: switch; run `js320_jtag` coverage (add if missing).
5. **js320_fwup.c** (largest): switch `ctrl_send_fw_cmd` +
   response handling to the helpers (keep the fwup state machine);
   run `js320_fwup_test` and `js320_fwup_mgr_test`, then a hardware
   firmware-update smoke test.

## Risks
- fwup/jtag/cal own per-module transaction-id counters and state
  machines; the helpers must NOT own the id/state — caller passes
  `transaction_id` and matches responses as today.
- fwup uses a large stack buffer (`FW_PAGE_SIZE`); keep the builder
  zero-copy or caller-buffer-based to avoid a second large copy.
- Each module has unit tests with stubs for `jsdrvp_mb_dev_*`; add stubs
  for the new helpers (and consider testing the helpers directly).

## Verification
- Unit: existing js320_{fwup,fwup_mgr,cal}_test pass after each step; add
  a direct test for `mem_cmd`/`mem_rsp_parse` (layout/offsets).
- Hardware: metadata read on open (`info`), a calibration read/write, and
  a full firmware update each succeed.
