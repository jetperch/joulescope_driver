<!--
# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
-->

# mb_device unit-test harness

`src/devices/mb_device/mb_device.c` has no dedicated unit tests.  It is
compiled into `frontend_test`, but that exercises the frontend, not the
mb_device state machine.  New logic keeps landing here without coverage;
most recently the host sleep/resume handling (JSDRV_USBBK_MSG_POWER ->
SLEEP_REQ, resume ping revalidation, EV_LINK_REVALIDATE_FAILED handshake
replay), which was validated on hardware only.

## The seam

mb_device's dependencies are already narrow:

- `d->ll.cmd_q` / `d->ll.rsp_q` message queues (backend side)
- `d->ul.cmd_q` (frontend side)
- `jsdrvp_backend_send` (frontend publishes)
- `jsdrv_time_utc()` for the timeout / revalidate deadlines
- the optional `drv` upper-driver callbacks

A test can drive the UL thread for real (like the firmware's usbd_test
drives the task): create the queues, start `jsdrvp_ul_mb_device_usb_factory`
with `drv = NULL`, and script the backend side by popping `ll.cmd_q` and
pushing crafted responses into `ll.rsp_q`.  Frame builders can be shared
with (or copied from) the minibitty `usbd_test.c` helpers.  Time control
needs a mockable clock; either a `jsdrv_time_utc` weak/UNITTEST override
or tolerate real 250-500 ms waits (the mb_device timeouts are short
enough that real-time tests stay under ~5 s).

## Tests to write

1. Open handshake: OPEN cmd -> LL open acks -> CONNECT_REQ appears on
   cmd_q -> feed CONNECT_ACK + IDENTITY -> reaches ST_OPEN, OPEN# emitted.
2. Power suspend in ST_OPEN: `JSDRV_USBBK_MSG_POWER` (suspend) ->
   SLEEP_REQ control frame appears on ll.cmd_q; not sent in other states.
3. Power resume, link alive: resume msg -> PING on cmd_q -> feed PONG ->
   no handshake replay; revalidation stands down.
4. Power resume, link dead: resume msg -> 4 PINGs, no PONG -> CONNECT_REQ
   replay; feed CONNECT_ACK + IDENTITY -> back to ST_OPEN without close;
   host frame ids restart at 0.
5. Revalidation stands down when a close request arrives mid-revalidate.
6. Frame-id reset on replay: data frame after replay carries frame_id 0.
