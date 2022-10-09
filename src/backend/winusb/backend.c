/*
 * Copyright 2014-2022 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 *
 * @brief Joulescope driver winusb backend
 */

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_INFO
#include "jsdrv.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/devices.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/windows.h"
#include "jsdrv_prv/msg_queue.h"
#include "device_change_notifier.h"
#include "jsdrv/error_code.h"
#include "jsdrv/cstr.h"
#include "tinyprintf.h"
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdio.h>
#include <wchar.h>
#include <inttypes.h>


#define DEVICES_MAX                     (256U)
#define DEVICE_PATH_MAX                 (256U)
#define CONTROL_TIMEOUT_MS              (1000)
#define BULK_IN_FRAME_LENGTH            (512)
#define BULK_IN_TRANSFER_SIZE           (64 * BULK_IN_FRAME_LENGTH)
#define BULK_IN_TRANSFER_OUTSTANDING    (4)

enum device_mark_e {        // for scan add/remove mark & sweep
    DEVICE_MARK_NONE = 0,
    DEVICE_MARK_FOUND,
    DEVICE_MARK_ADDED,
    DEVICE_MARK_REMOVED
};

// forward declarations, see below.
struct dev_s;
struct bulk_in_s;
struct endpoint_s;

typedef int32_t (*ep_process)(struct endpoint_s *);  // 0 or error code
typedef void (*ep_finalize)(struct endpoint_s *);

struct endpoint_s {
    struct dev_s * dev;
    uint8_t pipe_id;
    HANDLE event;
    ep_process process;
    ep_finalize finalize;
    struct jsdrv_list_s item;     // for in use list
};

struct bulk_in_transfer_s {
    struct bulk_in_s * bulk;
    OVERLAPPED overlapped;
    struct jsdrv_list_s item;
    uint8_t buffer[BULK_IN_TRANSFER_SIZE];
};

struct bulk_in_s {
    struct endpoint_s ep;
    struct jsdrv_list_s transfers_pending;
    struct jsdrv_list_s transfers_free;
};

struct bulk_out_s {
    struct endpoint_s ep;
    OVERLAPPED overlapped;
    struct jsdrv_list_s msg_pending;
};

struct dev_s {
    struct jsdrv_context_s * context;
    struct jsdrvp_ll_device_s device;

    uint8_t mark;       // for mark and sweep on scan
    char device_path[DEVICE_PATH_MAX];  // SetupDiGetDeviceInterfaceDetailW->SP_DEVICE_INTERFACE_DETAIL_DATA.DevicePath as UTF-8
    const struct device_type_s * device_type;

    HANDLE file;        // The file handle for this device
    HANDLE winusb;      // The WinUSB handle for the device's default interface

    bool do_exit;
    HANDLE thread;
    DWORD thread_id;
    bool update_handles;

    OVERLAPPED ctrl_overlapped;
    HANDLE ctrl_event;
    struct jsdrv_list_s ctrl_list;  // jsdrvp_msg_s

    struct endpoint_s * endpoints[256];
    struct jsdrv_list_s endpoints_active;  // struct endpoint_s

    struct jsdrv_list_s item;     // manage devices: free and allocated lists
};


//#############################################################################
//# BULK IN STREAMING ENDPOINT                                                #
//#############################################################################

static struct bulk_in_transfer_s * bulk_in_transfer_alloc(struct bulk_in_s * b) {
    struct jsdrv_list_s * item = jsdrv_list_remove_head(&b->transfers_free);
    struct bulk_in_transfer_s * t;
    if (item) {
        t = JSDRV_CONTAINER_OF(item, struct bulk_in_transfer_s, item);
    } else {
        t = jsdrv_alloc_clr(sizeof(struct bulk_in_transfer_s));
        jsdrv_list_initialize(&t->item);
    }
    t->bulk = b;
    memset(&t->overlapped, 0, sizeof(t->overlapped));
    t->overlapped.hEvent = b->ep.event;
    JSDRV_LOGD3("bulk_in_transfer_alloc %p", &t->overlapped);
    return t;
}

static void bulk_in_transfer_free(struct bulk_in_transfer_s * t) {
    JSDRV_LOGD3("bulk_in_transfer_free %p", &t->overlapped);
    jsdrv_list_add_tail(&t->bulk->transfers_free, &t->item);
}

