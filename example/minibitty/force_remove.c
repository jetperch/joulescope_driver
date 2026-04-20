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

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/os_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char * power_device_ = NULL;
static const char * target_device_ = NULL;
static int32_t iterations_ = -1;
static uint32_t stream_ms_ = 500;
static uint32_t settle_ms_ = 2000;
static uint32_t remove_timeout_ms_ = 3000;
static uint32_t open_timeout_ms_ = 10000;

static volatile bool target_present_ = false;
static volatile time_t last_data_time_ = 0;
static volatile uint64_t data_count_ = 0;

static int usage(void) {
    printf(
        "usage: minibitty force_remove [options]\n"
        "\n"
        "Stress test: stream JS320 data, then cut DUT power via JS220\n"
        "to force USB removal during streaming.  Loops until segfault\n"
        "or Ctrl-C.\n"
        "\n"
        "Options:\n"
        "  --power <filter>            Power-supply device (e.g. u/js220/002557)\n"
        "  --target <filter>           DUT device (e.g. u/js320/8W2A)\n"
        "  --iterations <N>            Max iterations (default: infinite)\n"
        "  --stream-ms <ms>            Stream before yanking power (default: 500)\n"
        "  --settle-ms <ms>            Wait after power-on for enumeration (default: 2000)\n"
        "  --remove-timeout-ms <ms>    Wait for @/!remove after yank (default: 3000)\n"
        "  --open-timeout-ms <ms>      jsdrv_open timeout (default: 10000)\n"
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

static int publish_u32(struct app_s * self, const char * device,
                       const char * subtopic, uint32_t value) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, device);
    jsdrv_topic_append(&topic, subtopic);
    int32_t rc = jsdrv_publish(self->context, topic.topic,
                               &jsdrv_union_u32(value),
                               JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("  publish %s = %u failed: %d\n", topic.topic, value, rc);
    }
    return rc;
}

