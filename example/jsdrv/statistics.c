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
#include "jsdrv/union.h"
#include "jsdrv_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/list.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "jsdrv_prv/thread.h"


typedef int32_t (*device_fn)(const char * device, void * user_data);

static int usage(void) {
    printf("usage: jsdrv_util statistics\n");
    return 1;
}

static int32_t foreach_device(const char * devices, device_fn fn, void * user_data) {
    int32_t rc = 0;
    char device[JSDRV_TOPIC_LENGTH_MAX];
    const char * a = devices;
    char * b = device;

    if ((NULL == a) || (!a[0])) {
        return 0;
    }
    while (1) {
        if ((*a == 0) || (*a == ',')) {
            *b = 0;
            rc = fn(device, user_data);
            if (*a == 0) {
                break;
            }
            if (rc) {
                printf("foreach_device error %d on %s", rc, b);
                return rc;
            }
            ++a;
            b = device;
        } else {
            *b++ = *a++;
        }
    }
    return 0;
}

static void on_statistics_value(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) self;
    struct jsdrv_statistics_s * s = (struct jsdrv_statistics_s *) value->value.bin;
    printf("%s,%" PRId64   ",%g,%g,%g,%g,"   "%g,%g,%g,%g,"   "%g,%g,%g,%g,"  "%g,%g\n",
           topic, s->block_sample_id,
           s->i_avg, s->i_std, s->i_min, s->i_max,
           s->v_avg, s->v_std, s->v_min, s->v_max,
           s->p_avg, s->p_std, s->p_min, s->p_max,
           s->charge_f64, s->energy_f64);
}

static int32_t publish(struct app_s * self, const char * device, const char * topic, const struct jsdrv_union_s * value) {
    char buf[32];
    struct jsdrv_topic_s t;
    jsdrv_topic_set(&t, device);
    jsdrv_topic_append(&t, topic);
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("Publish %s => %s\n", t.topic, buf);
    int32_t rc = jsdrv_publish(self->context, t.topic, value, JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("publish %s failed with %d\n", topic, (int) rc);
    }
    return rc;
}

static int32_t device_initialize(const char * device, void * user_data) {
    struct app_s * self = (struct app_s *) user_data;
    char t[2 * JSDRV_TOPIC_LENGTH_MAX];
    printf("device_open %s\n", device);

    if (jsdrv_cstr_starts_with(device, "u/js220")) {
        ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_DEFAULTS));
        ROE(publish(self, device, "s/i/range/mode", &jsdrv_union_cstr_r("auto")));
        ROE(publish(self, device, "s/stats/ctrl", &jsdrv_union_u8_r(1)));
        snprintf(t, sizeof(t), "%s/s/stats/value", device);
        jsdrv_subscribe(self->context, t, JSDRV_SFLAG_PUB, on_statistics_value, self, JSDRV_TIMEOUT_MS_DEFAULT);
    } else {
        printf("Unsupported device: %s\n", device);
    }
    return 0;
}

int on_statistics(struct app_s * self, int argc, char * argv[]) {
    (void) argv;
    while (argc) {
        return usage();
    }

    ROE(app_scan(self));
    ROE(foreach_device(self->devices, device_initialize, self));

    // display the header
    printf("#device,sampled_id,"
           "i_avg,i_std,i_min,i_max,"
           "v_avg,v_std,v_min,v_max,"
           "p_avg,p_std,p_min,p_max,"
           "charge,energy\n");

    while (!quit_) {
        jsdrv_thread_sleep_ms(1); // do nothing
    }

    return 0;
}