static void bulk_in_finalize(struct endpoint_s * ep) {
    struct bulk_in_s * b = (struct bulk_in_s *) ep;
    JSDRV_LOGI("bulk_in_finalize ep=0x%02x", b->ep.pipe_id);
    WinUsb_AbortPipe(b->ep.dev->winusb, b->ep.pipe_id);
    while (!jsdrv_list_is_empty(&b->transfers_pending)) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(&b->transfers_pending);
        struct bulk_in_transfer_s * t = JSDRV_CONTAINER_OF(item, struct bulk_in_transfer_s, item);
        ULONG sz = 0;
        WinUsb_GetOverlappedResult(b->ep.dev->winusb, &t->overlapped, &sz, TRUE);
        jsdrv_list_add_tail(&b->transfers_free, &t->item);
    }

    while (!jsdrv_list_is_empty(&b->transfers_free)) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(&b->transfers_free);
        struct bulk_in_transfer_s * t = JSDRV_CONTAINER_OF(item, struct bulk_in_transfer_s, item);
        jsdrv_free(t);
    }

    if (b->ep.event) {
        CloseHandle(b->ep.event);
    }
    b->ep.event = NULL;
    jsdrv_free(b);
}

static int32_t bulk_in_pend(struct bulk_in_s * b) {
    JSDRV_LOGD2("bulk_in_pend");
    // Pend read operations
    size_t pending = jsdrv_list_length(&b->transfers_pending);
    for (size_t idx = pending; idx < BULK_IN_TRANSFER_OUTSTANDING; ++idx) {
        struct bulk_in_transfer_s * t = bulk_in_transfer_alloc(b);
        if (!WinUsb_ReadPipe(b->ep.dev->winusb, b->ep.pipe_id, t->buffer, BULK_IN_TRANSFER_SIZE, NULL, &t->overlapped)) {
            DWORD ec = GetLastError();
            if (ec != ERROR_IO_PENDING) {
                WINDOWS_LOGE("%s", "bulk_in_pend WinUsb_ReadPipe error");
                bulk_in_transfer_free(t);
                return 1;
            }
        }
        JSDRV_LOGD3("bulk_in_pend WinUsb_ReadPipe %p", &t->overlapped);
        jsdrv_list_add_tail(&b->transfers_pending, &t->item);
    }
    return 0;
}

static int32_t bulk_in_process(struct endpoint_s * ep) {
    JSDRV_LOGD2("bulk_in_process");
    struct bulk_in_s * b = (struct bulk_in_s *) ep;
    ResetEvent(b->ep.event);
    int32_t rc = 0;

    // Handle completed read operations
    while (1) {
        struct jsdrv_list_s * item = jsdrv_list_peek_head(&b->transfers_pending);
        if (!item) {
            break;
        }
        struct bulk_in_transfer_s * t = JSDRV_CONTAINER_OF(item, struct bulk_in_transfer_s, item);
        ULONG sz = 0;
        if (WinUsb_GetOverlappedResult(b->ep.dev->winusb, &t->overlapped, &sz, FALSE)) {
            JSDRV_LOGD3("bulk_in_process %p ready, %zu bytes",  &t->overlapped, sz);
            jsdrv_list_remove_head(&b->transfers_pending);
            struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(b->ep.dev->context);
            jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));
            m->value = jsdrv_union_bin(t->buffer, sz);
            m->extra.bkusb_stream.endpoint = b->ep.pipe_id;
            msg_queue_push(b->ep.dev->device.rsp_q, m);
        } else {
            DWORD ec = GetLastError();
            if ((ec == ERROR_IO_INCOMPLETE) || (ec == ERROR_IO_PENDING)) {
                break;  // ok, not done yet
            } else if (ec == ERROR_SEM_TIMEOUT) {
                JSDRV_LOGD1("bulk_in_process timeout");
                bulk_in_transfer_free(t);  // timeout ok
            } else {
                WINDOWS_LOG(JSDRV_LOGW, "%s", "bulk_in_process WinUsb_GetOverlappedResult error");
                bulk_in_transfer_free(t);
                rc = 1;
            }
        }
    }

    if (rc) {
        return rc;
    } else {
        return bulk_in_pend(b);
    }
}

static struct bulk_in_s * bulk_in_initialize(struct dev_s * dev, uint8_t pipe_id) {
    pipe_id |= 0x80;  // force IN
    JSDRV_LOGI("bulk_in_initialize pipe_id=0x%02x", pipe_id);
    struct bulk_in_s * b = jsdrv_alloc_clr(sizeof(struct bulk_in_s));
    b->ep.dev = dev;
    b->ep.pipe_id = pipe_id;
    b->ep.process = bulk_in_process;
    b->ep.finalize = bulk_in_finalize;
    b->ep.event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );
    if (!b->ep.event) {
        JSDRV_LOGE("CreateEvent failed");
        free(b);
        return NULL;
    }
    jsdrv_list_initialize(&b->ep.item);
    jsdrv_list_initialize(&b->transfers_pending);
    jsdrv_list_initialize(&b->transfers_free);

    //IGNORE_SHORT_PACKETS;
    ULONG value = 0;
    ULONG value_size = sizeof(value);
    if (!WinUsb_GetPipePolicy(dev->winusb, pipe_id, MAXIMUM_TRANSFER_SIZE, &value_size, &value)) {
        WINDOWS_LOGE("%s", "WinUsb_GetPipePolicy MAXIMUM_TRANSFER_SIZE");
    } else {
        JSDRV_LOGI("MAXIMUM_TRANSFER_SIZE pipe_id=0x%02x bytes=%d", pipe_id, (int) value);
    }

    //value = TRUE;
    //if (!WinUsb_SetPipePolicy(dev->winusb, pipe_id, AUTO_CLEAR_STALL, sizeof(value), &value)) {
    //    WINDOWS_LOGE("%s", "WinUsb_SetPipePolicy AUTO_CLEAR_STALL");
    //}

    value = TRUE;
    if (!WinUsb_SetPipePolicy(dev->winusb, pipe_id, RAW_IO, sizeof(value), &value)) {
        WINDOWS_LOGE("%s", "WinUsb_SetPipePolicy RAW_IO");
    }

    //value = 100;
    //if (!WinUsb_SetPipePolicy(dev->winusb, pipe_id, PIPE_TRANSFER_TIMEOUT, sizeof(value), &value)) {
    //    WINDOWS_LOGE("%s", "WinUsb_SetPipePolicy PIPE_TRANSFER_TIMEOUT");
    //}

    return b;
}


