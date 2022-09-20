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
 * @brief Joulescope driver libusb backend
 */

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/devices.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv/error_code.h"
#include "jsdrv/cstr.h"
#include "tinyprintf.h"
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include "libusb.h"


#define CTRL_TIMEOUT_MS                 (1000U)
#define DEVICES_MAX                     (256U)
#define BULK_OUT_TIMEOUT_MS             (250U)
#define BULK_IN_TIMEOUT_MS              (50U)
#define BULK_IN_FRAME_LENGTH            (512U)
#define BULK_IN_TRANSFER_SIZE           (64U * BULK_IN_FRAME_LENGTH)
#define BULK_IN_TRANSFER_OUTSTANDING    (4U)
#define ENDPOINT_MAX                    (255U)


enum device_mark_e {
    DEVICE_MARK_NONE = 0,
    DEVICE_MARK_FOUND = 1,
    DEVICE_MARK_ADDED = 2,
    DEVICE_MARK_REMOVED = 3,
};

enum device_mode_e {
    DEVICE_MODE_UNASSIGNED,  // not present or remove
    DEVICE_MODE_CLOSED,      // present but not in use
    DEVICE_MODE_OPEN,        // present and in use
    DEVICE_MODE_CLOSING,
};

enum endpoint_mode_e {
    EP_MODE_OFF = 0,
    EP_MODE_BULK_OUT = 0x01,
    EP_MODE_BULK_IN = 0x81,
};

struct dev_s;
struct backend_s;

struct transfer_s {
    struct libusb_transfer * transfer;      // user_data points to the transfer_s instance
    struct jsdrvp_msg_s * msg;              // not for BULK IN
    struct dev_s * device;
    uint8_t buffer[BULK_IN_TRANSFER_SIZE];  // OUT uses msg->value.value.bin
    struct jsdrv_list_s item;
};

struct dev_s {
    struct jsdrvp_ll_device_s ll_device;
    libusb_device * usb_device;
    libusb_device_handle * handle;
    struct backend_s * backend;
    const struct device_type_s * device_type;
    struct libusb_device_descriptor device_descriptor;
    char serial_number[JSDRV_TOPIC_LENGTH_MAX];
    uint8_t mode;  // device_mode_e
    uint8_t mark;
    uint8_t endpoint_mode[ENDPOINT_MAX];

    struct jsdrv_list_s transfers_pending;
    struct jsdrv_list_s transfers_free;

    struct jsdrv_list_s item;
};

struct backend_s {
    struct jsdrvbk_s backend;
    struct jsdrv_context_s * context;

    libusb_context * ctx;
    libusb_hotplug_callback_handle hotplug_callback_handle;

    struct dev_s devices[DEVICES_MAX];
    struct jsdrv_list_s devices_free;
    struct jsdrv_list_s devices_active;

    jsdrv_os_event_t hotplug_event;
    volatile bool do_exit;
    pthread_t thread_id;
};

static struct transfer_s * transfer_alloc(struct dev_s * d) {
    struct transfer_s * t;
    struct jsdrv_list_s * item = jsdrv_list_remove_head(&d->transfers_free);
    if (NULL != item) {
        t = JSDRV_CONTAINER_OF(item, struct transfer_s, item);
    } else {
        t = jsdrv_alloc_clr(sizeof(struct transfer_s));
        jsdrv_list_initialize(&t->item);
        t->transfer = libusb_alloc_transfer(0);
    }
    t->device = d;
    jsdrv_list_add_tail(&d->transfers_pending, &t->item);
    return t;
}

static void transfer_free(struct transfer_s * t) {
    if (NULL == t) {
        return;
    }
    t->msg = NULL;  // do not free, must do externally.
    jsdrv_list_remove(&t->item);
    if (NULL != t->device->handle) {
        jsdrv_list_add_tail(&t->device->transfers_free, &t->item);
    } else {
        if (NULL != t->transfer) {
            libusb_free_transfer(t->transfer);
            t->transfer = NULL;
        }
        t->device = NULL;
        jsdrv_free(t);
    }
}

