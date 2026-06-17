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
#include "jsdrv/time.h"
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
     * @brief Defer OPEN# to sync child pubsub instances.
     *
     * Called once after the core (link-identity) instance state sync
     * completes, BEFORE mb_device signals OPEN#.  Return true to DEFER
     * the open: the device is not declared open yet, and the driver must
     * eventually complete it by either:
     *   - syncing a child instance via jsdrvp_mb_dev_instance_state_sync()
     *     with emit_open=true (that sync signals OPEN# when it finishes), or
     *   - calling jsdrvp_mb_dev_open_complete() directly (e.g. a child
     *     readiness timeout -- open must complete, never fail).
     * Return false (or NULL hook) to signal OPEN# immediately.
     *
     * Typical use (JS320): arm a sensor-ready timeout here, then sync the
     * sensor 's' instance once c/comm/sensor/state reports the link up.
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle.
     * @return true to defer OPEN# (driver completes it), false to open now.
     */
    bool (*open_children)(struct jsdrvp_mb_drv_s * drv,
                          struct jsdrvp_mb_dev_s * dev);

    /**
     * @brief Called when a child instance sync (emit_open=false) completes.
     *
     * Lets the driver chain additional work after a deferred-open child
     * sync -- e.g. after the sensor 's' device instance, restore the
     * host-side 'h' instance via jsdrvp_mb_dev_host_replay() and then
     * complete the open via jsdrvp_mb_dev_open_complete().
     *
     * @param drv This driver instance.
     * @param dev The mb_device handle.
     * @param prefix The instance prefix char that just finished syncing.
     */
    void (*on_instance_synced)(struct jsdrvp_mb_drv_s * drv,
                               struct jsdrvp_mb_dev_s * dev,
                               char prefix);

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

/**
 * @brief Get the cached timesync map for the given pubsub-instance prefix.
 *
 * mb_device caches one map per top-level pubsub-instance prefix char
 * (e.g. 's' for sensor-side, 'c' for ctrl-side).  Each incoming
 * `<prefix>/ts/!map` publish updates its slot.  Device-specific drivers
 * choose which slot they trust for sample timestamping.
 *
 * @param dev The mb_device handle.
 * @param prefix The first character of the pubsub-instance topic.
 * @return Pointer to the device's cached map for this prefix, or NULL
 *         if no !map has arrived for this prefix in the current open
 *         session.  The returned pointer stays valid for the session;
 *         contents are updated in place by subsequent !map publishes.
 */
const struct jsdrv_time_map_s * jsdrvp_mb_dev_time_map(
        struct jsdrvp_mb_dev_s * dev, char prefix);

/**
 * @brief Start the device state_fetch protocol exchange.
 *
 * Called automatically during open unless drv->open_ready returned
 * false, in which case the driver is expected to invoke this
 * explicitly once it becomes ready.  Safe to call at most once per
 * open session; subsequent calls before the fetch completes are
 * ignored.
 *
 * @param dev The mb_device handle.
 */
void jsdrvp_mb_dev_state_fetch_start(struct jsdrvp_mb_dev_s * dev);

/**
 * @brief Best-effort, non-blocking state sync for a child pubsub instance.
 *
 * mb_device manages only the link-identity (USB) instance during open.
 * A device-specific driver calls this to sync an additional pubsub
 * instance (e.g. the JS320 sensor 's') once that instance is alive,
 * AFTER the core open has completed.  It runs the same open-mode
 * sequence (DEFAULTS/RESUME) against the given prefix but never emits
 * OPEN# and never fails the open: errors are logged and skipped.
 *
 * Typically invoked from handle_publish when a readiness signal proves
 * the child instance is running, with on_timeout as a fallback.
 *
 * @param dev The mb_device handle.
 * @param prefix The child instance's top-level pubsub prefix char.
 * @param emit_open If true, signal OPEN# when this sync completes; use
 *        this to finish an open deferred by drv->open_children.  If
 *        false, the sync is purely best-effort (no OPEN#).
 * @return true if the sync started; false if a sequence is already
 *         running (caller should retry later) or the prefix is invalid.
 */
bool jsdrvp_mb_dev_instance_state_sync(struct jsdrvp_mb_dev_s * dev,
                                       char prefix, bool emit_open);

/**
 * @brief Signal OPEN# for an open deferred by drv->open_children.
 *
 * Use when completing a deferred open WITHOUT a child sync that carries
 * emit_open=true -- e.g. a child-readiness timeout where the child is
 * skipped but the open must still complete (it never fails on a child).
 *
 * @param dev The mb_device handle.
 */
void jsdrvp_mb_dev_open_complete(struct jsdrvp_mb_dev_s * dev);

/**
 * @brief Restore the host's retained values under a prefix to handle_cmd.
 *
 * Subscribes RETAIN to {device}/{prefix} and immediately unsubscribes,
 * re-delivering the host pubsub's retained values to the device-specific
 * driver's handle_cmd.  Used for the host-side 'h' instance (topics such
 * as h/fp, h/fs, h/i_scale, h/v_scale that the driver owns in handle_cmd
 * rather than a device pubsub instance), to restore the driver's internal
 * state from the host cache on open.
 *
 * @param dev The mb_device handle.
 * @param prefix The host-side instance prefix char (e.g. 'h').
 */
void jsdrvp_mb_dev_host_replay(struct jsdrvp_mb_dev_s * dev, char prefix);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_PRV_MB_DRV_H_ */