//#############################################################################
//# BULK OUT                                                                  #
//#############################################################################

static void bulk_out_finalize(struct endpoint_s * ep) {
    struct bulk_out_s * b = (struct bulk_out_s *) ep;
    WinUsb_AbortPipe(b->ep.dev->winusb, b->ep.pipe_id);
    if (!jsdrv_list_is_empty(&b->msg_pending)) {
        ULONG sz = 0;
        WinUsb_GetOverlappedResult(b->ep.dev->winusb, &b->overlapped, &sz, TRUE);
    }

    while (!jsdrv_list_is_empty(&b->msg_pending)) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(&b->msg_pending);
        struct jsdrvp_msg_s * m = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
        m->value = jsdrv_union_i32(JSDRV_ERROR_ABORTED);
        msg_queue_push(b->ep.dev->device.rsp_q, m);
    }

    if (b->ep.event) {
        CloseHandle(b->ep.event);
        b->ep.event = NULL;
    }

    jsdrv_free(b);
}

static bool bulk_out_complete_next(struct bulk_out_s * b) {
    if (!jsdrv_list_is_empty(&b->msg_pending)) {
        ULONG sz = 0;
        if (WinUsb_GetOverlappedResult(b->ep.dev->winusb, &b->overlapped, &sz, FALSE)) {
            JSDRV_LOGD1("bulk_out_process ready");
            struct jsdrv_list_s * item = jsdrv_list_remove_head(&b->msg_pending);
            struct jsdrvp_msg_s * m = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
            m->value = jsdrv_union_i32(0);
            msg_queue_push(b->ep.dev->device.rsp_q, m);
        } else {
            DWORD ec = GetLastError();
            if ((ec == ERROR_IO_INCOMPLETE) || (ec == ERROR_IO_PENDING)) {
                return true;
            }
            WINDOWS_LOGE("%s", "bulk_out_complete_next WinUsb_GetOverlappedResult error");
            return false;   // stream error!
        }
    }
    return true;
}

static int32_t bulk_out_send_next(struct bulk_out_s * b) {
    if (!jsdrv_list_is_empty(&b->msg_pending)) {
        struct jsdrv_list_s * item = jsdrv_list_peek_head(&b->msg_pending);
        struct jsdrvp_msg_s * m = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
        memset(&b->overlapped, 0, sizeof(b->overlapped));
        b->overlapped.hEvent = b->ep.event;
        if (!WinUsb_WritePipe(b->ep.dev->winusb, b->ep.pipe_id, (uint8_t *) m->value.value.bin, m->value.size, NULL, &b->overlapped)) {
            DWORD ec = GetLastError();
            if (ec != ERROR_IO_PENDING) {
                WINDOWS_LOGE("%s", "bulk_out_send_next error");
                m->value = jsdrv_union_i32(JSDRV_ERROR_IO);
                msg_queue_push(b->ep.dev->device.rsp_q, m);
                return 1;
            }
        }
        JSDRV_LOGD2("bulk_out_send_next WinUsb_WritePipe %p", &b->overlapped);
    }
    return 0;
}

static int32_t bulk_out_process(struct endpoint_s * ep) {
    JSDRV_LOGD1("bulk_out_process");
    struct bulk_out_s * b = (struct bulk_out_s *) ep;
    ResetEvent(b->ep.event);
    if (!bulk_out_complete_next(b)) {
        return 1;
    }
    return bulk_out_send_next(b);
}

static struct bulk_out_s * bulk_out_initialize(struct dev_s * dev, uint8_t pipe_id) {
    struct bulk_out_s * b = jsdrv_alloc_clr(sizeof(struct bulk_out_s));
    b->ep.dev = dev;
    b->ep.pipe_id = pipe_id & ~0x80;  // force OUT
    b->ep.event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );
    JSDRV_ASSERT(b->ep.event);
    b->ep.process = bulk_out_process;
    b->ep.finalize = bulk_out_finalize;
    jsdrv_list_initialize(&b->ep.item);
    jsdrv_list_initialize(&b->msg_pending);
    return b;
}

