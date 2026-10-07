/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
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

// js110_usb unit tests.  These include the device source directly,
// following the js220_usb_test pattern, and stub the frontend services.
// A responder thread stands in for the lower-level USB device: it pops
// requests from d->ll.cmd_q and returns them on d->ll.rsp_q.  The message
// stubs count allocations so that tests can check message ownership.

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "jsdrv.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/platform.h"

#include "../../../src/devices/js110/js110_usb.c"

#define PREFIX "u/js110/test"

// --- Stubs (frontend-side services not linked from jsdrv.c) ---

static volatile int32_t msg_alloc_count_;
static volatile int32_t msg_free_count_;

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = jsdrv_alloc_clr(sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    ++msg_alloc_count_;
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_bin(m->payload.bin, 0);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context,
                                             const char * topic,
                                             const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    return m;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    (void) context;
    ++msg_free_count_;
    jsdrv_free(msg);
}

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    jsdrvp_msg_free(context, msg);
}

void jsdrvp_send_finalize_msg(struct jsdrv_context_s * context, struct msg_queue_s * q,
                              const char * topic) {
    (void) context; (void) q; (void) topic;
}

// --- Lower-level device responder ---

struct responder_s {
    struct js110_dev_s * d;
    volatile bool quit;
    uint8_t version;             // JS110_HOST_API_VERSION unless testing a mismatch
    uint32_t busy_count;         // status responses with settings_result -1 or 19
    int32_t settings_result;     // the final settings_result
    volatile uint32_t status_count;
    jsdrv_thread_t thread;
};

static JSDRV_THREAD_RETURN_TYPE responder_thread(JSDRV_THREAD_ARG_TYPE arg) {
    struct responder_s * r = (struct responder_s *) arg;
    while (!r->quit) {
        struct jsdrvp_msg_s * m = NULL;
        if (msg_queue_pop(r->d->ll.cmd_q, &m, 10) || (NULL == m)) {
            continue;
        }
        struct js110_host_packet_s * pkt = (struct js110_host_packet_s *) m->payload.bin;
        memset(pkt, 0, sizeof(*pkt));
        pkt->header.version = r->version;
        pkt->header.type = JS110_HOST_PACKET_TYPE_STATUS;
        uint32_t idx = r->status_count++;
        if (idx < r->busy_count) {
            pkt->payload.status.settings_result = (idx & 1) ? 19 : -1;
        } else {
            pkt->payload.status.settings_result = r->settings_result;
        }
        m->value.size = STATUS_SETUP.s.wLength;
        msg_queue_push(r->d->ll.rsp_q, m);
    }
    JSDRV_THREAD_RETURN();
}

// --- Fixture ---

static int setup(void ** state) {
    struct responder_s * r = jsdrv_alloc_clr(sizeof(struct responder_s));
    struct js110_dev_s * d = jsdrv_alloc_clr(sizeof(struct js110_dev_s));
    jsdrv_cstr_copy(d->ll.prefix, PREFIX, sizeof(d->ll.prefix));
    d->ll.cmd_q = msg_queue_init();
    d->ll.rsp_q = msg_queue_init();
    r->d = d;
    r->version = JS110_HOST_API_VERSION;
    msg_alloc_count_ = 0;
    msg_free_count_ = 0;
    *state = r;
    return 0;
}

static int teardown(void ** state) {
    struct responder_s * r = (struct responder_s *) *state;
    r->quit = true;
    jsdrv_thread_join(&r->thread, 1000);
    msg_queue_finalize(r->d->ll.cmd_q, r->d->context);
    msg_queue_finalize(r->d->ll.rsp_q, r->d->context);
    jsdrv_free(r->d);
    jsdrv_free(r);
    return 0;
}

static void responder_start(struct responder_s * r) {
    assert_int_equal(0, jsdrv_thread_create(&r->thread, responder_thread, r, 0));
}

// --- Tests ---

static void test_wait_for_sensor_command_frees_status_messages(void ** state) {
    struct responder_s * r = (struct responder_s *) *state;
    r->busy_count = 3;
    r->settings_result = 0;
    responder_start(r);
    assert_int_equal(0, wait_for_sensor_command(r->d));
    assert_int_equal(4, r->status_count);
    assert_int_equal(4, msg_alloc_count_);
    assert_int_equal(msg_alloc_count_, msg_free_count_);
}

static void test_wait_for_sensor_command_error_frees_status_message(void ** state) {
    struct responder_s * r = (struct responder_s *) *state;
    r->version = JS110_HOST_API_VERSION + 1;
    responder_start(r);
    assert_int_equal(JSDRV_ERROR_NOT_SUPPORTED, wait_for_sensor_command(r->d));
    assert_int_equal(1, r->status_count);
    assert_int_equal(msg_alloc_count_, msg_free_count_);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test_setup_teardown(test_wait_for_sensor_command_frees_status_messages, setup, teardown),
            cmocka_unit_test_setup_teardown(test_wait_for_sensor_command_error_frees_status_message, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
