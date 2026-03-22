/*
 * Copyright 2025 Jetperch LLC
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
 * @brief MiniBitty device-specific upper driver interface.
 *
 * The mb_device driver handles the generic MiniBitty protocol (framing,
 * link management, state machine, pubsub routing). Device-specific drivers
 * implement this interface to handle product-specific functionality such
 * as streaming data interpretation and custom commands.
 *
 * Callbacks execute synchronously in mb_device's driver thread.
 */

#ifndef JSDRV_PRV_MB_DRV_H_
#define JSDRV_PRV_MB_DRV_H_

#include "jsdrv/cmacro_inc.h"
#include "mb/comm/frame.h"
#include <stdbool.h>
#include <stdint.h>

JSDRV_CPP_GUARD_START

// Forward declarations
struct jsdrvp_mb_dev_s;
struct jsdrvp_msg_s;
struct jsdrv_context_s;
struct jsdrv_union_s;
struct mb_link_identity_s;

/**
 * @brief Device-specific upper driver interface.
 *
 * Implement this vtable to provide device-specific behavior on top
 * of the generic MiniBitty protocol driver.  All callbacks are optional
 * (NULL is allowed).
 */
struct jsdrvp_mb_drv_s {

    /**
     * @brief Called when the device link is established (ST_OPEN entered).
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     * @param identity The device's reported identity information.
     */
    void (*on_open)(struct jsdrvp_mb_drv_s * drv,
                    struct jsdrvp_mb_dev_s * dev,
                    const struct mb_link_identity_s * identity);

    /**
     * @brief Called when the device is disconnecting.
     *
     * Called at the start of the disconnect sequence (before pubsub flush
     * and link disconnect).  The upper driver may still send messages via
     * the dev handle during this callback.
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle.
     */
    void (*on_close)(struct jsdrvp_mb_drv_s * drv,
                     struct jsdrvp_mb_dev_s * dev);

    /**
     * @brief Called for MB_FRAME_ST_APP data frames.
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     * @param metadata The 12-bit frame metadata.
     * @param data Pointer to the payload data (u32 words).
     * @param length The payload length in u32 words.
     */
    void (*handle_app)(struct jsdrvp_mb_drv_s * drv,
                       struct jsdrvp_mb_dev_s * dev,
                       uint16_t metadata,
                       const uint32_t * data,
                       uint8_t length);

    /**
     * @brief Called for commands from the API before mb_device's default handling.
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     * @param subtopic The topic after prefix stripping (e.g., "h/ctrl/range").
     * @param value The command value.
     * @return true if handled (mb_device will not process further),
     *         false to fall through to mb_device defaults.
     */
    bool (*handle_cmd)(struct jsdrvp_mb_drv_s * drv,
                       struct jsdrvp_mb_dev_s * dev,
                       const char * subtopic,
                       const struct jsdrv_union_s * value);

    /**
     * @brief Called for incoming device publishes before forwarding to frontend.
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     * @param subtopic The device-relative topic (e.g., "c/jtag/!done").
     * @param value The published value.
     * @return true if consumed (will not be forwarded to frontend),
     *         false to forward normally.
     */
    bool (*handle_publish)(struct jsdrvp_mb_drv_s * drv,
                           struct jsdrvp_mb_dev_s * dev,
                           const char * subtopic,
                           const struct jsdrv_union_s * value);

    /**
     * @brief Called when the upper driver timer expires.
     *
     * Set the timer with jsdrvp_mb_dev_set_timeout().
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     */
    void (*on_timeout)(struct jsdrvp_mb_drv_s * drv,
                       struct jsdrvp_mb_dev_s * dev);

    /**
     * @brief Called to publish factory default values for all configurable topics.
     *
     * Called when the device is opened with JSDRV_DEVICE_OPEN_MODE_DEFAULTS.
     * The upper driver should publish default values for all parameters
     * to the physical device via jsdrvp_mb_dev_publish_to_device().
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle for calling service functions.
     */
    void (*publish_defaults)(struct jsdrvp_mb_drv_s * drv,
                             struct jsdrvp_mb_dev_s * dev);

