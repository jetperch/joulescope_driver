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
 * @brief Thread-safe message queue.
 */

#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/event.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv/error_code.h"
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>



// ----------------------------------------------------------------------------
// Message queue implementation
// ----------------------------------------------------------------------------

struct msg_queue_s {
    jsdrv_os_event_t event;
    struct jsdrv_list_s items;
    pthread_mutex_t mutex;
};

struct msg_queue_s * msg_queue_init() {
    struct msg_queue_s * q = jsdrv_alloc_clr(sizeof(struct msg_queue_s));
    if (pthread_mutex_init(&q->mutex, NULL)) {
        jsdrv_free(q);
        return NULL;
    }
    q->event = jsdrv_os_event_alloc();
    if (NULL == q->event) {
        jsdrv_free(q);
        return NULL;
    }
    //JSDRV_LOGI("msg_queue_init %p %p", q, q->available_event);
    jsdrv_list_initialize(&q->items);
    return q;
}

void msg_queue_finalize(struct msg_queue_s * queue) {
    struct jsdrvp_msg_s * msg;
    if (queue) {
        pthread_mutex_lock(&queue->mutex);
        while (1) {
            // return items in the queue.
            struct jsdrv_list_s * item = jsdrv_list_remove_tail(&queue->items);
            if (item) {
                msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
            } else {
                break;
            }
        }
        pthread_mutex_unlock(&queue->mutex);
        pthread_mutex_destroy(&queue->mutex);
        jsdrv_os_event_free(queue->event);
        jsdrv_free(queue);
    }
}

bool msg_queue_is_empty(struct msg_queue_s* queue) {
    bool rv;
    pthread_mutex_lock(&queue->mutex);
    rv = jsdrv_list_is_empty(&queue->items);
    pthread_mutex_unlock(&queue->mutex);
    return rv;
}

void msg_queue_push(struct msg_queue_s * queue, struct jsdrvp_msg_s * msg) {
    JSDRV_DBC_NOT_NULL(msg);
    jsdrv_list_remove(&msg->item);  // remove from any existing list
    pthread_mutex_lock(&queue->mutex);
    jsdrv_list_add_tail(&queue->items, &msg->item);
    pthread_mutex_unlock(&queue->mutex);
    jsdrv_os_event_signal(queue->event);
}

struct jsdrvp_msg_s * msg_queue_pop_immediate(struct msg_queue_s* queue) {
    struct jsdrv_list_s * item;
    struct jsdrvp_msg_s * msg = NULL;
    pthread_mutex_lock(&queue->mutex);
    jsdrv_os_event_reset(queue->event);
    item = jsdrv_list_remove_head(&queue->items);
    if (item) {
        msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
    }
    pthread_mutex_unlock(&queue->mutex);
    return msg;
}

int32_t msg_queue_pop(struct msg_queue_s* queue, struct jsdrvp_msg_s ** msg, uint32_t timeout_ms) {
    JSDRV_DBC_NOT_NULL(msg);
    *msg = msg_queue_pop_immediate(queue);

    while (!*msg) {
        if (0 == timeout_ms) {
            return JSDRV_ERROR_TIMED_OUT;
        }
        struct pollfd fds = {
            .fd = queue->event->fd_poll,
            .events = queue->event->events,
            .revents = 0,
        };
        int rv = poll(&fds, 1, (int) timeout_ms);
        if (rv > 0) {
            *msg = msg_queue_pop_immediate(queue);
            if (*msg) {
                return JSDRV_SUCCESS;
            }
        } else if (0 == rv) {
            JSDRV_LOGE("msg_queue_pop timed out");
            return JSDRV_ERROR_TIMED_OUT;
        } else {
            JSDRV_LOGE("msg_queue_pop error %d", errno);
            return JSDRV_ERROR_IO;
        }
    }
    return JSDRV_SUCCESS;
}

msg_handle msg_queue_handle_get(struct msg_queue_s* queue) {
    return queue->event->fd_poll;
}