static void bulk_out_send(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    uint8_t ep = msg->extra.bkusb_stream.endpoint;
    struct bulk_out_s * b;
    if (!d->endpoints[ep]) {
        JSDRV_LOGD1("bulk_out_send initializing %d", (int) ep);
        b = bulk_out_initialize(d, ep);
        d->endpoints[ep] = &b->ep;
        jsdrv_list_add_tail(&d->endpoints_active, &b->ep.item);
        d->update_handles = true;
    } else {
        b = (struct bulk_out_s *) d->endpoints[ep];
    }
    if (jsdrv_list_is_empty(&b->msg_pending)) {
        jsdrv_list_add_tail(&b->msg_pending, &msg->item);
        bulk_out_send_next(b);
    } else {
        jsdrv_list_add_tail(&b->msg_pending, &msg->item);
    }
}

//#############################################################################
//# DEVICE                                                                    #
//#############################################################################

static int32_t device_open(struct dev_s * d) {
    JSDRV_LOGI("device_open(%s) %s", d->device.prefix, d->device_path);

    d->file = CreateFile(
            d->device_path,                                 // lpFileName
            GENERIC_WRITE | GENERIC_READ,                   // dwDesiredAccess
            FILE_SHARE_WRITE | FILE_SHARE_READ,             // dwShareMode
            NULL,                                           // lpSecurityAttributes
            OPEN_EXISTING,                                  // dwCreationDisposition, open only if exists
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,   // dwFlagsAndAttributes
            NULL                                            // hTemplateFile
    );
    if (d->file == INVALID_HANDLE_VALUE) {
        JSDRV_LOGE("device_open(%s) could not open device path %s", d->device.prefix, d->device_path);
        return 1;
    }
    if (!WinUsb_Initialize(d->file, &d->winusb)) {
        WINDOWS_LOGE("WinUsb_Initialize %s", d->device.prefix);
        return 1;
    }
    DWORD ctrl_timeout = CONTROL_TIMEOUT_MS;
    if (!WinUsb_SetPipePolicy(d->winusb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(ctrl_timeout), &ctrl_timeout)) {
        JSDRV_LOGW("WinUsb_SetPipePolicy failed");
    }

    jsdrv_list_initialize(&d->ctrl_list); // jsdrvp_msg_s
    return 0;
}

static void ep_finalize_by_id(struct dev_s * d, uint8_t ep_id) {
    if (d->endpoints[ep_id]) {
        jsdrv_list_remove(&d->endpoints[ep_id]->item);
        d->endpoints[ep_id]->finalize(d->endpoints[ep_id]);
        d->endpoints[ep_id] = NULL;
    }
}

static void device_close(struct dev_s * d) {
    JSDRV_LOGI("device_close(%s)", d->device.prefix);

    for (uint32_t idx = 1; idx < JSDRV_ARRAY_SIZE(d->endpoints); ++idx) {
        ep_finalize_by_id(d, idx);
    }

    if (INVALID_HANDLE_VALUE != d->winusb) {
        WinUsb_Free(d->winusb);
        d->winusb = INVALID_HANDLE_VALUE;
    }
    if (INVALID_HANDLE_VALUE != d->file) {
        CloseHandle(d->file);
        d->file = INVALID_HANDLE_VALUE;
    }
}

static void ctrl_send_done(struct dev_s * d, int32_t ec) {
    struct jsdrv_list_s * item = jsdrv_list_remove_head(&d->ctrl_list);
    if (!item) {
        return;
    }
    struct jsdrvp_msg_s * msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
    msg->extra.bkusb_ctrl.status = ec;
    msg_queue_push(d->device.rsp_q, msg);
}

static void ctrl_start_next(struct dev_s * d) {
    struct jsdrv_list_s * item = jsdrv_list_peek_head(&d->ctrl_list);
    if (!item) {
        return;
    }
    JSDRV_LOGD1("ctrl_start_next");
    struct jsdrvp_msg_s * msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);

    memset(&d->ctrl_overlapped, 0, sizeof(d->ctrl_overlapped));
    d->ctrl_overlapped.hEvent = d->ctrl_event;
    ULONG buf_sz = msg->extra.bkusb_ctrl.setup.s.wLength;
    ULONG sz = 0;
    WINUSB_SETUP_PACKET setup = *((WINUSB_SETUP_PACKET *) &msg->extra.bkusb_ctrl.setup.u64);

    BOOL rb = WinUsb_ControlTransfer(d->winusb, setup, msg->payload.bin, buf_sz, &sz, &d->ctrl_overlapped);
    if (rb || (GetLastError() != ERROR_IO_PENDING)) {
        WINDOWS_LOGE("%s", "ctrl_start_next failed");
        ctrl_send_done(d, JSDRV_ERROR_UNSPECIFIED);
        return;
    }
    JSDRV_LOGD1("ctrl %" PRIx64 " len=%d", msg->extra.bkusb_ctrl.setup.u64, buf_sz);
}