    /**
     * @brief Called to destroy the upper driver instance.
     *
     * @param drv This driver instance.  Free all resources including drv itself.
     */
    void (*finalize)(struct jsdrvp_mb_drv_s * drv);
};


// --- Services provided by mb_device to upper drivers ---

/**
 * @brief Send a value to the frontend pubsub system.
 *
 * @param dev The mb_device handle.
 * @param subtopic The subtopic to append to the device prefix.
 * @param value The value to publish.
 */
void jsdrvp_mb_dev_send_to_frontend(struct jsdrvp_mb_dev_s * dev,
                                     const char * subtopic,
                                     const struct jsdrv_union_s * value);

/**
 * @brief Publish a value to the physical device via stdmsg.
 *
 * @param dev The mb_device handle.
 * @param topic The topic string for the device pubsub.
 * @param value The value to publish.
 */
void jsdrvp_mb_dev_publish_to_device(struct jsdrvp_mb_dev_s * dev,
                                      const char * topic,
                                      const struct jsdrv_union_s * value);

/**
 * @brief Send raw data to device with specified service type and metadata.
 *
 * @param dev The mb_device handle.
 * @param service_type The MB frame service type.
 * @param metadata The 12-bit metadata value.
 * @param data The payload data (u32 words).
 * @param length_u32 The payload length in u32 words.
 */
void jsdrvp_mb_dev_send_to_device(struct jsdrvp_mb_dev_s * dev,
                                   enum mb_frame_service_type_e service_type,
                                   uint16_t metadata,
                                   const uint32_t * data,
                                   uint32_t length_u32);

/**
 * @brief Get the jsdrv context for message allocation.
 *
 * @param dev The mb_device handle.
 * @return The jsdrv context.
 */
struct jsdrv_context_s * jsdrvp_mb_dev_context(struct jsdrvp_mb_dev_s * dev);

/**
 * @brief Get the device prefix string.
 *
 * @param dev The mb_device handle.
 * @return The device prefix (e.g., "a/js320/12345").
 */
const char * jsdrvp_mb_dev_prefix(struct jsdrvp_mb_dev_s * dev);

/**
 * @brief Send a pre-built message to the frontend.
 *
 * @param dev The mb_device handle.
 * @param msg The message to send.  Ownership transfers to the frontend.
 */
void jsdrvp_mb_dev_backend_send(struct jsdrvp_mb_dev_s * dev,
                                 struct jsdrvp_msg_s * msg);

/**
 * @brief Set the upper driver timeout.
 *
 * @param dev The mb_device handle.
 * @param timeout_utc The absolute timeout time as jsdrv_time_utc(),
 *        or 0 to cancel any pending timeout.
 *
 * When the timeout expires, drv->on_timeout is called from the driver thread.
 * Only one timeout may be active at a time; calling again replaces the previous.
 */
void jsdrvp_mb_dev_set_timeout(struct jsdrvp_mb_dev_s * dev, int64_t timeout_utc);

/**
 * @brief Send a return code to the frontend pubsub.
 *
 * @param dev The mb_device handle.
 * @param subtopic The subtopic (without device prefix) to send the return code for.
 * @param rc The return code (0=success, else error code).
 *
 * Appends the '#' suffix and sends to the frontend pubsub.
 */
void jsdrvp_mb_dev_send_return_code(struct jsdrvp_mb_dev_s * dev,
                                     const char * subtopic,
                                     int32_t rc);

/**
 * @brief Get the device open mode.
 *
 * @param dev The mb_device handle.
 * @return The open mode (jsdrv_device_open_mode_e).
 */
int32_t jsdrvp_mb_dev_open_mode(struct jsdrvp_mb_dev_s * dev);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_MB_DRV_H_ */
