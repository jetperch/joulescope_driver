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
#include "jsdrv/error_code.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv/topic.h"
#include "tinyprintf.h"


#include "jsdrv_prv/windows.h"

enum state_e { // See opts_state
    ST_NOT_PRESENT = 0,  //
    ST_CLOSED = 1,
    ST_OPENING = 3,
    ST_OPEN = 2,
};

struct dev_s {
    struct jsdrvp_ul_device_s ul; // MUST BE FIRST!
    struct jsdrvp_ll_device_s ll;
    struct jsdrv_context_s * context;

    enum state_e state;
    bool do_exit;
    HANDLE thread;
};

static void send_to_frontend(struct dev_s * d, const char * subtopic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(d->context, "", value);
    tfp_snprintf(m->topic, sizeof(m->topic), "%s/%s", d->ll.prefix, subtopic);
    jsdrvp_backend_send(d->context, m);
}

static void update_state(struct dev_s * d, enum state_e state) {
    d->state = state;
    send_to_frontend(d, "h/state", &jsdrv_union_u32_r(d->state));
}

static const char * prefix_match_and_strip(const char * prefix, const char * topic) {
    while (*prefix) {
        if (*prefix++ != *topic++) {
            return NULL;
        }
    }
    if (*topic++ != '/') {
        return NULL;
    }
    return topic;
}

static bool handle_cmd(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    const char * topic = prefix_match_and_strip(d->ll.prefix, msg->topic);
    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            // full driver shutdown
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if (!topic) {
        JSDRV_LOGE("handle_cmd mismatch %s, %s", msg->topic, d->ll.prefix);
    } else if (topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, topic)) {
            // todo
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, topic)) {
            // todo
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if ((topic[0] == 'h') && (topic[1] == '/')) {
        // todo handle any host-side parameters here.
    } else {
        // todo all other device parameters
    }
    jsdrvp_msg_free(d->context, msg);
    return rv;
}

static DWORD WINAPI device_thread(LPVOID lpParam) {
    struct jsdrvp_msg_s * msg;
    struct dev_s * d = (struct dev_s *) lpParam;
    JSDRV_LOGI("emu device thread started for %s", d->ll.prefix);

    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count;
    handles[0] = msg_queue_handle_get(d->ul.cmd_q);
    handle_count = 1;

    // todo publish metadata for our parameters
    // todo publish state close

    update_state(d, ST_CLOSED);

    while (!d->do_exit) {
        WaitForMultipleObjects(handle_count, handles, false, 5000);
        while (1) {
            msg = msg_queue_pop_immediate(d->ul.cmd_q);
            if (NULL == msg) {
                break;
            }
            handle_cmd(d, msg);
        }
    }

    JSDRV_LOGI("emu device thread done %s", d->ll.prefix);
    return 0;
}

static void join(struct jsdrvp_ul_device_s * device) {
    if (device) {
        struct dev_s * d = (struct dev_s *) device;
        if (d->thread) {
            jsdrvp_send_finalize_msg(d->context, d->ul.cmd_q, "");
            // and wait for thread to exit.
            if (WAIT_OBJECT_0 != WaitForSingleObject(d->thread, 1000)) {
                        JSDRV_LOGE("js110 thread not closed cleanly.");
            }
            CloseHandle(d->thread);
            d->thread = NULL;
        }

                JSDRV_LOGD3("js220 free %p", d);
        jsdrv_free(d);
    }
}

int32_t jsdrvp_ul_emu_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll) {
    JSDRV_DBC_NOT_NULL(device);
    JSDRV_DBC_NOT_NULL(context);
    JSDRV_DBC_NOT_NULL(ll);
    *device = NULL;
    struct dev_s * d = jsdrv_alloc_clr(sizeof(struct dev_s));
    JSDRV_LOGD3("jsdrvp_ul_emu_factory %p", d);
    d->context = context;
    d->ll = *ll;
    d->ul.cmd_q = msg_queue_init();
    d->ul.join = join;

    d->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            device_thread,          // thread function name
            d,                      // argument to thread function
            0,                      // use default creation flags
            NULL);                  // returns the thread identifier
    if (d->thread == NULL) {
        JSDRV_LOGE("CreateThread failed");
        return JSDRV_ERROR_UNSPECIFIED;
    }
    if (!SetThreadPriority(d->thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        WINDOWS_LOGE("%s", "SetThreadPriority");
    }
    *device = &d->ul;
    return 0;
}