static int on_hotplug(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data) {
    (void) ctx;
    JSDRV_LOGI("hotplug %p: inserted=%d, remove=%d",
               device,
               (event & LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) ? 1 : 0,
               (event & LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) ? 1 : 0);
    struct backend_s * s = (struct backend_s *) user_data;
    jsdrv_os_event_signal(s->hotplug_event);
    return 0;
}

static struct dev_s * device_lookup_by_usb_device(struct backend_s * s, libusb_device * usb_device) {
    struct jsdrv_list_s * item;
    jsdrv_list_foreach(&s->devices_active, item) {
        struct dev_s * d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        if (d->usb_device == usb_device) {
            return d;
        }
    }
    return NULL;
}

static void device_announce(struct backend_s * s, struct dev_s * d, const char * topic) {
    JSDRV_LOGI("device_announce %s %s", d->ll_device.prefix, topic);
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc(s->context);
    jsdrv_cstr_copy(msg->topic, topic, sizeof(msg->topic));
    msg->value.type = JSDRV_UNION_BIN;
    msg->value.app = JSDRV_PAYLOAD_TYPE_DEVICE;
    msg->value.value.bin = (const uint8_t *) &msg->payload.device;
    msg->payload.device = d->ll_device;
    jsdrvp_backend_send(s->context, msg);
}

static void device_close(struct dev_s * d) {
    struct jsdrv_list_s * item;
    struct transfer_s * t;
    if (d->handle && (d->mode == DEVICE_MODE_OPEN)) {
        JSDRV_LOGI("device_close(%s)", d->ll_device.prefix);
        for (uint32_t endpoint = 0; endpoint <= ENDPOINT_MAX; ++endpoint) {
            d->endpoint_mode[endpoint] = EP_MODE_OFF;
        }
        jsdrv_list_foreach_reverse(&d->transfers_pending, item) {
            t = JSDRV_CONTAINER_OF(item, struct transfer_s, item);
            if (d->endpoint_mode[t->transfer->endpoint] == EP_MODE_BULK_IN) {
                libusb_cancel_transfer(t->transfer);
            }
        }
        if (jsdrv_list_is_empty(&d->transfers_pending)) {
            // cannot call libusb_close from event callbacks
            // post to guarantee handling outside of libusb_handle_events*
            d->mode = DEVICE_MODE_CLOSING;
        }
    }
}

static int32_t device_open(struct dev_s * d) {
    int rc;
    device_close(d);
    JSDRV_LOGI("device_open(%s)", d->ll_device.prefix);
    rc = libusb_open(d->usb_device, &d->handle);
    if (rc) {
        if (rc == LIBUSB_ERROR_ACCESS) {
            JSDRV_LOGE("libusb_open - insufficient permissions");
        } else if (rc) {
            JSDRV_LOGE("libusb_open failed %d", rc);
        }
        return (int32_t) rc;
    }

    rc = libusb_set_configuration(d->handle, 1);
    if (rc) {
        JSDRV_LOGE("libusb_set_configuration failed: %d", rc);
        return (int32_t) rc;
    }
    rc = libusb_claim_interface(d->handle, 0);
    if (rc) {
        JSDRV_LOGE("libusb_claim_interface failed: %d", rc);
        return (int32_t) rc;
    }
    rc = libusb_set_interface_alt_setting(d->handle, 0, 0);
    if (rc) {
        JSDRV_LOGE("libusb_set_interface_alt_setting failed: %d", rc);
        return (int32_t) rc;
    }
    d->mode = DEVICE_MODE_OPEN;
    return (int32_t) rc;
}

static const char * transfer_status_to_str(int32_t status) {
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED: return "COMPLETED";
        case LIBUSB_TRANSFER_ERROR: return "ERROR";
        case LIBUSB_TRANSFER_TIMED_OUT: return "TIMED OUT";
        case LIBUSB_TRANSFER_CANCELLED: return "CANCELLED";
        case LIBUSB_TRANSFER_STALL: return "STALL";
        case LIBUSB_TRANSFER_NO_DEVICE: return "NO DEVICE";
        case LIBUSB_TRANSFER_OVERFLOW: return "OVERFLOW";
        default: return "UNKNOWN";
    }
}