static const char * device_prefix(const struct jsdrv_union_s * value) {
    if (value->type == JSDRV_UNION_STR) {
        return value->value.str;
    } else if (value->type == JSDRV_UNION_BIN && value->value.bin) {
        return (const char *) value->value.bin;
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

static void on_stream_data(void * user_data, const char * topic,
                           const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    (void) value;
    last_data_time_ = time(NULL);
    ++data_count_;
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

int on_force_remove(struct app_s * self, int argc, char * argv[]) {
    uint32_t iteration = 0;
    uint32_t successes = 0;
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
        } else if (0 == strcmp(argv[0], "--iterations") && argc > 1) {
            ARG_CONSUME();
            iterations_ = (int32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--stream-ms") && argc > 1) {
            ARG_CONSUME();
            stream_ms_ = (uint32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--settle-ms") && argc > 1) {
            ARG_CONSUME();
            settle_ms_ = (uint32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--remove-timeout-ms") && argc > 1) {
            ARG_CONSUME();
            remove_timeout_ms_ = (uint32_t) atoi(argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--open-timeout-ms") && argc > 1) {
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

    struct jsdrv_topic_s power_topic;
    jsdrv_topic_clear(&power_topic);
    ROE(app_match(self, power_device_));
    jsdrv_topic_set(&power_topic, self->device.topic);
    printf("Power device: %s\n", power_topic.topic);
    printf("Target filter: %s\n", target_device_);

    target_present_ = (0 == app_match(self, target_device_));

    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                        JSDRV_SFLAG_PUB, on_device_add, self, 0));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                        JSDRV_SFLAG_PUB, on_device_remove, self, 0));

    rc = jsdrv_open(self->context, power_topic.topic,
                    JSDRV_DEVICE_OPEN_MODE_RESUME,
                    JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("  ERROR: power device open failed: %d\n", rc);
        return rc;
    }
    publish_str(self, power_topic.topic, "s/i/range/select", "10 A");

    while (!quit_) {
        ++iteration;
        if ((iterations_ >= 0) && (iteration > (uint32_t) iterations_)) {
            break;
        }
        time_t now = time(NULL);
        printf("\n=== Iteration %u (successes=%u, t=%ld) ===\n",
               iteration, successes, (long) now);

        // Power ON (in case we are starting fresh or after an OFF iteration)
        printf("  [%u] Power ON\n", iteration);
        publish_str(self, power_topic.topic, "s/i/range/mode", "auto");

        // Wait for DUT enumeration via @/!add
        printf("  [%u] Waiting up to %u ms for target enumeration...\n",
               iteration, settle_ms_);
        rc = wait_for_condition(&target_present_, true, settle_ms_);
        if (rc) {
            printf("  [%u] WARN: target did not enumerate; retrying\n",
                   iteration);
            continue;
        }

        // Re-resolve the full topic (handles port/serial rebinding)
        rc = app_match(self, target_device_);
        if (rc) {
            printf("  [%u] WARN: app_match failed after enumerate\n",
                   iteration);
            continue;
        }
        struct jsdrv_topic_s dut;
        jsdrv_topic_set(&dut, self->device.topic);
        printf("  [%u] Target: %s\n", iteration, dut.topic);

        // Open DUT
        printf("  [%u] Opening DUT (timeout=%u ms)\n",
               iteration, open_timeout_ms_);
        rc = jsdrv_open(self->context, dut.topic,
                        JSDRV_DEVICE_OPEN_MODE_RESUME,
                        open_timeout_ms_);
        if (rc) {
            printf("  [%u] WARN: jsdrv_open failed: %d\n", iteration, rc);
            continue;
        }

        // Subscribe to i/v data streams
        struct jsdrv_topic_s i_topic;
        struct jsdrv_topic_s v_topic;
        jsdrv_topic_set(&i_topic, dut.topic);
        jsdrv_topic_append(&i_topic, "s/i/!data");
        jsdrv_topic_set(&v_topic, dut.topic);
        jsdrv_topic_append(&v_topic, "s/v/!data");
        jsdrv_subscribe(self->context, i_topic.topic,
                        JSDRV_SFLAG_PUB, on_stream_data, NULL, 0);
        jsdrv_subscribe(self->context, v_topic.topic,
                        JSDRV_SFLAG_PUB, on_stream_data, NULL, 0);

        // Enable streams
        uint64_t data_count_start = data_count_;
        last_data_time_ = time(NULL);
        publish_u32(self, dut.topic, "s/i/ctrl", 1);
        publish_u32(self, dut.topic, "s/v/ctrl", 1);

        // Let data flow
        printf("  [%u] Streaming for %u ms...\n", iteration, stream_ms_);
        uint32_t elapsed = 0;
        while (!quit_ && elapsed < stream_ms_) {
            jsdrv_thread_sleep_ms(10);
            elapsed += 10;
        }
        uint64_t msgs = data_count_ - data_count_start;
        printf("  [%u] Received %llu data messages\n",
               iteration, (unsigned long long) msgs);

        // *** The risky step: force-remove by cutting DUT power.
        // Do NOT jsdrv_close first -- we want the backend to detect
        // involuntary removal while data is still in-flight.
        printf("  [%u] *** POWER OFF (force remove) ***\n", iteration);
        publish_str(self, power_topic.topic, "s/i/range/mode", "off");

        // Wait for driver to observe @/!remove
        printf("  [%u] Waiting up to %u ms for @/!remove...\n",
               iteration, remove_timeout_ms_);
        rc = wait_for_condition(&target_present_, false, remove_timeout_ms_);
        if (rc) {
            printf("  [%u] WARN: @/!remove not observed within timeout\n",
                   iteration);
        } else {
            printf("  [%u] Removal observed\n", iteration);
        }

        // Let teardown finish (or crash) before next iteration
        jsdrv_thread_sleep_ms(500);

        ++successes;
    }

    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                      on_device_add, self, 0);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                      on_device_remove, self, 0);

    // Restore power so next run starts clean
    publish_str(self, power_topic.topic, "s/i/range/mode", "auto");
    jsdrv_close(self->context, power_topic.topic,
                JSDRV_TIMEOUT_MS_DEFAULT);

    printf("\n=== Done: %u iterations survived ===\n", successes);
    return 0;
}
