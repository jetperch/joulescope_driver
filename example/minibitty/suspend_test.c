/*
 * Copyright 2026 Jetperch LLC
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

/*
 * USB selective suspend HIL test (Windows).
 *
 * The device's MS OS descriptors enable selective suspend with AUTO_SUSPEND
 * on and DefaultIdleTimeout=2000 ms.  Measured behavior (2026-07-14): the
 * idle timer resets on every host I/O submission, INCLUDING the bulk IN
 * read resubmissions that follow each completed read.  A live host session
 * therefore never idles -- the device's own 1 Hz stats publishes complete a
 * pending read and the host's resubmission counts as activity -- even with
 * no OUT traffic at all.  The device only suspends when the host process
 * stops submitting I/O: frozen (Modern Standby), killed, or closed.
 *
 * This test asserts that live-session behavior:
 *   1. keep-alive on, session idle: stats must keep flowing (no suspend).
 *   2. h/link/keep=0: stats must KEEP flowing -- the host's pending-read
 *      turnover alone holds the device awake.  Old keep-alive-less hosts
 *      are therefore not at risk of mid-session suspends.  If a Windows
 *      update changes the idle accounting, this phase fails and the
 *      old-host compatibility assessment must be revisited.
 *
 * The actual suspend-on-freeze path cannot be exercised from inside this
 * process (a live process cannot stop its own read resubmissions).  Freeze
 * the process externally instead (NtSuspendProcess) while streaming, or
 * sleep the machine; then verify no device watchdog reset in the boot
 * history and that streaming recovers on thaw.
 */

#include "minibitty_exe_prv.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv/os_thread.h"
#include <stdio.h>
#include <string.h>

#define PHASE1_DURATION_MS      (6000)
#define PHASE1_STATS_MIN        (3)
#define PHASE2_DURATION_MS      (8000)   // > DefaultIdleTimeout (2 s) + margin
#define PHASE2_STATS_MIN        (4)

static volatile int64_t stat_time_ = 0;     // jsdrv_time_utc() of last stat
static volatile uint32_t stat_count_ = 0;

static void on_stat(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    (void) value;
    stat_time_ = jsdrv_time_utc();
    ++stat_count_;
}

static int32_t keepalive_set(struct app_s * self, uint8_t enable) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/link/keep");
    return jsdrv_publish(self->context, topic.topic, &jsdrv_union_u8(enable),
                         JSDRV_TIMEOUT_MS_DEFAULT);
}

static int run(struct app_s * self, const char * device) {
    struct jsdrv_topic_s topic;
    int rc = 0;

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_topic_set(&topic, device);
    jsdrv_topic_append(&topic, "c/comm/usbd/tx/!stat");
    rc = jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_stat, NULL,
                         JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("subscribe %s failed: %d\n", topic.topic, rc);
        jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);
        return rc;
    }

    // Phase 1: keep-alive on, idle session: device must NOT suspend.
    printf("phase 1: keep-alive on; expect device stats at ~1 Hz\n");
    uint32_t count_start = stat_count_;
    jsdrv_thread_sleep_ms(PHASE1_DURATION_MS);
    uint32_t count_phase1 = stat_count_ - count_start;
    printf("phase 1: %lu stats in %d ms\n", (unsigned long) count_phase1, PHASE1_DURATION_MS);
    if (count_phase1 < PHASE1_STATS_MIN) {
        printf("FAIL: device suspended (or stats absent) with keep-alive on\n");
        rc = 1;
        goto cleanup;
    }

    // Phase 2: keep-alive off: a live session must STAY awake on the
    // host's pending-read turnover alone (see the file comment).  This is
    // what shields old keep-alive-less hosts from mid-session suspends.
    printf("phase 2: keep-alive off; expect stats to continue (no suspend)\n");
    rc = keepalive_set(self, 0);
    if (rc) {
        printf("FAIL: keepalive=0 publish failed: %d\n", rc);
        goto cleanup;
    }
    count_start = stat_count_;
    jsdrv_thread_sleep_ms(PHASE2_DURATION_MS);
    uint32_t count_phase2 = stat_count_ - count_start;
    printf("phase 2: %lu stats in %d ms\n", (unsigned long) count_phase2, PHASE2_DURATION_MS);
    rc = keepalive_set(self, 1);  // restore before judging, so cleanup is normal
    if (rc) {
        printf("FAIL: keepalive=1 publish failed: %d\n", rc);
        goto cleanup;
    }
    if (count_phase2 < PHASE2_STATS_MIN) {
        printf("FAIL: device suspended mid-session without keep-alive.\n"
               "Windows idle accounting changed: pending-read turnover no\n"
               "longer counts as activity, so hosts without the keep-alive\n"
               "(older jsdrv) will suspend mid-session on this OS build.\n");
        rc = 1;
        goto cleanup;
    }
    printf("PASS\n");

cleanup:
    jsdrv_unsubscribe(self->context, topic.topic, on_stat, NULL, JSDRV_TIMEOUT_MS_DEFAULT);
    jsdrv_close(self->context, device, JSDRV_TIMEOUT_MS_DEFAULT);
    return rc;
}

static int usage(void) {
    printf(
        "usage: minibitty suspend_test device_path\n"
        "\n"
        "Verify USB selective suspend live-session behavior: an open\n"
        "session stays awake (no suspend) with and without the keep-alive.\n"
        "The suspend-on-freeze path needs an external process freeze; see\n"
        "the comment in suspend_test.c.  Windows only.\n"
        );
    return 1;
}

int on_suspend_test(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (NULL == device_filter) {
        printf("device_filter required\n");
        return usage();
    }

    ROE(app_match(self, device_filter));

    return run(self, self->device.topic);
}