static void ctrl_complete(struct dev_s * d) {
    struct jsdrv_list_s * item = jsdrv_list_peek_head(&d->ctrl_list);
    if (!item) {
        return;
    }
    struct jsdrvp_msg_s * msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);

    ULONG sz = 0;
    if (0 == WinUsb_GetOverlappedResult(d->winusb, &d->ctrl_overlapped, &sz, TRUE)) {
        DWORD ec = GetLastError();
        if ((ec == ERROR_IO_INCOMPLETE) || (ec == ERROR_IO_PENDING)) {
            // still in progress
            return;
        } else {
            WINDOWS_LOGE("%s", "ctrl_complete failed");
            ctrl_send_done(d, JSDRV_ERROR_UNSPECIFIED); // unspecified error
            return;
        }
    } else if (USB_REQUEST_TYPE_IS_IN(msg->extra.bkusb_ctrl.setup)) {
        if (sz > msg->extra.bkusb_ctrl.setup.s.wLength) {  // should never happen
            JSDRV_LOGE("ctrl too long: %d %d", (int) sz, (int) msg->extra.bkusb_ctrl.setup.s.wLength);
            ctrl_send_done(d, JSDRV_ERROR_UNSPECIFIED);
            return;
        }
        msg->value.size = sz;
    } else {  // out
        // no message updates needed
    }
    ctrl_send_done(d, 0);
    ctrl_start_next(d);
}

static void ctrl_add(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    JSDRV_LOGD1("ctrl_add");
    bool do_issue = jsdrv_list_is_empty(&d->ctrl_list);
    jsdrv_list_add_tail(&d->ctrl_list, &msg->item);
    if (do_issue) {
        ctrl_start_next(d);
    }
}

static bool device_handle_msg(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_USBBK_MSG_STREAM_IN_DATA, msg->topic)) {
        struct bulk_in_transfer_s * t = JSDRV_CONTAINER_OF(msg->value.value.bin, struct bulk_in_transfer_s, buffer);
        bulk_in_transfer_free(t);
        jsdrvp_msg_free(d->context, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic)) {
        bulk_out_send(d, msg);
    } else if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, msg->topic)) {
            int32_t rc = device_open(d);
            msg->value = jsdrv_union_i32(rc);
            msg_queue_push(d->device.rsp_q, msg);
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, msg->topic)) {
            device_close(d);
            msg->value = jsdrv_union_i32(0);
            msg_queue_push(d->device.rsp_q, msg);
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            msg->value = jsdrv_union_i32(0);
            msg_queue_push(d->device.rsp_q, msg);
            d->do_exit = true;
            return false;
        } else {
            JSDRV_LOGE("device_handle_msg unsupported %s", msg->topic);
            jsdrvp_msg_free(d->context, msg);
        }
    } else if (0 == strcmp(JSDRV_USBBK_MSG_CTRL_IN, msg->topic)) {
        ctrl_add(d, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_CTRL_OUT, msg->topic)) {
        ctrl_add(d, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, msg->topic)) {
        uint8_t ep = msg->extra.bkusb_stream.endpoint;
        JSDRV_LOGI("bulk_in_stream_open %d", (int) ep);
        ep_finalize_by_id(d, ep);
        struct bulk_in_s * b = bulk_in_initialize(d, ep);
        d->endpoints[ep] = &b->ep;
        jsdrv_list_add_tail(&d->endpoints_active, &b->ep.item);
        d->update_handles = true;
        msg->value = jsdrv_union_i32(0);  // return code
        msg_queue_push(d->device.rsp_q, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_CLOSE, msg->topic)) {
        uint8_t ep = msg->extra.bkusb_stream.endpoint;
        JSDRV_LOGI("bulk_in_stream_close %d", (int) ep);
        ep_finalize_by_id(d, ep);
        d->update_handles = true;
        msg->value = jsdrv_union_i32(0);  // return code
        msg_queue_push(d->device.rsp_q, msg);
    } else {
        JSDRV_LOGW("unsupported topic %s", msg->topic);
        msg->value = jsdrv_union_i32(JSDRV_ERROR_PARAMETER_INVALID);
        msg_queue_push(d->device.rsp_q, msg);
    }
    return true;
}