static void submit_transfer(struct transfer_s * t) {
    int rc = libusb_submit_transfer(t->transfer);
    if (rc) {
        JSDRV_LOGW("libusb_submit_transfer returned %d", rc);
        if (t->msg) {
            if ((t->transfer->endpoint & 0x7f) == 0) {
                t->msg->extra.bkusb_ctrl.status = JSDRV_ERROR_IO;
            } else {
                t->msg->extra.bkusb_stream.endpoint = t->transfer->endpoint;
                t->msg->value = jsdrv_union_i32(JSDRV_ERROR_IO);
            }
            msg_queue_push(t->device->ll_device.rsp_q, t->msg);
        }
        transfer_free(t);
    }
}

static void device_rsp(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    if (NULL != d->ll_device.rsp_q) {
        msg_queue_push(d->ll_device.rsp_q, msg);
    } else {
        jsdrvp_msg_free(d->backend->context, msg);
    }
}

static void device_rsp_transfer(struct transfer_s * t) {
    device_rsp(t->device, t->msg);
    transfer_free(t);
}

static void on_bulk_out_done(struct libusb_transfer * transfer) {
    struct transfer_s *t = (struct transfer_s *) transfer->user_data;
    int32_t rc = 0;
    if (transfer->status) {
        if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
            rc = JSDRV_ERROR_ABORTED;
        } else {
            rc = JSDRV_ERROR_IO;
        }
        JSDRV_LOGW("bulk out returned %d %s", transfer->status, transfer_status_to_str(transfer->status));
    }
    t->msg->value = jsdrv_union_i32(rc);
    device_rsp_transfer(t);
}

static void bulk_out_send(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    struct transfer_s * t = transfer_alloc(d);
    t->msg = msg;
    JSDRV_LOGI("bulk_out_send(%s) %d bytes", d->ll_device.prefix, (int) msg->value.size);
    uint8_t ep = msg->extra.bkusb_stream.endpoint;
    libusb_fill_bulk_transfer(t->transfer, d->handle, ep,
                              msg->payload.bin, msg->value.size,
                              on_bulk_out_done, t, BULK_OUT_TIMEOUT_MS);
    submit_transfer(t);
}

static void on_ctrl_in_done(struct libusb_transfer * transfer) {
    struct transfer_s *t = (struct transfer_s *) transfer->user_data;
    int32_t rc = 0;
    JSDRV_LOGI("ctrl_in_done(%s) %d", t->device->ll_device.prefix, transfer->status);
    if (transfer->status) {
        if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
            rc = JSDRV_ERROR_ABORTED;
        } else {
            rc = JSDRV_ERROR_IO;
        }
    } else {
        memcpy(t->msg->payload.bin, t->buffer + 8, transfer->actual_length);
        t->msg->value = jsdrv_union_bin(t->msg->payload.bin, transfer->actual_length);
    }
    t->msg->extra.bkusb_ctrl.status = rc;
    device_rsp_transfer(t);
}

static void ctrl_in_start(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    struct transfer_s * t = transfer_alloc(d);
    t->msg = msg;
    JSDRV_LOGI("ctrl_in_start(%s)", d->ll_device.prefix);
    uint64_t * setup = (uint64_t * ) t->buffer;
    *setup = msg->extra.bkusb_ctrl.setup.u64;
    libusb_fill_control_transfer(t->transfer, d->handle, t->buffer,
                                 on_ctrl_in_done, t, CTRL_TIMEOUT_MS);
    submit_transfer(t);
}

static void on_ctrl_out_done(struct libusb_transfer * transfer) {
    struct transfer_s *t = (struct transfer_s *) transfer->user_data;
    int32_t rc = 0;
    JSDRV_LOGI("ctrl_out_done(%s) %d", t->device->ll_device.prefix, transfer->status);
    if (transfer->status) {
        if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
            rc = JSDRV_ERROR_ABORTED;
        } else {
            rc = JSDRV_ERROR_IO;
        }
    }
    t->msg->extra.bkusb_ctrl.status = rc;
    device_rsp_transfer(t);
}

