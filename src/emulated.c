/*
 * Copyright 2022 Jetperch LLC
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
#include "jsdrv_prv/cdef.h"
#include "jsdrv/log.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/assert.h"
#include "js220_api.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/windows.h"

struct device_s {
    struct jsdrv_list_s item;
};

#define DEVICES_MAX                     (256U)

struct backend_s {
    struct jsdrvbk_s backend;
    struct jsdrv_context_s * context;
    bool do_exit;
    HANDLE thread;
    DWORD thread_id;
};

static bool backend_handle_msg(struct backend_s * s, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
        JSDRV_LOGI("winusb backend finalize");
        s->do_exit = true;
        rv = false;
    } else {
        // todo add device
        // todo remove device
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
    JSDRV_LOGI("emulated backend_thread started");

    HANDLE handles[1];
    handles[0] = msg_queue_handle_get(s->backend.cmd_q);
    backend_ready(s);

    while (!s->do_exit) {
        WaitForMultipleObjects(1, handles, false, 5000);
        if (WAIT_OBJECT_0 == WaitForSingleObject(handles[0], 0)) {
            ResetEvent(handles[0]);
            while (1) {
                struct jsdrvp_msg_s * msg = msg_queue_pop_immediate(s->backend.cmd_q);
                if (!backend_handle_msg(s, msg)) {
                    break;
                }
            }

        }
    }

    JSDRV_LOGI("emulated backend_thread done");
    return 0;
}

static void backend_finalize(struct jsdrvbk_s * backend) {
    if (backend) {
        struct backend_s * s = (struct backend_s *) backend;
        JSDRV_LOGI("finalize emulated backend");
        if (s->thread) {
            jsdrvp_send_finalize_msg(s->context, s->backend.cmd_q, JSDRV_MSG_FINALIZE);
            // and wait for thread to exit.
            if (WAIT_OBJECT_0 != WaitForSingleObject(s->thread, 1000)) {
                JSDRV_LOGE("emulated thread not closed cleanly.");
            }
            CloseHandle(s->thread);
            s->thread = NULL;
        }
        if (s->backend.cmd_q) {
            msg_queue_finalize(s->backend.cmd_q);
            s->backend.cmd_q = NULL;
        }
        jsdrv_free(s);
    }
}

int32_t jsdrv_emulation_backend_factory(struct jsdrv_context_s * context, struct jsdrvbk_s ** backend) {
    struct backend_s * self = jsdrv_alloc_clr(sizeof(struct backend_s));
    self->context = context;
    self->backend.prefix = 'z';
    self->backend.finalize = backend_finalize;
    self->backend.cmd_q = msg_queue_init();

    self->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            backend_thread,         // thread function name
            self,                   // argument to thread function
            0,                      // use default creation flags
            &self->thread_id);      // returns the thread identifier
    if (self->thread == NULL) {
        JSDRV_LOGE("CreateThread failed");
        backend_finalize(&self->backend);
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (!SetThreadPriority(self->thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }

    *backend = &self->backend;
    return 0;
}