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
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv/error_code.h"
#include "jsdrv/cstr.h"
#include "tinyprintf.h"
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>


#define DEVICES_MAX   (256U)

enum device_mark_e {
    DEVICE_MARK_NONE = 0,
    DEVICE_MARK_FOUND = 1,
    DEVICE_MARK_ADDED = 2,
    DEVICE_MARK_REMOVED = 3,
};

struct dev_s {
    struct jsdrvp_ll_device_s ll_device;
    libusb_device * usb_device;
    libusb_device_handle * handle;
    struct jsdrv_context_s * context;
    const struct device_type_s * device_type;
    struct libusb_device_descriptor device_descriptor;
    char serial_number[JSDRV_TOPIC_LENGTH_MAX];
    uint8_t mark;

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

static int32_t device_prefix_set(struct backend_s * s, struct dev_s * d) {
    int rc = libusb_get_string_descriptor_ascii(d->handle, d->device_descriptor.iSerialNumber, 
                                                (uint8_t *) d->serial_number, sizeof(d->serial_number));
    if (rc <= 0) {
        JSDRV_LOGW("Could not retrieve serial number");
        return 1;
    }
    tfp_snprintf(d->ll_device.prefix, sizeof(d->ll_device.prefix), "%c/%s/%s",
                 s->backend.prefix, d->device_type->model, d->serial_number);
    return 0;
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
            // ideally, we could get serial number without open
            // See https://github.com/libusb/libusb/issues/866
            // For now, perform greedy libusb_open on matching devices
            int rc = libusb_open(d->usb_device, &d->handle);
            if (rc != 0) {
                if (rc == LIBUSB_ERROR_ACCESS) {
                    JSDRV_LOGE("libusb_open - insufficient permissions");
                } else if (rc) {
                    JSDRV_LOGE("libusb_open failed %d", rc);
                }
                break;
            }
            if (device_prefix_set(s, d)) {
                JSDRV_LOGE("could not set device prefix");
                break;
            }
            jsdrv_list_add_tail(&s->devices_active, &d->item);
            device_announce(s, d, JSDRV_MSG_DEVICE_ADD);
            return 0;
        }
        ++dt;
    }
    jsdrv_list_add_tail(&s->devices_free, &d->item);
    return 1;
}

static void device_remove(struct backend_s * s, struct dev_s * d) {
    jsdrv_list_remove(&d->item);
    if (d->handle) {
        libusb_close(d->handle);
        d->handle = NULL;
        libusb_unref_device(d->usb_device);
    }
    device_announce(s, d, JSDRV_MSG_DEVICE_REMOVE);
    jsdrv_list_add_tail(&s->devices_free, &d->item);
}

static void handle_hotplug(struct backend_s * s) {
    struct libusb_device_descriptor descriptor;
    libusb_device ** device_list;
    ssize_t n = libusb_get_device_list(s->ctx, &device_list);
    struct dev_s * d;

    struct jsdrv_list_s * item;
    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        d->mark = DEVICE_MARK_NONE;
    }

    for (ssize_t i = 0; i < n; ++i) {
        libusb_device * usbd = device_list[i];
        d = device_lookup_by_usb_device(s, usbd);
        if (NULL != d) {
            JSDRV_LOGI("Found device: %p %s", usbd, d->serial_number);
            d->mark = DEVICE_MARK_FOUND;
            libusb_unref_device(usbd);
            continue;
        }
        if (libusb_get_device_descriptor(usbd, &descriptor)) {
            JSDRV_LOGW("could not get device descriptor for %p", d);
            continue;
        }
        if (!device_add(s, usbd, &descriptor)) {
            libusb_unref_device(usbd);
        }
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

static void process_devices(struct backend_s * s) {
    struct jsdrv_list_s * item;
    struct dev_s * d;
    struct jsdrvp_msg_s * msg;
    jsdrv_list_foreach(&s->devices_active, item) {
        d = JSDRV_CONTAINER_OF(item, struct dev_s, item);
        msg = msg_queue_pop_immediate(d->ll_device.cmd_q);
        if (!msg) {
            continue;
        }

    }
}

void * backend_thread(void * arg) {
    struct pollfd fds[1024];
    nfds_t nfds;
    struct timeval libusb_timeout_tv;

    JSDRV_LOGI("jsdrv_usb_backend_thread start");
    memset(&libusb_timeout_tv, 0, sizeof(libusb_timeout_tv));

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

        rc = poll(fds, nfds, 100);
        rc = libusb_handle_events_timeout_completed(s->ctx, &libusb_timeout_tv, NULL);
        // todo service device message queues
        // todo service main backend message queue
        if (fds[0].revents) {
            while (handle_msg(s, msg_queue_pop_immediate(s->backend.cmd_q))) {
                ; //
            }
        }
        process_devices(s);
        if (fds[1].revents) {
            handle_hotplug(s);
        }
    }

exit:
    libusb_hotplug_deregister_callback(s->ctx, s->hotplug_callback_handle);
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
        if (d->handle) {
            libusb_close(d->handle);
            d->handle = NULL;
            libusb_unref_device(d->usb_device);
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
        d->context = context;
        d->ll_device.cmd_q = msg_queue_init();
        d->ll_device.rsp_q = msg_queue_init();
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