static void ctrl_out_start(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    struct transfer_s * t = transfer_alloc(d);
    t->msg = msg;
    JSDRV_LOGI("ctrl_in_start(%s) %d bytes", d->ll_device.prefix, (int) msg->value.size);
    uint64_t * setup = (uint64_t * ) t->buffer;
    *setup = msg->extra.bkusb_ctrl.setup.u64;
    libusb_fill_control_transfer(t->transfer, d->handle, t->buffer,
                                 on_ctrl_out_done, t, CTRL_TIMEOUT_MS);
    if (msg->value.size > 4096) {
        msg->value = jsdrv_union_i32(JSDRV_ERROR_TOO_BIG);
        device_rsp(d, msg);
    } else {
        memcpy(&setup[1], msg->value.value.bin, msg->value.size);
        submit_transfer(t);
    }
}

static void bulk_in_start(struct dev_s * d, uint8_t pipe_id);

static void on_bulk_in_done(struct libusb_transfer * transfer) {
    struct transfer_s *t = (struct transfer_s *) transfer->user_data;
    struct dev_s * d = t->device;
    uint8_t pipe_id = transfer->endpoint;
    struct jsdrvp_msg_s * m;
    JSDRV_LOGI("bulk_in_done(%s) status=%d, length=%d",
               d->ll_device.prefix, transfer->status, t->transfer->actual_length);
    switch (transfer->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            bulk_in_start(t->device, pipe_id);
            if (0 == t->transfer->actual_length) {
                JSDRV_LOGW("zero length bulk in transfer");
                transfer_free(t);
            } else {
                jsdrv_list_remove(&t->item); // not pending or free, temporary loan to upper layer
                m = jsdrvp_msg_alloc(t->device->backend->context);
                jsdrv_cstr_copy(m->topic, JSDRV_USBBK_MSG_STREAM_IN_DATA, sizeof(m->topic));
                m->value = jsdrv_union_bin(t->buffer, t->transfer->actual_length);
                m->extra.bkusb_stream.endpoint = t->transfer->endpoint;
                device_rsp(d, m);
            }
            break;
        case LIBUSB_TRANSFER_TIMED_OUT:
            bulk_in_start(t->device, pipe_id);
            transfer_free(t);
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            transfer_free(t);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            t->device->mode = DEVICE_MODE_UNASSIGNED;
            transfer_free(t);
            break;
        default:
            JSDRV_LOGW("bulk_in error %d", transfer->status);
            transfer_free(t);
            break;
    }
}

static void bulk_in_start(struct dev_s * d, uint8_t pipe_id) {
    if (d->mode != DEVICE_MODE_OPEN) {
        return;
    }
    if (d->endpoint_mode[pipe_id] != EP_MODE_BULK_IN) {
        return;
    }
    struct transfer_s * t = transfer_alloc(d);
    libusb_fill_bulk_transfer(t->transfer, d->handle,
                              pipe_id, t->buffer, sizeof(t->buffer),
                              on_bulk_in_done, t, BULK_IN_TIMEOUT_MS);
    submit_transfer(t);
}

static void bulk_in_open(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    uint8_t ep = msg->extra.bkusb_stream.endpoint;
    uint8_t pipe_id = ep | 0x80;
    JSDRV_LOGI("bulk_in_open(%d, endpoint=0x%02x)", d->ll_device.prefix, (int) ep);
    d->endpoint_mode[pipe_id] = EP_MODE_BULK_IN;
    libusb_clear_halt(d->handle, pipe_id);
    for (uint32_t i = 0; i < BULK_IN_TRANSFER_OUTSTANDING; ++i) {
        bulk_in_start(d, pipe_id);
    }
    msg->value = jsdrv_union_i32(0);  // return code
    device_rsp(d, msg);
}

static void bulk_in_close(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    uint8_t ep = msg->extra.bkusb_stream.endpoint;
    JSDRV_LOGI("bulk_in_close %d", (int) ep);
    d->endpoint_mode[ep] = EP_MODE_OFF;
    struct jsdrv_list_s * item;
    struct transfer_s * t;
    jsdrv_list_foreach(&d->transfers_pending, item) {
        t = JSDRV_CONTAINER_OF(item, struct transfer_s, item);
        if (t->transfer->endpoint == ep) {
            libusb_cancel_transfer(t->transfer);
        }
    }
    msg->value = jsdrv_union_i32(0);  // return code
    device_rsp(d, msg);
}