static DWORD WINAPI device_thread(LPVOID lpParam) {
    struct dev_s *d = (struct dev_s *) lpParam;
    JSDRV_LOGI("USB device_thread started %s", d->device.prefix);
    d->update_handles = true;
    struct jsdrv_list_s * item;
    struct endpoint_s * ep;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count = 0;

    while (!d->do_exit) {
        if (d->update_handles) {
            handle_count = 0;
            handles[handle_count++] = msg_queue_handle_get(d->device.cmd_q);
            handles[handle_count++] = d->ctrl_event;
            jsdrv_list_foreach(&d->endpoints_active, item) {
                ep = JSDRV_CONTAINER_OF(item, struct endpoint_s, item);
                if (ep->event) {
                    handles[handle_count++] = ep->event;
                }
            }
            JSDRV_LOGD2("device_thread handle_count=%d", (int) handle_count);
        }
        WaitForMultipleObjects(handle_count, handles, false, 5000);
        //JSDRV_LOGD3("winusb ll thread %s", d->device.prefix);
        if (WAIT_OBJECT_0 == WaitForSingleObject(handles[0], 0)) {
            // note: ResetEvent handled automatically by msg_queue_pop_immediate
            while (device_handle_msg(d, msg_queue_pop_immediate(d->device.cmd_q))) {
                ; //
            }
        }
        if (WAIT_OBJECT_0 == WaitForSingleObject(d->ctrl_event, 0)) {
            ResetEvent(d->ctrl_event);
            ctrl_complete(d);
        }
        jsdrv_list_foreach(&d->endpoints_active, item) {
            ep = JSDRV_CONTAINER_OF(item, struct endpoint_s, item);
            if (WAIT_OBJECT_0 == WaitForSingleObject(ep->event, 0)) {
                ep->process(ep);
            }
        }
    }

    JSDRV_LOGI("USB device_thread closing %s", d->device.prefix);
    device_close(d);
    jsdrvp_send_finalize_msg(d->context, d->device.rsp_q, d->device.prefix);
    JSDRV_LOGI("USB device_thread closed %s", d->device.prefix);
    return 0;
}

//#############################################################################
//# DISCOVERY                                                                 #
//#############################################################################

struct backend_s {
    struct jsdrvbk_s backend;
    struct jsdrv_context_s * context;

    struct dev_s devices[DEVICES_MAX];
    struct jsdrv_list_s devices_free;
    struct jsdrv_list_s devices_active;

    HANDLE discovery;  // discovery event
    bool do_exit;
    HANDLE thread;
    DWORD thread_id;
};

static void on_device_change(void* cookie) {
    struct backend_s * s = (struct backend_s *) cookie;
    JSDRV_LOGD1("on_device_change");
    if (s->discovery) {
        SetEvent(s->discovery);
    }
}

static void device_path_to_serial_number(const char * device_path, char * sn) {
    int section = 0;
    while (*device_path) {
        if (*device_path == '#') {
            ++section;
            ++device_path;
            continue;
        }
        if (section == 2) {
            *sn++ = *device_path++;
            *sn = 0;
        } else {
            ++device_path;
        }
    }
}

static void device_thread_stop(struct dev_s * d) {
    if (d->thread) {
        jsdrvp_send_finalize_msg(d->context, d->device.cmd_q, d->device.prefix);
        // and wait for thread to exit.
        if (WAIT_OBJECT_0 != WaitForSingleObject(d->thread, 1000)) {
            JSDRV_LOGW("device thread %s not closed cleanly.", d->device.prefix);
        }
        CloseHandle(d->thread);
        d->thread = NULL;
    }
}

