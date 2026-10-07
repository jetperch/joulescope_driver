/*
* SPDX-FileCopyrightText: Copyright 2014-2022 Jetperch LLC
* SPDX-License-Identifier: Apache-2.0
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

#include "jsdrv.h"
#include "jsdrv_prv/event.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/usb_spec.h"
#include "jsdrv_prv/pubsub.h"

/**
 * @file
 *
 * @brief Joulescope host driver frontend API.
 */

#ifndef JSDRV_PRV_FRONTEND_H_
#define JSDRV_PRV_FRONTEND_H_

JSDRV_CPP_GUARD_START

// also see jsdrv_payload_type_e
enum jsdrvp_payload_type_e {     // for jsdrv_union_s.app
    JSDRV_PAYLOAD_TYPE_SUB  = 128,          // jsdrv_payload_subscribe_s
    JSDRV_PAYLOAD_TYPE_DEVICE,              // jsdrvp_device_s
    JSDRV_PAYLOAD_TYPE_USB_CTRL,
    JSDRV_PAYLOAD_TYPE_USB_BULK,
};

// enum jsdrvp_msg_type_e
#define JSDRV_MSG_TYPE_NORMAL   0x55aa1234U
#define JSDRV_MSG_TYPE_DATA     0xaa55F00FU
// Sentinel: final message pushed by the LL thread into rsp_q as it exits.
// The UL thread treats receipt as a causal barrier — no more LL pushes will
// ever arrive after this message — and safely exits rsp_q consumption.
#define JSDRV_MSG_TYPE_LL_TERMINATED 0x3c5aa53cU

/**
 * @brief The topic prefix for a device's open gather sentinel.
 *
 * A DEFAULTS open subscribes to the host's retained device values, then
 * publishes a sentinel to its own "mbg/..." topic.  The frontend delivers
 * in FIFO order, so the sentinel marks the end of the retained values.
 * The sentinel must not use the device prefix, which is suppressed as an
 * echo.  The frontend routes backend topics that start with 'm' without
 * a device lookup, which keeps "mbg/" free of lookup warnings.  See
 * doc/plans/open_gather_completion.md to replace the sentinel.
 */
#define JSDRVP_GATHER_TOPIC_PREFIX "mbg/"

/**
 * @brief The subscribe completion message topic.
 *
 * A JSDRV_PUBSUB_SUBSCRIBE_DONE subscribe delivers this message to the
 * subscriber after its retained values.  The value is the subscribed
 * topic string.  See jsdrvp_device_subscribe_done().
 */
#define JSDRVP_MSG_SUBSCRIBE_DONE "@/!subdn"

struct jsdrvp_payload_subscribe_s {  // also for unsubscribe
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_pubsub_subscriber_s subscriber;
};

struct jsdrvp_payload_query_s {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    struct jsdrv_union_s * value;  // for the return, buffer for str, json, bin
};

// lower-level (backend) device driver API
struct jsdrvp_ll_device_s {
    char prefix[JSDRV_TOPIC_LENGTH_MAX];
    struct msg_queue_s * cmd_q;
    struct msg_queue_s * rsp_q;
};

struct jsdrvp_ul_device_s {
    struct msg_queue_s * cmd_q;
    void (*join)(struct jsdrvp_ul_device_s *);
};

// forward declaration for MiniBitty devices
struct jsdrvp_mb_drv_s;

// create and bind to ll
int32_t jsdrvp_ul_js110_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll);
int32_t jsdrvp_ul_js220_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll);
int32_t jsdrvp_ul_mb_device_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context,
                                        struct jsdrvp_ll_device_s * ll, struct jsdrvp_mb_drv_s * drv);
int32_t jsdrvp_mb_device_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll);
int32_t jsdrvp_js320_device_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll);


struct jsdrvp_msg_extra_frontend_s {
    struct jsdrv_pubsub_subscriber_s subscriber;    // allow for deduplication
};

struct jsdrvp_msg_extra_backend_usb_control_s {
    usb_setup_t setup;
    int32_t status;
};

struct jsdrvp_msg_extra_backend_usb_stream_s {
    uint8_t endpoint;
};

union jsdrvp_msg_extra_s {
    struct jsdrvp_msg_extra_frontend_s frontend;
    struct jsdrvp_msg_extra_backend_usb_control_s bkusb_ctrl;
    struct jsdrvp_msg_extra_backend_usb_stream_s bkusb_stream;
};

union jsdrvp_payload_u {
    uint8_t bin[JSDRV_PAYLOAD_LENGTH_MAX];      // unstructured binary
    char str[JSDRV_PAYLOAD_LENGTH_MAX];         // string or json string
    struct jsdrvp_payload_subscribe_s sub;
    struct jsdrvp_payload_query_s query;
    struct jsdrvp_ll_device_s device;           // for @/add from backend
};