static bool device_handle_msg(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    if (NULL == msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_USBBK_MSG_STREAM_IN_DATA, msg->topic)) {
        struct transfer_s * t;
        t = JSDRV_CONTAINER_OF(msg->value.value.bin, struct transfer_s, buffer);
        transfer_free(t);
        jsdrvp_msg_free(d->backend->context, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic)) {
        bulk_out_send(d, msg);
    } else if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, msg->topic)) {
            int32_t rc = device_open(d);
            JSDRV_LOGI("device_open(%s) => %d", d->ll_device.prefix, rc);
            msg->value = jsdrv_union_i32(rc);
            msg_queue_push(d->ll_device.rsp_q, msg);
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, msg->topic)) {
            device_close(d);
            msg->value = jsdrv_union_i32(0);
            msg_queue_push(d->ll_device.rsp_q, msg);
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            device_close(d);
            msg->value = jsdrv_union_i32(0);
            msg_queue_push(d->ll_device.rsp_q, msg);
            return false;
        } else {
            JSDRV_LOGE("device_handle_msg unsupported %s", msg->topic);
            jsdrvp_msg_free(d->backend->context, msg);
        }
    } else if (0 == strcmp(JSDRV_USBBK_MSG_CTRL_IN, msg->topic)) {
        ctrl_in_start(d, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_CTRL_OUT, msg->topic)) {
        ctrl_out_start(d, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, msg->topic)) {
        bulk_in_open(d, msg);
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_IN_STREAM_CLOSE, msg->topic)) {
        bulk_in_close(d, msg);
    } else {
        JSDRV_LOGW("unsupported topic %s", msg->topic);
        msg->value = jsdrv_union_i32(JSDRV_ERROR_PARAMETER_INVALID);
        msg_queue_push(d->ll_device.rsp_q, msg);
    }
    return true;
}

static void process_devices(struct backend_s * s) {
    struct jsdrv_list_s * item;
    struct dev_s * d;
    struct jsdrvp_msg_s * msg;
    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        msg = msg_queue_pop_immediate(d->ll_device.cmd_q);
        device_handle_msg(d, msg);
    }
}

static int32_t device_add(struct backend_s * s, libusb_device * usb_device, struct libusb_device_descriptor * descriptor) {
    struct dev_s * d;
    struct jsdrv_list_s * item;
    item = jsdrv_list_remove_head(&s->devices_free);
    if (!item) {
        JSDRV_LOGW("device_add but too many devices");
        return 1;
    }
    d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
    jsdrv_list_initialize(&d->item);

    const struct device_type_s * dt = device_types;
    while (dt->device_type) {
        if ((dt->vendor_id == descriptor->idVendor) && (dt->product_id == descriptor->idProduct)) {
            // device matches device_type
            d->mark = DEVICE_MARK_ADDED;
            d->device_type = dt;
            d->usb_device = usb_device;
            d->device_descriptor = *descriptor;
            int rc = libusb_get_serial_string_descriptor_ascii(d->usb_device, (uint8_t *) d->serial_number, sizeof(d->serial_number));
            if (rc < 0) {
                JSDRV_LOGW("Could not get serial number string");
                tfp_snprintf(d->serial_number, sizeof(d->serial_number), "unknown");
            } else {
                unsigned long slen = strlen(d->serial_number);
                while (slen && (d->serial_number[slen - 1] == '\n')) {
                    d->serial_number[--slen] = 0;
                }
            }
            tfp_snprintf(d->ll_device.prefix, sizeof(d->ll_device.prefix), "%c/%s/%s",
                         s->backend.prefix, d->device_type->model, d->serial_number);
            jsdrv_list_add_tail(&s->devices_active, &d->item);
            d->mode = DEVICE_MODE_CLOSED;
            device_announce(s, d, JSDRV_MSG_DEVICE_ADD);
            return 0;
        }
        ++dt;
    }
    jsdrv_list_add_tail(&s->devices_free, &d->item);
    return 1;
}

static void device_remove(struct backend_s * s, struct dev_s * d) {
    JSDRV_LOGI("device_remove(%s)", d->ll_device.prefix);
    jsdrv_list_remove(&d->item);
    device_close(d);
    if (d->usb_device) {
        libusb_unref_device(d->usb_device);
        d->usb_device = NULL;
    }
    d->mode = DEVICE_MODE_UNASSIGNED;
    device_announce(s, d, JSDRV_MSG_DEVICE_REMOVE);
    jsdrv_list_add_tail(&s->devices_free, &d->item);
}

