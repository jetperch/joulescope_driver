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

#include "jsdrv_util_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/topic.h"
#include "jsdrv_prv/thread.h"
#include <stdio.h>
#include <inttypes.h>


static void on_pub_cmd(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    const char * payload = NULL;
    char buffer[256];
    if (jsdrv_cstr_ends_with(topic, "/!data")) {
        printf("on_pub_data(%s)\n", topic);
        return;
    }

    if (value->app == JSDRV_PAYLOAD_TYPE_UNION) {
        switch (value->type) {
            case JSDRV_UNION_NULL: payload = "[NULL]"; break;
            case JSDRV_UNION_STR:  payload = value->value.str; break;
            case JSDRV_UNION_JSON: payload = value->value.str; break;
            case JSDRV_UNION_BIN:  payload = "[binary]"; break;
            case JSDRV_UNION_F32: break;
            case JSDRV_UNION_F64: break;
            case JSDRV_UNION_U8:  snprintf(buffer, sizeof(buffer), "%" PRIu8,  value->value.u8); break;
            case JSDRV_UNION_U16: snprintf(buffer, sizeof(buffer), "%" PRIu16, value->value.u16); break;
            case JSDRV_UNION_U32: snprintf(buffer, sizeof(buffer), "%" PRIu32, value->value.u32); break;
            case JSDRV_UNION_U64: snprintf(buffer, sizeof(buffer), "%" PRIu64, value->value.u64); break;
            case JSDRV_UNION_I8:  snprintf(buffer, sizeof(buffer), "%" PRId8,  value->value.i8); break;
            case JSDRV_UNION_I16: snprintf(buffer, sizeof(buffer), "%" PRId16, value->value.i16); break;
            case JSDRV_UNION_I32: snprintf(buffer, sizeof(buffer), "%" PRId32, value->value.i32); break;
            case JSDRV_UNION_I64: snprintf(buffer, sizeof(buffer), "%" PRId64, value->value.i64); break;
            default:
                printf("unsupported type for %s: %d", topic, (int) value->type);
                break;
        }
    }
    // JSDRV_LOGI("pub %s => %s", topic, payload ? payload : buffer);
}

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

int on_demo(struct app_s * self, int argc, char * argv[]) {
    ROE(app_match(self, NULL));
    char * device = self->device.topic;
    ROE(publish(self, device, JSDRV_MSG_OPEN, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_subscribe(self->context, device, JSDRV_SFLAG_PUB, on_pub_cmd, self, JSDRV_TIMEOUT_MS_DEFAULT);

    if (jsdrv_cstr_starts_with(device, "u/js220")) {
        ROE(publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("10 A"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/i/range/mode", &jsdrv_union_cstr_r("manual"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/adc/0/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        jsdrv_thread_sleep_ms(1000);
        ROE(publish(self, device, "s/adc/0/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
    } else if (jsdrv_cstr_starts_with(device, "u/js110")) {
        ROE(publish(self, device, "s/i/range/select", &jsdrv_union_cstr_r("auto"), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
        jsdrv_thread_sleep_ms(1000);
        ROE(publish(self, device, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(publish(self, device, "s/v/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
    } else {
        printf("Unsupported device: %s\n", device);
    }

    ROE(publish(self, device, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