static int32_t device_thread_start(struct dev_s * d) {
    device_thread_stop(d);
    d->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            device_thread,          // thread function name
            d,                 // argument to thread function
            0,                      // use default creation flags
            &d->thread_id);    // returns the thread identifier
    if (d->thread == NULL) {
        JSDRV_LOGE("CreateThread device failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (!SetThreadPriority(d->thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }
    return 0;
}

static int32_t device_add(struct backend_s * s, const struct device_type_s * device_type, const char * device_path) {
    int32_t rc = 0;
    char serial_number[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrvp_msg_s * msg;

    struct jsdrv_list_s * item = jsdrv_list_remove_head(&s->devices_free);
    if (!item) {
        JSDRV_LOGE("device_add_msg %s but too many devices", device_path);
        return JSDRV_ERROR_NOT_ENOUGH_MEMORY;
    }

    struct dev_s * device = JSDRV_CONTAINER_OF(item, struct dev_s, item);
    device->mark = DEVICE_MARK_ADDED;
    device->do_exit = false;
    device->device_type = device_type;
    jsdrv_cstr_copy(device->device_path, device_path, sizeof(device->device_path));
    device_path_to_serial_number(device_path, serial_number);
    tfp_snprintf(device->device.prefix, sizeof(device->device.prefix), "%c/%s/%s",
                 s->backend.prefix, device_type->model, serial_number);
    JSDRV_LOGI("device_add_msg %s : %s", device->device.prefix, device_path);
    jsdrv_list_initialize(&device->endpoints_active);
    jsdrv_list_add_tail(&s->devices_active, &device->item);

    while (msg_queue_pop_immediate(device->device.cmd_q)) {
        ; // pop all pending commands from old instance
    }

    rc = device_thread_start(device);
    if (rc) {
        jsdrv_list_add_tail(&s->devices_free, &device->item);
    } else {
        // send device added message to frontend
        msg = jsdrvp_msg_alloc(s->context);
        jsdrv_cstr_copy(msg->topic, JSDRV_MSG_DEVICE_ADD, sizeof(msg->topic));
        msg->value.type = JSDRV_UNION_BIN;
        msg->value.app = JSDRV_PAYLOAD_TYPE_DEVICE;
        msg->value.value.bin = (const uint8_t *) &msg->payload.device;
        msg->payload.device = device->device;
        jsdrvp_backend_send(s->context, msg);
    }
    return rc;
}

static void device_remove(struct dev_s * d) {
    JSDRV_LOGI("device_remove_msg(%s): %s", d->device.prefix, d->device_path);
    device_thread_stop(d);

    // send device remove message to frontend
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc(d->context);
    jsdrv_cstr_copy(msg->topic, JSDRV_MSG_DEVICE_REMOVE, sizeof(msg->topic));
    msg->value.type = JSDRV_UNION_STR;
    msg->value.value.str = msg->payload.str;
    jsdrv_cstr_copy(msg->payload.str, d->device.prefix, sizeof(msg->payload.str));
    jsdrvp_backend_send(d->context, msg);
}

static bool device_update(struct backend_s * s, const struct device_type_s * device_type, const char * device_path) {
    struct jsdrv_list_s* item;
    struct dev_s * device;
    jsdrv_list_foreach(&s->devices_active, item) {
        device = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        if (0 == strcmp(device->device_path, device_path)) {
            JSDRV_LOGD1("device match %s", device_path);
            device->mark = DEVICE_MARK_FOUND;
            return false; // already exists, no change
        }
    }
    device_add(s, device_type, device_path);
    return true;
}

static void device_free(struct backend_s * s, struct dev_s * d) {
    jsdrv_list_remove(&d->item);
    device_thread_stop(d);

    if (d->device.cmd_q) {
        msg_queue_finalize(d->device.cmd_q);
        d->device.cmd_q = NULL;
    }

    if (d->device.rsp_q) {
        msg_queue_finalize(d->device.rsp_q);
        d->device.rsp_q = NULL;
    }

    if (d->ctrl_event) {
        CloseHandle(d->ctrl_event);
        d->ctrl_event = NULL;
    }

    d->device_type = NULL;
    d->device_path[0] = 0;
    d->device.prefix[0] = 0;
    jsdrv_list_add_tail(&s->devices_free, &d->item);
}

static void device_scan(struct backend_s * s) {
    char device_path[DEVICE_PATH_MAX];
    SP_DEVICE_INTERFACE_DATA dev_interface;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W * dev_interface_detail;
    const struct device_type_s * dt = device_types;

    JSDRV_LOGD1("device_scan");
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        s->devices[i].mark = DEVICE_MARK_NONE;
    }

    while (dt->device_type) {
        JSDRV_LOGD1("scan device_type %d: model=%s, vid=0x%04x, pid=0x%04x",
                  dt->device_type, dt->model, dt->vendor_id, dt->product_id);
        HANDLE handle = SetupDiGetClassDevsW(&dt->guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (!handle) {
            JSDRV_LOGE("SetupDiGetClassDevs failed.");
            continue;
        }
        DWORD member_index = 0;
        jsdrv_memset(&dev_interface, 0, sizeof(dev_interface));
        dev_interface.cbSize = sizeof(dev_interface);
        while (SetupDiEnumDeviceInterfaces(handle, NULL, &dt->guid, member_index, &dev_interface)) {
            DWORD required_size = 0;
            SetupDiGetDeviceInterfaceDetailW(handle, &dev_interface, NULL, 0, &required_size, 0);
            dev_interface_detail = jsdrv_alloc_clr(required_size);
            dev_interface_detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(
                    handle,
                    &dev_interface,
                    dev_interface_detail, required_size, &required_size, 0)) {
                wcstombs_s(0, device_path, sizeof(device_path), dev_interface_detail->DevicePath, _TRUNCATE);
                jsdrv_free(dev_interface_detail);
                device_update(s, dt, device_path);
            } else {
                WINDOWS_LOGE("SetupDiGetDeviceInterfaceDetailW failed %d", dt->device_type);
            }
            jsdrv_memset(&dev_interface, 0, sizeof(dev_interface));
            dev_interface.cbSize = sizeof(dev_interface);
            ++member_index;
        }
        SetupDiDestroyDeviceInfoList(handle);
        ++dt;
    }

    // Find removed devices.
    struct jsdrv_list_s* item;
    struct dev_s* device;
    jsdrv_list_foreach(&s->devices_active, item) {
        device = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        if (DEVICE_MARK_NONE == device->mark) {
            device_remove(device);
            device->mark = DEVICE_MARK_REMOVED;
            jsdrv_list_remove(&device->item);
            jsdrv_list_add_tail(&s->devices_free, &device->item);
        }
    }
}

static bool handle_msg(struct backend_s * s, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
        JSDRV_LOGI("winusb backend finalize");
        s->do_exit = true;
        rv = false;
    } else {
        JSDRV_LOGE("backend_handle_msg unsupported %s", msg->topic);
    }
    jsdrvp_msg_free(s->context, msg);
    return rv;
}