static void handle_hotplug(struct backend_s * s) {
    struct libusb_device_descriptor descriptor;
    libusb_device ** device_list;
    struct dev_s * d;
    struct jsdrv_list_s * item;
    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        d->mark = DEVICE_MARK_NONE;
    }
    ssize_t n = libusb_get_device_list(s->ctx, &device_list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device * usbd = device_list[i];
        d = device_lookup_by_usb_device(s, usbd);
        if (NULL != d) {
            JSDRV_LOGI("Found device: %p %s", usbd, d->serial_number);
            d->mark = DEVICE_MARK_FOUND;
        } else if (libusb_get_device_descriptor(usbd, &descriptor)) {
            JSDRV_LOGW("could not get device descriptor for %p", d);
        } else if (0 == device_add(s, usbd, &descriptor)) {
            continue;  // success, keep device reference
        }
        libusb_unref_device(usbd);
    }
    libusb_free_device_list(device_list, 0);

    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        if (d->mark == DEVICE_MARK_NONE) {
            device_remove(s, d);
        }
    }
}

static bool handle_msg(struct backend_s * s, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
        JSDRV_LOGI("libusb backend finalize");
        s->do_exit = true;
        rv = false;
    } else {
        JSDRV_LOGE("backend_handle_msg unsupported %s", msg->topic);
    }
    jsdrvp_msg_free(s->context, msg);
    return rv;
}

static void backend_init_done(struct backend_s * s, int32_t status) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_value(s->context, JSDRV_MSG_INITIALIZE, &jsdrv_union_i32(status));
    msg->payload.str[0] = s->backend.prefix;
    jsdrvp_backend_send(s->context, msg);
}

static bool are_all_devices_idle(struct backend_s * s) {
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s *d = &s->devices[i];
        if (!jsdrv_list_is_empty(&d->transfers_pending)) {
            return false;
        }
    }
    return true;
}

static void handle_device_close(struct backend_s * s) {
    struct jsdrv_list_s * item;
    struct dev_s * d;
    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        if ((d->mode == DEVICE_MODE_CLOSING) && (jsdrv_list_is_empty(&d->transfers_pending))) {
            if (d->handle) {
                libusb_close(d->handle);
                d->handle = NULL;
            }
            d->mode = DEVICE_MODE_CLOSED;
        }
    }
}

static void device_close_all(struct backend_s * s) {
    struct timeval libusb_timeout_tv = {.tv_sec=1, .tv_usec=20000};
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s *d = &s->devices[i];
        if (d->handle) {
            device_close(d);
        }
    }
    while (!are_all_devices_idle(s)) {
        libusb_handle_events_timeout_completed(s->ctx, &libusb_timeout_tv, NULL);
        handle_device_close(s);
        // todo timeout?
    }
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s *d = &s->devices[i];
        if (NULL != d->handle) {
            JSDRV_LOGW("device_close_all did not close device");
            libusb_close(d->handle);
            d->handle = NULL;
        }
        if (NULL != d->usb_device) {
            libusb_unref_device(d->usb_device);
            d->usb_device = NULL;
        }
    }
}

