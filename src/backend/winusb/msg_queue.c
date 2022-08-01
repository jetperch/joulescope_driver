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
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv/error_code.h"
#include <windows.h>


// ----------------------------------------------------------------------------
// Message queue implementation
// ----------------------------------------------------------------------------

struct msg_queue_s {
    HANDLE available_event;           // event
    struct jsdrv_list_s items;
    CRITICAL_SECTION critical_section;
};

struct msg_queue_s * msg_queue_init() {
    struct msg_queue_s * q = jsdrv_alloc_clr(sizeof(struct msg_queue_s));
    InitializeCriticalSection(&q->critical_section);
    q->available_event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            FALSE, // start unsignaled
            NULL   // no name
    );
    if (!q->available_event) {
        return NULL;
    }
    //JSDRV_LOGI("msg_queue alloc %p %p", q, q->available_event);
    jsdrv_list_initialize(&q->items);
    return q;
}

void msg_queue_finalize(struct msg_queue_s * queue) {
    struct jsdrvp_msg_s * msg;
    if (queue) {
        //JSDRV_LOGI("msg_queue free %p %p", queue, queue->available_event);
        EnterCriticalSection(&queue->critical_section);
        while (1) {
            // return items in the queue.
            struct jsdrv_list_s * item = jsdrv_list_remove_tail(&queue->items);
            if (item) {
                msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
                jsdrv_free(msg);
            } else {
                break;
            }
        }
        LeaveCriticalSection(&queue->critical_section);
        DeleteCriticalSection(&queue->critical_section);
        CloseHandle(queue->available_event);
        queue->available_event = 0;
        jsdrv_free(queue);
    }
}

bool msg_queue_is_empty(struct msg_queue_s* queue) {
    bool rv;
    EnterCriticalSection(&queue->critical_section);
    rv = jsdrv_list_is_empty(&queue->items);
    LeaveCriticalSection(&queue->critical_section);
    return rv;
}

void msg_queue_push(struct msg_queue_s * queue, struct jsdrvp_msg_s * msg) {
    JSDRV_DBC_NOT_NULL(msg);
    jsdrv_list_remove(&msg->item);  // remove from any existing list
    EnterCriticalSection(&queue->critical_section);
    jsdrv_list_add_tail(&queue->items, &msg->item);
    LeaveCriticalSection(&queue->critical_section);
    SetEvent(queue->available_event);
}

struct jsdrvp_msg_s * msg_queue_pop_immediate(struct msg_queue_s* queue) {
    struct jsdrv_list_s * item;
    struct jsdrvp_msg_s * msg = NULL;
    EnterCriticalSection(&queue->critical_section);
    item = jsdrv_list_remove_head(&queue->items);
    if (item) {
        msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
        if (jsdrv_list_is_empty(&queue->items)) {
            ResetEvent(queue->available_event);
        }
    } else {
        ResetEvent(queue->available_event);
    }
    LeaveCriticalSection(&queue->critical_section);
    return msg;
}

int32_t msg_queue_pop(struct msg_queue_s* queue, struct jsdrvp_msg_s ** msg, uint32_t timeout_ms) {
    JSDRV_DBC_NOT_NULL(msg);
    *msg = msg_queue_pop_immediate(queue);

    while (!*msg) {
        if (0 == timeout_ms) {
            return JSDRV_ERROR_TIMED_OUT;
        }
        DWORD rv = WaitForSingleObject(queue->available_event, timeout_ms);
        if (rv == WAIT_OBJECT_0) {
            *msg = msg_queue_pop_immediate(queue);
            if (*msg) {
                return JSDRV_SUCCESS;
            }
        } else if (rv == WAIT_TIMEOUT) {
            return JSDRV_ERROR_TIMED_OUT;
        } else {
            JSDRV_LOGE("msg_queue_pop WaitForSingleObject error %d", rv);
            return JSDRV_ERROR_TIMED_OUT;
        }
    }
    return JSDRV_SUCCESS;
}

msg_handle msg_queue_handle_get(struct msg_queue_s* queue) {
    return queue->available_event;
}
