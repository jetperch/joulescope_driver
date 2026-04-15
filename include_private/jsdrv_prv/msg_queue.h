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

#ifndef JSDRV_MSG_QUEUE_H_
#define JSDRV_MSG_QUEUE_H_

/**
 * @file
 *
 * @brief Thread-safe message queue.
 */

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

#if _WIN32
#include <windows.h>  // todo support libusb for linux & mac
#define msg_handle HANDLE
#else /* presume POSIX */
#define msg_handle int
#endif

JSDRV_CPP_GUARD_START

// opaque handle
struct msg_queue_s;

// forward declaration for "jsdrv/frontend.h"
struct jsdrvp_msg_s;
struct jsdrv_context_s;

struct msg_queue_s * msg_queue_init(void);

/**
 * @brief Finalize a message queue and free any residual messages.
 *
 * @param queue The queue to finalize.  Safe to pass NULL.
 * @param context The jsdrv context used to properly free residual
 *      messages (releases heap payloads, decrements tmap refs,
 *      returns messages to the free pool when possible).  Pass
 *      NULL only when no context is available (e.g. after
 *      jsdrv_finalize has already freed it, or for queues that
 *      do not carry jsdrvp_msg_s payloads).  With NULL, residual
 *      messages are released with jsdrv_free directly and their
 *      embedded heap payloads leak.
 */
void msg_queue_finalize(struct msg_queue_s * queue,
                        struct jsdrv_context_s * context);

bool msg_queue_is_empty(struct msg_queue_s* queue);

void msg_queue_push(struct msg_queue_s * queue, struct jsdrvp_msg_s * msg);

struct jsdrvp_msg_s * msg_queue_pop_immediate(struct msg_queue_s* queue);

int32_t msg_queue_pop(struct msg_queue_s* queue, struct jsdrvp_msg_s ** msg, uint32_t timeout_ms);

msg_handle msg_queue_handle_get(struct msg_queue_s* queue);

JSDRV_CPP_GUARD_END

#endif  /* JSDRV_MSG_QUEUE_H_ */
