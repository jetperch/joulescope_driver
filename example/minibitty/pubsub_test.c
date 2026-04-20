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
 * @brief Regression test for MB device pubsub API features.
 *
 * Tests return codes, confirmed delivery, metadata, and
 * state save/restore using a physical MiniBitty device.
 */

#include "minibitty_exe_prv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/cstr.h"
#include "jsdrv/topic.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "jsdrv/os_thread.h"

// --- Test state ---

struct test_state_s {
    struct app_s * app;
    int pass_count;
    int fail_count;

    // return code tracking
    volatile int32_t rc_received;
    volatile bool rc_valid;
    char rc_topic[JSDRV_TOPIC_LENGTH_MAX];

    // metadata tracking
    volatile bool meta_received;
    char meta_topic[JSDRV_TOPIC_LENGTH_MAX];
    char meta_json[1024];

    // publish tracking
    volatile int32_t pub_value;
    volatile bool pub_received;
};

static struct test_state_s ts_;

static void test_pass(const char * name) {
    printf("  PASS: %s\n", name);
    ts_.pass_count++;
}

static void test_fail(const char * name, const char * detail) {
    printf("  FAIL: %s - %s\n", name, detail);
    ts_.fail_count++;
}

#define TEST_ASSERT(name, cond, detail) do { \
    if (cond) { test_pass(name); }           \
    else { test_fail(name, detail); }        \
} while (0)

// --- Callbacks ---

static void on_metadata(void * user_data,
                        const char * topic,
                        const struct jsdrv_union_s * value) {
    (void) user_data;
    if (value->type == JSDRV_UNION_JSON || value->type == JSDRV_UNION_STR) {
        jsdrv_cstr_copy(ts_.meta_topic, topic, sizeof(ts_.meta_topic));
        jsdrv_cstr_copy(ts_.meta_json, value->value.str, sizeof(ts_.meta_json));
        ts_.meta_received = true;
    }
}

// --- Test: Host-side command return codes ---
//
// This is the most fundamental test: commands handled entirely
// on the host side in handle_cmd should return codes.

