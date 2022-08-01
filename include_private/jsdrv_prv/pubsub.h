/*
 * Copyright 2020-2021 Jetperch LLC
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
 * @brief Joulescope driver publish-subscribe.
 *
 * Based upon fitterbap/pubsub.h.
 */

#ifndef JSDRV_PUBSUB_H__
#define JSDRV_PUBSUB_H__

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/union.h"
#include "jsdrv.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_pubsub Publish-Subscribe
 *
 * @brief A simple, opinionated, distributed Publish-Subscribe architecture.
 *
 * This publish/subscribe implementation is based upon the fitterbap/pubsub.h.
 * However, this version is customized to use jsdrvp_msg_s for publish,
 * subscribe, unsubscribe, and topic data storage.
 *
 * This implementation is not thread-safe, and it is designed for
 * single-threaded operation only.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/// The topic prefix for local topics (no distributed pubsub).
#define JSDRV_PUBSUB_COMMAND_PREFIX   '_'
#define JSDRV_PUBSUB_SUBSCRIBE        "_/!sub"
#define JSDRV_PUBSUB_UNSUBSCRIBE      "_/!unsub"
#define JSDRV_PUBSUB_UNSUBSCRIBE_ALL  "_/!unsub+"
#define JSDRV_PUBSUB_QUERY            "_/!query"

/// The opaque PubSub instance.
struct jsdrv_pubsub_s;

// Forward declarations from "jsdrv.h"
struct jsdrvp_msg_s;
struct jsdrv_context_s;

/**
 * @brief Function called on topic updates.
 *
 * @param user_data The arbitrary user data.
 * @param msg The message for this update.
 * @return 0 or error code.  Only the topic "owner" should response
 *      with an error code.
 *
 * If this callback responds with an error code, then the pubsub
 * instance will publish a topic# error.
 */
typedef uint8_t (*jsdrv_pubsub_subscribe_fn)(void * user_data, struct jsdrvp_msg_s * msg);

/**
 * @brief The subscriber structure.
 *
 * This pubsub instance supports two types of subscribers.
 * External subscribers get topic + value.  Internal subscribers
 * get the raw message.  This structure unifies the
 * subscriber specification.
 */
struct jsdrv_pubsub_subscriber_s {
    union {
        jsdrv_subscribe_fn external_fn;
        jsdrv_pubsub_subscribe_fn internal_fn;
        void * void_fn;
    };
    void * user_data;
    uint8_t is_internal;
    uint8_t flags;          ///< jsdrv_subscribe_flag_e
};

struct jsdrv_pubsub_subscriber_internal_s {
    jsdrv_pubsub_subscribe_fn fn;
    void * user_data;
};

/**
 * @brief Create and initialize a new PubSub instance.
 *
 * @return The new PubSub instance.
 */
struct jsdrv_pubsub_s * jsdrv_pubsub_initialize(struct jsdrv_context_s * context);

/**
 * @brief Finalize the instance and free resources.
 *
 * @param self The PubSub instance.
 */
void jsdrv_pubsub_finalize(struct jsdrv_pubsub_s * self);

/**
 * @brief Publish to a topic.
 *
 * @param self The PubSub instance.
 * @param msg The message to publish.  The caller transfers ownership
 *      to this function and must never access this msg instance again.
 * @return 0 or error code.
 * @see jsdrv_pubsub_subscribe_fn()
 *
 * If the topic does not already exist, this function will
 * automatically create it.
 */
int32_t jsdrv_pubsub_publish(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg);

/**
 * @brief Process all outstanding topic updates.
 *
 * @param self The PubSub instance to process.
 */
void jsdrv_pubsub_process(struct jsdrv_pubsub_s * self);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_PUBSUB_H__ */
