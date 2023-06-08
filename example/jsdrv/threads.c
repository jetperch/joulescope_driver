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

#include "jsdrv_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <inttypes.h>

struct thread_args_s {
    size_t index;
    struct app_s * app;
    jsdrv_thread_t thread;
};

volatile bool quit_ = false;

static int32_t publish(struct app_s * self, const char * device, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    char buf[32];
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, topic);
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    int32_t rc = jsdrv_publish(self->context, t.topic, value, timeout_ms);
    if (rc) {
        printf("publish %s failed with %d %s %s\n", topic,
               (int) rc, jsdrv_error_code_name(rc), jsdrv_error_code_description(rc));
    }
    return rc;
}

#if _WIN32
static DWORD __stdcall thread_fn(LPVOID arg) {
#else
static void * thread_fn(void * arg) {
#endif
    struct thread_args_s * t = (struct thread_args_s *) arg;
    int32_t counter = 0;
    while (!quit_) {
        uint64_t v64 = 10 | (((uint64_t) t->index) << 56) | (((uint64_t) counter) << 32);
        publish(t->app, t->app->device.topic, "h/timeout", &jsdrv_union_u64_r(v64), 1000);
        ++counter;
    }
    printf("query thread %d %d\n", (int) t->index, (int) counter);
    return 0;
}

static int usage(void) {
    printf("usage: jsdrv_util threads [--duration duration_ms]\n");
    return 1;
}

int on_threads(struct app_s * self, int argc, char * argv[]) {
    uint32_t duration_ms = 2000;
    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if (0 == strcmp(argv[0], "--duration")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &duration_ms));
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, NULL));
    ROE(publish(self, self->device.topic, JSDRV_MSG_OPEN, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));

    int32_t rc = publish(self, self->device.topic, "h/timeout", &jsdrv_union_u32(100), 10);
    if (rc != JSDRV_ERROR_TIMED_OUT) {
        printf("timeout expected, failed with %d\n", (int) rc);
    }

    struct thread_args_s threads[10];
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(threads); ++i) {
        struct thread_args_s * t = &threads[i];
        t->app = self;
        t->index = i;
        ROE(jsdrv_thread_create(&t->thread, thread_fn, t, 0));
    }

    int64_t t_end = jsdrv_time_utc() + duration_ms * JSDRV_TIME_MILLISECOND;
    while (!quit_ && (jsdrv_time_utc() < t_end)) {
        jsdrv_thread_sleep_ms(5);
    }
    quit_ = true;
    for (size_t i = 0; i < JSDRV_ARRAY_SIZE(threads); ++i) {
        ROE(jsdrv_thread_join(&threads[i].thread, 1000));
    }

    ROE(publish(self, self->device.topic, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