static int test_host_cmd_return_codes(struct app_s * self) {
    printf("\n--- Test: Host-side command return codes ---\n");
    struct jsdrv_topic_s topic;
    char detail[256];

    // Test 1: Publish to h/in/frecord with timeout
    // This is handled on the host side in mb_device.c handle_cmd.
    // Should get return code 0 (success).
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/in/frecord");
    printf("  publish topic: %s\n", topic.topic);
    int32_t rc = jsdrv_publish(self->context, topic.topic,
        &jsdrv_union_cstr_r(""), 1000);
    snprintf(detail, sizeof(detail), "got rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("h/in/frecord returns success", rc == 0, detail);

    // Test 2: Publish to unrecognized h/ topic
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/bogus/topic");
    printf("  publish topic: %s\n", topic.topic);
    rc = jsdrv_publish(self->context, topic.topic,
        &jsdrv_union_u32(0), 1000);
    snprintf(detail, sizeof(detail), "got rc=%d (%s), expected %d (%s)",
        (int) rc, jsdrv_error_code_name(rc),
        (int) JSDRV_ERROR_NOT_SUPPORTED,
        jsdrv_error_code_name(JSDRV_ERROR_NOT_SUPPORTED));
    TEST_ASSERT("h/bogus/topic returns NOT_SUPPORTED",
        rc == JSDRV_ERROR_NOT_SUPPORTED, detail);

    return 0;
}

// --- Test: Return codes from device ---

static int test_device_return_codes(struct app_s * self) {
    printf("\n--- Test: Device return codes ---\n");
    struct jsdrv_topic_s topic;
    char detail[256];

    // Publish with timeout=0 (fire-and-forget)
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/test/value");
    int32_t rc = jsdrv_publish(self->context, topic.topic,
        &jsdrv_union_u32(42), 0);
    TEST_ASSERT("publish fire-and-forget returns 0",
        rc == 0, "expected rc=0 for timeout=0 publish");

    // Publish with timeout (synchronous confirmed delivery).
    // Use a test topic that may not exist on the device.
    // Device returns NOT_FOUND for unknown topics, which still
    // proves the round-trip confirmation works.
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/test/value2");
    rc = jsdrv_publish(self->context, topic.topic,
        &jsdrv_union_u32(99), 2000);
    snprintf(detail, sizeof(detail), "got rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("confirmed delivery round-trip",
        rc != JSDRV_ERROR_TIMED_OUT, detail);

    return 0;
}

// --- Test: Metadata from upper driver ---

static int test_metadata(struct app_s * self) {
    printf("\n--- Test: Metadata ---\n");

    ts_.meta_received = false;
    ts_.meta_json[0] = 0;
    ROE(jsdrv_subscribe(self->context, self->device.topic,
        JSDRV_SFLAG_METADATA_RSP | JSDRV_SFLAG_RETAIN,
        on_metadata, NULL, 0));

    jsdrv_thread_sleep_ms(200);

    if (ts_.meta_received) {
        char detail[256];
        snprintf(detail, sizeof(detail),
            "topic=%s, json=%.60s", ts_.meta_topic, ts_.meta_json);
        test_pass("received metadata response");
        printf("  INFO: %s\n", detail);
    } else {
        printf("  SKIP: no metadata received (upper driver may not publish any yet)\n");
    }

    ROE(jsdrv_unsubscribe(self->context, self->device.topic,
        on_metadata, NULL, 0));
    return 0;
}

// --- Test: State restore (RESUME mode) ---
//
// State restore replays retained host pubsub values to the
// physical device via the device's cmd_q.  We verify by
// querying the pubsub retained value after reopen -- the
// retained value should still be present in the host pubsub.

static int test_state_restore(struct app_s * self) {
    printf("\n--- Test: State restore (RESUME) ---\n");
    struct jsdrv_topic_s topic;
    char detail[256];
    int32_t rc;

    // Publish a retained value to a device topic
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/test/persist");
    struct jsdrv_union_s val = jsdrv_union_u32(12345);
    val.flags |= JSDRV_UNION_FLAG_RETAIN;
    rc = jsdrv_publish(self->context, topic.topic, &val, 0);
    TEST_ASSERT("publish retained value", rc == 0, "publish failed");
    jsdrv_thread_sleep_ms(100);

    // Verify retained value is queryable
    struct jsdrv_union_s qval = jsdrv_union_i32(0);
    rc = jsdrv_query(self->context, topic.topic, &qval, 1000);
    snprintf(detail, sizeof(detail), "query rc=%d, value=%" PRId32,
        (int) rc, qval.value.i32);
    TEST_ASSERT("retained value queryable before close",
        rc == 0 && qval.value.i32 == 12345, detail);

    // Close device
    printf("  closing device...\n");
    rc = jsdrv_close(self->context, self->device.topic, 5000);
    snprintf(detail, sizeof(detail), "close rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("close device", rc == 0, detail);
    if (rc) {
        printf("  Cannot continue: close failed\n");
        return 0;
    }
    jsdrv_thread_sleep_ms(1000);

    // Verify retained value persists across close
    qval = jsdrv_union_i32(0);
    rc = jsdrv_query(self->context, topic.topic, &qval, 1000);
    snprintf(detail, sizeof(detail), "query rc=%d, value=%" PRId32,
        (int) rc, qval.value.i32);
    TEST_ASSERT("retained value persists after close",
        rc == 0 && qval.value.i32 == 12345, detail);

    // Reopen with RESUME mode — retained values replayed to device
    printf("  reopening with RESUME...\n");
    rc = jsdrv_open(self->context, self->device.topic, 1, 5000);
    snprintf(detail, sizeof(detail), "open rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("reopen with RESUME", rc == 0, detail);
    if (rc) {
        printf("  Cannot continue: reopen failed\n");
        return 0;
    }
    jsdrv_thread_sleep_ms(500);

    // Verify retained value still queryable after resume
    // (state restore replays it TO the device via cmd_q,
    // the host pubsub retains it throughout)
    qval = jsdrv_union_i32(0);
    rc = jsdrv_query(self->context, topic.topic, &qval, 1000);
    snprintf(detail, sizeof(detail), "query rc=%d, value=%" PRId32,
        (int) rc, qval.value.i32);
    TEST_ASSERT("retained value available after RESUME",
        rc == 0 && qval.value.i32 == 12345, detail);

    return 0;
}

// --- Test: Open modes ---

static int test_open_modes(struct app_s * self) {
    printf("\n--- Test: Open modes ---\n");
    char detail[256];
    int32_t rc;

    // Close current device
    printf("  closing...\n");
    rc = jsdrv_close(self->context, self->device.topic, 5000);
    snprintf(detail, sizeof(detail), "close rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("close for mode test", rc == 0, detail);
    if (rc) {
        printf("  Cannot continue: close failed\n");
        return 0;
    }
    jsdrv_thread_sleep_ms(1000);

    // Open with DEFAULTS mode (0)
    printf("  opening DEFAULTS...\n");
    rc = jsdrv_open(self->context, self->device.topic, 0, 5000);
    snprintf(detail, sizeof(detail), "open DEFAULTS rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("open DEFAULTS mode", rc == 0, detail);

    // Close
    printf("  closing...\n");
    rc = jsdrv_close(self->context, self->device.topic, 5000);
    if (rc) {
        printf("  close failed rc=%d, cannot continue\n", rc);
        return 0;
    }
    jsdrv_thread_sleep_ms(1000);

    // Open with RESUME mode (1)
    printf("  opening RESUME...\n");
    rc = jsdrv_open(self->context, self->device.topic, 1, 5000);
    snprintf(detail, sizeof(detail), "open RESUME rc=%d (%s)",
        (int) rc, jsdrv_error_code_name(rc));
    TEST_ASSERT("open RESUME mode", rc == 0, detail);

    return 0;
}

// --- Main test entry point ---

static int usage(void) {
    printf("usage: minibitty pubsub_test [DEVICE_FILTER]\n"
           "\n"
           "Regression test for MB device pubsub API features.\n"
           "Tests return codes, confirmed delivery, metadata,\n"
           "and state save/restore.\n"
           "\n"
           "Requires a physical MiniBitty device.\n"
           "\nTip: run with --log-level info for diagnostics.\n");
    return 1;
}

int on_pubsub_test(struct app_s * self, int argc, char * argv[]) {
    const char * filter = NULL;
    memset(&ts_, 0, sizeof(ts_));
    ts_.app = self;

    while (argc > 0) {
        if (argv[0][0] == '-') {
            printf("Unknown option: %s\n", argv[0]);
            return usage();
        } else {
            filter = argv[0];
        }
        ARG_CONSUME();
    }

    ROE(app_match(self, filter));
    printf("Device: %s\n", self->device.topic);

    // Open device initially with RESUME mode
    printf("Opening device...\n");
    ROE(jsdrv_open(self->context, self->device.topic, 1, 5000));
    jsdrv_thread_sleep_ms(500);
    printf("Device open.\n");

    // Run test suites
    int rc = 0;
    if (!rc) rc = test_host_cmd_return_codes(self);
    if (!rc) rc = test_device_return_codes(self);
    if (!rc) rc = test_metadata(self);
    if (!rc) rc = test_state_restore(self);
    if (!rc) rc = test_open_modes(self);

    // Cleanup
    jsdrv_close(self->context, self->device.topic, 5000);

    // Summary
    printf("\n=== Results: %d passed, %d failed ===\n",
           ts_.pass_count, ts_.fail_count);

    if (rc) {
        return rc;
    }
    return ts_.fail_count ? 1 : 0;
}