void * backend_thread(void * arg) {
    struct pollfd fds[1024];
    nfds_t nfds;
    struct timeval libusb_timeout_tv = {.tv_sec=0, .tv_usec=0};
    JSDRV_LOGI("jsdrv_usb_backend_thread start");
    struct backend_s * s = (struct backend_s *) arg;
    int rc = libusb_init(&s->ctx);
    if (rc) {
        JSDRV_LOGE("libusb_init failed: %d", rc);
        backend_init_done(s, JSDRV_ERROR_IO);
        return NULL;
    }

    rc = libusb_hotplug_register_callback(
            s->ctx,
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            0,                              // flags
            LIBUSB_HOTPLUG_MATCH_ANY,       // vid
            LIBUSB_HOTPLUG_MATCH_ANY,       // pid
            LIBUSB_HOTPLUG_MATCH_ANY,       // device class
            on_hotplug,                     // callback
            s,                              // callback argument
            &s->hotplug_callback_handle     // allocated callback handle
    );
    if (LIBUSB_SUCCESS != rc) {
        JSDRV_LOGE("libusb_hotplug_register_callback returned %d", rc);
        goto exit;
    }

    handle_hotplug(s);  // perform an initial scan
    backend_init_done(s, 0);

    while (!s->do_exit) {
        nfds = 0;
        fds[nfds].fd = msg_queue_handle_get(s->backend.cmd_q);
        fds[nfds++].events = POLLIN;
        fds[nfds].fd = s->hotplug_event->fd_poll;
        fds[nfds++].events = s->hotplug_event->events;

        const struct libusb_pollfd ** libusb_fds = libusb_get_pollfds(s->ctx);
        for (int i = 0; libusb_fds[i]; ++i) {
            fds[nfds].fd = libusb_fds[i]->fd;
            fds[nfds++].events = libusb_fds[i]->events;
        }
        libusb_free_pollfds(libusb_fds);

        rc = poll(fds, nfds, 10);
        rc = libusb_handle_events_timeout_completed(s->ctx, &libusb_timeout_tv, NULL);
        //if (fds[0].revents) {
            while (handle_msg(s, msg_queue_pop_immediate(s->backend.cmd_q))) {
                ; //
            }
        //}
        process_devices(s);
        if (fds[1].revents) {
            handle_hotplug(s);
        }
        handle_device_close(s);
    }

exit:
    libusb_hotplug_deregister_callback(s->ctx, s->hotplug_callback_handle);
    device_close_all(s);
    libusb_exit(s->ctx);
    JSDRV_LOGI("jsdrv_usb_backend_thread exit");
    return NULL;
}

static void finalize(struct jsdrvbk_s * backend) {
    struct backend_s * s = (struct backend_s *) backend;
    char topic[2] = {backend->prefix, 0};
    s->do_exit = true;
    JSDRV_LOGI("backend finalize");
    if (s->thread_id) {
        jsdrvp_send_finalize_msg(s->context, s->backend.cmd_q, topic);
        int rv = pthread_join(s->thread_id, NULL);
        if (rv) {
            JSDRV_LOGW("pthread_join returned %d", rv);
        }
        s->thread_id = 0;
    }
    if (s->backend.cmd_q) {
        msg_queue_finalize(s->backend.cmd_q);
        s->backend.cmd_q = NULL;
    }
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s * d = &s->devices[i];
        if (d->ll_device.cmd_q) {
            msg_queue_finalize(d->ll_device.cmd_q);
            d->ll_device.cmd_q = NULL;
        }
        if (d->ll_device.rsp_q) {
            msg_queue_finalize(d->ll_device.rsp_q);
            d->ll_device.rsp_q = NULL;
        }
    }
    jsdrv_free(s);
}

int32_t jsdrv_usb_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend) {
    JSDRV_LOGI("jsdrv_usb_backend_factory");
    struct backend_s * s = jsdrv_alloc_clr(sizeof(struct backend_s));
    s->context = context;
    s->backend.prefix = 'u';
    s->backend.finalize = finalize;
    s->backend.cmd_q = msg_queue_init();
    jsdrv_list_initialize(&s->devices_active);
    jsdrv_list_initialize(&s->devices_free);
    for (uint32_t i = 0; i < DEVICES_MAX; ++i) {
        struct dev_s * d = &s->devices[i];
        d->backend = s;
        d->ll_device.cmd_q = msg_queue_init();
        d->ll_device.rsp_q = msg_queue_init();
        jsdrv_list_initialize(&d->transfers_pending);
        jsdrv_list_initialize(&d->transfers_free);
        jsdrv_list_initialize(&d->item);
        jsdrv_list_add_tail(&s->devices_free, &d->item);
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        JSDRV_LOGE("only support platforms with hotplug");
        return JSDRV_ERROR_UNAVAILABLE;
    }

    s->hotplug_event = jsdrv_os_event_alloc();

    int rc = pthread_create(&s->thread_id, NULL, backend_thread, s);
    if (rc) {
        JSDRV_LOGE("pthread_create failed: %d", rc);
        finalize(&s->backend);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    // todo set thread priority if possible

    *backend = &s->backend;
    return 0;
}