struct jsdrvp_api_timeout_s {
    struct jsdrv_list_s item;                   // for putting into list
    char topic[JSDRV_TOPIC_LENGTH_MAX];  // to match
    int64_t timeout;                            // timeout time as fbp_time_utc()
    jsdrv_os_event_t ev;                        // The event to signal upon completion or timeout
    volatile int32_t return_code;               // The return code for the operation.
};

struct jsdrvp_msg_s {
    struct jsdrv_list_s item;                   // queue support (internal use) - MUST BE FIRST
    uint32_t inner_msg_type;                    // jsdrvp_msg_type_e (internal use, do not edit)
    uint32_t source;                            // 0=backend/frontend/internal, 1=api
    uint32_t u32_a;                             // temporary storage variable, available for message processing
    uint32_t u32_b;                             // temporary storage variable, available for message processing
    char topic[JSDRV_TOPIC_LENGTH_MAX];    // the topic name or device identifier
    struct jsdrv_union_s value;                 // the value as a union type
    union jsdrvp_msg_extra_s extra;
    struct jsdrvp_api_timeout_s * timeout;
    union jsdrvp_payload_u payload;             // must be last
    // do not place any fields after payload!
};

/**
 * @brief Define parameter metadata.
 *
 * Declare tables as NULL-terminated "const struct jsdrvp_param_s name[]"
 * in a dedicated *_params.c file, using a topic string literal and a
 * meta JSON string literal (adjacent literals and comments allowed).
 * Tools parse these files to document topics offline, so keep to this
 * format.  Topics may contain "{buf}" and "{sig}" placeholders for
 * instance ids expanded at runtime.
 */
struct jsdrvp_param_s {
    const char * topic;
    const char * meta;
};

/**
 * @brief Allocate a new message.
 *
 * @param context The Joulescope driver context.
 * @return The message.
 * @throw assert on out of memory
 */
struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context);

/**
 * @brief Allocate a large binary data message.
 *
 * @param context The Joulescope driver context.
 * @param topic The topic for the message.
 * @return The message.  Using pointer msg->value.value.bin to fill data
 *      and set msg->value.size.  The maximum payload size is
 *      JSDRVP_MSG_DATA_PAYLOAD_SIZE_MAX.
 * @throw assert on out of memory
 */
struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic);

/**
 * @brief Allocation a new message and populate with same contents as another message.
 *
 * @param context The Joulescope driver context.
 * @param msg The message to clone.
 * @return The new message.
 * @throw assert on out of memory
 */
struct jsdrvp_msg_s * jsdrvp_msg_clone(struct jsdrv_context_s * context, const struct jsdrvp_msg_s * msg_src);

/**
 * @brief Allocate a new message with a string value.
 *
 * @param context The Joulescope driver context.
 * @param topic The topic name.
 * @param value The union value.
 * @return The message.
 * @throw assert on out of memory
 */
struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context, const char * topic, const struct jsdrv_union_s * value);

/**
 * @brief Free a message to the free list.
 *
 * @param msg The message to free.
 */
void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg);

/**
 * @brief Send an async message from the backend to the frontend.
 *
 * @param context The Joulescope driver context.
 * @param msg The message to send.
 *
 * Note: implemented by the front-end.
 */
void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg);

/**
 * @brief Send a finalize message to a queue.
 *
 * @param context The Joulescope driver context.
 * @param q The target queue for the finalize message.
 * @param topic The string value for the finalize message.
 */
void jsdrvp_send_finalize_msg(struct jsdrv_context_s * context, struct msg_queue_s * q, const char * topic);

/**
 * @brief Subscribe a device to an additional topics.
 *
 * @param context The Joulescope driver context.
 * @param dev_topic The device prefix topic.
 * @param topic The topic for the subscription.
 * @param flags The jsdrv_subscribe_flag_e subscription flags bitmap.
 */
void jsdrvp_device_subscribe(struct jsdrv_context_s * context, const char * dev_topic,
                             const char * topic, uint8_t flags);

/**
 * @brief Subscribe a device, then deliver JSDRVP_MSG_SUBSCRIBE_DONE.
 *
 * @param context The Joulescope driver context.
 * @param dev_topic The device prefix topic.
 * @param topic The topic for the subscription.
 * @param flags The jsdrv_subscribe_flag_e subscription flags bitmap.
 *
 * With JSDRV_SFLAG_RETAIN, the device receives every matching retained
 * value, then the completion message, in its command queue.  Only this
 * device receives the completion.
 */
void jsdrvp_device_subscribe_done(struct jsdrv_context_s * context, const char * dev_topic,
                                  const char * topic, uint8_t flags);

/**
 * @brief Unsubscribe a device from an additional topics.
 *
 * @param context The Joulescope driver context.
 * @param dev_topic The device prefix topic.
 * @param topic The topic for the unsubscription.
 * @param flags The jsdrv_subscribe_flag_e subscription flags bitmap.
 */
void jsdrvp_device_unsubscribe(struct jsdrv_context_s * context, const char * dev_topic,
                               const char * topic, uint8_t flags);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_FRONTEND_H_ */
