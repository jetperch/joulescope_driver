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

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/os_thread.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char * power_device_ = NULL;
static const char * target_device_ = NULL;
static int32_t count_ = -1;  // negative = infinite, 0 = power toggle only
static uint32_t delay_ms_ = 0;
static uint32_t open_timeout_ms_ = 10000;

static volatile bool target_present_ = false;

static int usage(void) {
    printf(
        "usage: minibitty power_cycle [options]\n"
        "\n"
        "Power cycle a target device and test open reliability.\n"
        "\n"
        "Options:\n"
        "  --power <device>   Power supply device filter\n"
        "  --target <device>  Target device filter\n"
        "  --count <N>        Iterations: negative=infinite, 0=power toggle only\n"
        "  --delay <ms>       Post-detection delay in ms (default=2500)\n"
        "  --timeout <ms>     Target open timeout in ms (default=10000)\n"
    );
    return 1;
}

static int publish_str(struct app_s * self, const char * device,
                       const char * subtopic, const char * value) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, device);
    jsdrv_topic_append(&topic, subtopic);
    int32_t rc = jsdrv_publish(self->context, topic.topic,
                               &jsdrv_union_cstr_r(value),
                               JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("  publish %s = \"%s\" failed: %d\n", topic.topic, value, rc);
    }
    return rc;
}

static const char * device_prefix(const struct jsdrv_union_s * value) {
    if (value->type == JSDRV_UNION_STR) {
        return value->value.str;
    } else if (value->type == JSDRV_UNION_BIN && value->value.bin) {
        return (const char *) value->value.bin;  // prefix is first field
    }
    return NULL;
}

static bool prefix_matches_target(const char * prefix) {
    return prefix && target_device_
        && jsdrv_cstr_starts_with(prefix, target_device_);
}

static void on_device_add(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (prefix_matches_target(device_prefix(value))) {
        target_present_ = true;
    }
}

static void on_device_remove(void * user_data, const char * topic,
                             const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (prefix_matches_target(device_prefix(value))) {
        target_present_ = false;
    }
}

static int wait_for_condition(volatile bool * flag, bool desired,
                              uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (!quit_ && elapsed < timeout_ms) {
        if (*flag == desired) {
            return 0;
        }
        jsdrv_thread_sleep_ms(10);
        elapsed += 10;
    }
    return 1;
}

int on_power_cycle(struct app_s * self, int argc, char * argv[]) {
    uint32_t pass = 0;
    uint32_t fail = 0;
    int32_t rc;

    while (argc) {
        if (0 == strcmp(argv[0], "--power") && argc > 1) {
            ARG_CONSUME();
            power_device_ = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--target") && argc > 1) {
            ARG_CONSUME();
            target_device_ = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--count") && argc > 1) {
            ARG_CONSUME();
            count_ = (int32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--delay") && argc > 1) {
            ARG_CONSUME();
            delay_ms_ = (uint32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--timeout") && argc > 1) {
            ARG_CONSUME();
            open_timeout_ms_ = (uint32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else {
            printf("Unknown argument: %s\n", argv[0]);
            return usage();
        }
    }

    if (!power_device_ || !target_device_) {
        printf("Both --power and --target are required.\n");
        return usage();
    }

    // Match power device
    struct jsdrv_topic_s power_topic;
    jsdrv_topic_clear(&power_topic);
    ROE(app_match(self, power_device_));
    jsdrv_topic_set(&power_topic, self->device.topic);
    printf("Power device: %s\n", power_topic.topic);

    // Set initial target presence from device list
    target_present_ = (0 == app_match(self, target_device_));

    // Subscribe to device add/remove events
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                        JSDRV_SFLAG_PUB, on_device_add, self, 0));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                        JSDRV_SFLAG_PUB, on_device_remove, self, 0));

    // Open power device
    rc = jsdrv_open(self->context, power_topic.topic,
                    JSDRV_DEVICE_OPEN_MODE_RESUME,
                    JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("  ERROR: power device open failed: %d\n", rc);
        return rc;
    }
    publish_str(self, power_topic.topic, "s/i/range/select", "10 A");

    for (uint32_t iteration = 1; !quit_; ++iteration) {
        if ((count_ >= 0) && (iteration > (uint32_t) (count_ + 1))) {
            break;
        }
        printf("\n--- Iteration %u (pass=%u, fail=%u) ---\n",
               iteration, pass, fail);

        // Power OFF
        printf("  Power OFF\n");
        publish_str(self, power_topic.topic, "s/i/range/mode", "off");

        // Wait for target device to disappear
        printf("  Waiting for target removal...\n");
        rc = wait_for_condition(&target_present_, false, 2500);
        if (rc) {
            printf("  WARN: target did not disappear within timeout\n");
        }
        jsdrv_thread_sleep_ms(500);  // be nice to the OS

        // Power ON
        printf("  Power ON\n");
        publish_str(self, power_topic.topic, "s/i/range/mode", "manual");

        // Wait for target device to appear
        printf("  Waiting for target insertion...\n");
        rc = wait_for_condition(&target_present_, true, 2500);
        if (rc) {
            printf("  FAIL: target device did not appear\n");
            ++fail;
            continue;
        }
        printf("  Target detected, waiting %u ms\n", delay_ms_);
        jsdrv_thread_sleep_ms(delay_ms_);

        if (0 == count_) {
            ++pass;
            continue;
        }

        // Match target device for its full topic
        rc = app_match(self, target_device_);
        if (rc) {
            printf("  FAIL: target device match failed\n");
            ++fail;
            continue;
        }

        // Open target device (single attempt)
        printf("  Opening target (timeout=%u ms)\n", open_timeout_ms_);
        rc = jsdrv_open(self->context, self->device.topic,
                        JSDRV_DEVICE_OPEN_MODE_RESUME,
                        open_timeout_ms_);
        if (rc) {
            printf("  FAIL: target open failed: %d\n", rc);
            ++fail;
            continue;
        }

        printf("  PASS\n");
        ++pass;

        // Brief test: enable auto ranging
        publish_str(self, self->device.topic, "s/i/range/mode", "auto");

        jsdrv_close(self->context, self->device.topic,
                    JSDRV_TIMEOUT_MS_DEFAULT);
    }

    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                      on_device_add, self, 0);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                      on_device_remove, self, 0);

    // Close power device
    jsdrv_close(self->context, power_topic.topic,
                JSDRV_TIMEOUT_MS_DEFAULT);

    printf("\n=== Results: pass=%u, fail=%u, total=%u ===\n",
           pass, fail, pass + fail);
    return fail ? 1 : 0;
}
