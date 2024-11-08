/*
 * Copyright 2022-2024 Jetperch LLC
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
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <inttypes.h>


static int32_t publish(struct app_s * self, const char * device, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    char buf[32];
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, topic);
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("Publish %s => %s\n", t.topic, buf);
    int32_t rc = jsdrv_publish(self->context, t.topic, value, timeout_ms);
    if (rc) {
        printf("publish %s failed with %d\n", topic, (int) rc);
    }
    return rc;
}

static void data_fn(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    FILE * f = (FILE *) user_data;
    if (value->type != JSDRV_UNION_BIN) {
        printf("data_fn: unsupported type\n");
        return;
    }
    struct jsdrv_stream_signal_s * s = (struct jsdrv_stream_signal_s *) value->value.bin;
    if ((JSDRV_DATA_TYPE_FLOAT != s->element_type) || (32 != s->element_size_bits)) {
        printf("data_fn: unsupported data type\n");
        return;
    }
    fwrite(s->data, 1, (s->element_count * s->element_size_bits) / 8, f);
}

static FILE * channel_init(struct app_s * self, const char * device, const char * channel, const char * filename) {
    int32_t rc;
    struct jsdrv_topic_s t;
    if (NULL == filename) {
        return NULL;
    }
    FILE * f = fopen(filename, "wb");
    if (NULL == f) {
        return NULL;
    }
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, "s");
    jsdrv_topic_append(&t, channel);
    jsdrv_topic_append(&t, "ctrl");
    rc = jsdrv_publish(self->context, t.topic, &jsdrv_union_u32_r(1), 0);
    if (rc) {
        fclose(f);
        return NULL;
    }

    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, "s");
    jsdrv_topic_append(&t, channel);
    jsdrv_topic_append(&t, "!data");
    rc = jsdrv_subscribe(self->context, t.topic, JSDRV_SFLAG_PUB, data_fn, (void *) f, 0);
    if (rc) {
        fclose(f);
        return NULL;
    }
    return f;
}

static void channel_finalize(struct app_s * self, const char * device, const char * channel, FILE * f) {
    int32_t rc;
    struct jsdrv_topic_s t;
    if (f == NULL) {
        return;
    }

    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, "s");
    jsdrv_topic_append(&t, channel);
    jsdrv_topic_append(&t, "!data");
    rc = jsdrv_unsubscribe(self->context, t.topic, data_fn, (void *) f, 0);
    if (rc) {
        printf("jsdrv_unsubscribe failed with %d\n", rc);
    }

    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, "s");
    jsdrv_topic_append(&t, channel);
    jsdrv_topic_append(&t, "ctrl");
    rc = jsdrv_publish(self->context, t.topic, &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("jsdrv_publish failed with %d\n", rc);
    }

    fclose(f);
}


static int usage(void) {
    printf("usage: jsdrv capture [<option> <value>]"
           "\n"
           "Options:\n"
           "    -d, --duration  The duration in milliseconds.\n"
           "                    0 (default) runs until CTRL-C\n"
           "    -f, --frequency The sampling frequency in Hz.\n"
           "    --filter        Downsample filter type uint32.\n"
           "    -i, --current   The capture filename for current.\n"
           "    -v, --voltage   The capture filename for voltage.\n"
           "    -p, --power     The capture filename for power.\n"
           "\n");
    return 1;
}

int on_capture(struct app_s * self, int argc, char * argv[]) {
    uint32_t frequency = 0;
    uint32_t filter = 0;
    char * filename_i = NULL;
    char * filename_v = NULL;
    char * filename_p = NULL;
    FILE * file_i = NULL;
    FILE * file_v = NULL;
    FILE * file_p = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if ((0 == strcmp(argv[0], "-d")) || (0 == strcmp(argv[0], "--duration"))) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &self->duration_ms));
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "-f")) || (0 == strcmp(argv[0], "--frequency"))) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &frequency));
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--filter")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jsdrv_cstr_to_u32(argv[0], &filter));
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "-i")) || (0 == strcmp(argv[0], "--current"))) {
            ARG_CONSUME();
            ARG_REQUIRE();
            filename_i = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "-v")) || (0 == strcmp(argv[0], "--voltage"))) {
            ARG_CONSUME();
            ARG_REQUIRE();
            filename_v = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "-p")) || (0 == strcmp(argv[0], "--power"))) {
            ARG_CONSUME();
            ARG_REQUIRE();
            filename_p = argv[0];
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, NULL));
    char * device = self->device.topic;
    ROE(publish(self, device, JSDRV_MSG_OPEN, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    if (jsdrv_cstr_starts_with(device, "u/js220")) {
        ROE(publish(self, device, "s/i/range/mode", &jsdrv_union_cstr_r("auto"), 0));
    } else if (jsdrv_cstr_starts_with(device, "u/js110")) {
        ROE(publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("auto"), 0));
    }
    if (frequency) {
        if (filter) {
            ROE(publish(self, device, "h/filter", &jsdrv_union_u32_r(filter), 0));
        }
        ROE(publish(self, device, "h/fs", &jsdrv_union_u32_r(frequency), 0));
    }

    file_i = channel_init(self, device, "i", filename_i);
    file_v = channel_init(self, device, "v", filename_v);
    file_p = channel_init(self, device, "p", filename_p);

    int64_t t_end = jsdrv_time_utc() + JSDRV_TIME_MILLISECOND * (int64_t) self->duration_ms;
    while (!quit_) {
        jsdrv_thread_sleep_ms(1); // do nothing
        if (self->duration_ms && (jsdrv_time_utc() >= t_end)) {
            break;
        }
    }

    channel_finalize(self, device, "i", file_i);
    channel_finalize(self, device, "v", file_v);
    channel_finalize(self, device, "p", file_p);
    ROE(publish(self, device, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