static void backend_ready(struct backend_s * s) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_value(s->context, JSDRV_MSG_INITIALIZE, &jsdrv_union_i32(0));
    msg->payload.str[0] = s->backend.prefix;
    jsdrvp_backend_send(s->context, msg);
}

static DWORD WINAPI backend_thread(LPVOID lpParam) {
    struct backend_s * s = (struct backend_s *) lpParam;
    JSDRV_LOGI("USB backend_thread started");

    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count = 0;
    handles[0] = s->discovery;
    handles[1] = msg_queue_handle_get(s->backend.cmd_q);
    handle_count = 2;

    device_scan(s);
    backend_ready(s);

    while (!s->do_exit) {
        WaitForMultipleObjects(handle_count, handles, false, 5000);
        if (WAIT_OBJECT_0 == WaitForSingleObject(handles[0], 0)) {
            ResetEvent(handles[0]);
            device_scan(s);
        }
        if (WAIT_OBJECT_0 == WaitForSingleObject(handles[1], 0)) {
            // note: ResetEvent handled automatically by msg_queue_pop_immediate
            while (handle_msg(s, msg_queue_pop_immediate(s->backend.cmd_q))) {
                ; //
            }
        }
    }

    JSDRV_LOGI("USB backend_thread done");
    return 0;
}

static void finalize(struct jsdrvbk_s * backend) {
    char topic[2] = {backend->prefix, 0};
    if (backend) {
        struct backend_s * s = (struct backend_s *) backend;
        JSDRV_LOGI("finalize usb backend");
        device_change_notifier_finalize();
        if (s->thread) {
            jsdrvp_send_finalize_msg(s->context, s->backend.cmd_q, topic);
            // and wait for thread to exit.
            if (WAIT_OBJECT_0 != WaitForSingleObject(s->thread, 1000)) {
                JSDRV_LOGE("winusb thread not closed cleanly.");
            }
            CloseHandle(s->thread);
            s->thread = NULL;
        }
        if (s->backend.cmd_q) {
            msg_queue_finalize(s->backend.cmd_q);
            s->backend.cmd_q = NULL;
        }
        if (s->discovery) {
            CloseHandle(s->discovery);
            s->discovery = NULL;
        }

        for (uint16_t i = 0; i < DEVICES_MAX; ++i) {
            struct dev_s * d = &s->devices[i];
            device_free(s, d);
        }

        jsdrv_free(s);
    }
}

int32_t jsdrv_usb_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend) {
    JSDRV_LOGI("jsdrv_usb_backend_factory");
    struct backend_s * s = jsdrv_alloc_clr(sizeof(struct backend_s));
    s->context = context;
    s->backend.prefix = 'u';
    s->backend.finalize = finalize;
    s->backend.cmd_q = msg_queue_init();

    jsdrv_list_initialize(&s->devices_free);
    jsdrv_list_initialize(&s->devices_active);
    for (uint16_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s * d = &s->devices[i];
        d->winusb = INVALID_HANDLE_VALUE;
        d->file = INVALID_HANDLE_VALUE;
        d->context = context;
        d->device.cmd_q = msg_queue_init();
        d->device.rsp_q = msg_queue_init();
        d->ctrl_event = CreateEvent(
                NULL,  // default security attributes
                TRUE,  // manual reset event
                FALSE, // start unsignalled
                NULL   // no name
        );
        JSDRV_ASSERT(d->ctrl_event);
        jsdrv_list_initialize(&d->item);
        jsdrv_list_add_tail(&s->devices_free, &d->item);
        jsdrv_list_initialize(&d->ctrl_list);
    }

    s->discovery = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            FALSE, // start unsignalled
            NULL   // no name
    );
    if (!s->discovery) {
        JSDRV_LOGE("CreateEvent discovery failed");
        finalize(&s->backend);
        return JSDRV_ERROR_UNSPECIFIED;
    }

    if (device_change_notifier_initialize(on_device_change, s)) {
        JSDRV_LOGE("device_change_notifier_initialize failed");
        finalize(&s->backend);
        return JSDRV_ERROR_UNSPECIFIED;
    }

    s->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            backend_thread,         // thread function name
            s,                      // argument to thread function
            0,                      // use default creation flags
            &s->thread_id);         // returns the thread identifier
    if (s->thread == NULL) {
        JSDRV_LOGE("CreateThread failed");
        finalize(&s->backend);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (!SetThreadPriority(s->thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }

    *backend = &s->backend;
    return 0;
}
