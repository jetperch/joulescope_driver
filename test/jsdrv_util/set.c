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
#include "jsdrv/topic.h"
#include "jsdrv/cstr.h"
#include "jsdrv/meta.h"
#include "jsdrv/error_code.h"
#include <stdio.h>
#include <string.h>

static int usage() {
    printf("usage: jsdrv_util set device_path \"topic=value\" ...\n");
    return 1;
}

#define VALIDATE(topic__, rc__)  if (!(rc__)) {printf("%s invalid\n", (topic__)); return 1; }

static int set_arg(struct app_s * self, char * device_path, char * arg) {
    struct jsdrv_topic_s topic;
    char metadata[4096];
    char * value = arg;
    while (1) {
        if (!*value) {
            return 1;
        } else if (*value == '=') {
            *value++ = 0;
            break;
        }
        value++;
    }

    if (jsdrv_cstr_starts_with(arg, device_path)) {
        jsdrv_topic_set(&topic, arg);
    } else {
        jsdrv_topic_set(&topic, device_path);
        jsdrv_topic_append(&topic, arg);
    }
    printf("%s => %s\n", topic.topic, value);

    jsdrv_topic_suffix_add(&topic, JSDRV_TOPIC_SUFFIX_METADATA_REQ);
    struct jsdrv_union_s v = jsdrv_union_str(metadata);
    v.size = (uint32_t) sizeof(metadata);
    ROE(jsdrv_query(self->context, topic.topic, &v, JSDRV_TIMEOUT_MS_DEFAULT));
    if (self->verbose) {
        printf("metadata = %s\n", metadata);
    }

    v = jsdrv_union_u64_r(0);
    ROE(jsdrv_meta_dtype(metadata, &v.type));
    switch (v.type) {
        case JSDRV_UNION_NULL: break;
        case JSDRV_UNION_STR:  /** intentional fall-through */
        case JSDRV_UNION_JSON:
            v = jsdrv_union_str(value);
            v.flags = 0;
            break;
        case JSDRV_UNION_BIN:  return JSDRV_ERROR_NOT_SUPPORTED;
        case JSDRV_UNION_RSV0: return JSDRV_ERROR_NOT_SUPPORTED;
        case JSDRV_UNION_RSV1: return JSDRV_ERROR_NOT_SUPPORTED;
        case JSDRV_UNION_F32:  return JSDRV_ERROR_NOT_SUPPORTED;
        case JSDRV_UNION_F64:  return JSDRV_ERROR_NOT_SUPPORTED;
        case JSDRV_UNION_U8:
            VALIDATE(arg, 0 == jsdrv_cstr_to_u32(value, &v.value.u32));
            VALIDATE(arg, v.value.u32 <= UINT8_MAX);
            break;
        case JSDRV_UNION_U16:
            VALIDATE(arg, 0 == jsdrv_cstr_to_u32(value, &v.value.u32));
            VALIDATE(arg, v.value.u32 <= UINT16_MAX);
            break;
        case JSDRV_UNION_U32:
            VALIDATE(arg, 0 == jsdrv_cstr_to_u32(value, &v.value.u32));
            break;
        case JSDRV_UNION_U64:
            VALIDATE(arg, 0 == jsdrv_cstr_to_u32(value, &v.value.u32));
            break;
        case JSDRV_UNION_I8:
            VALIDATE(arg, 0 == jsdrv_cstr_to_i32(value, &v.value.i32));
            VALIDATE(arg, (v.value.i32 >= INT8_MIN) && (v.value.i32 <= INT8_MAX));
            break;
        case JSDRV_UNION_I16:
            VALIDATE(arg, 0 == jsdrv_cstr_to_i32(value, &v.value.i32));
            VALIDATE(arg, (v.value.i32 >= INT16_MIN) && (v.value.i32 <= INT16_MAX));
            break;
        case JSDRV_UNION_I32:
            VALIDATE(arg, 0 == jsdrv_cstr_to_i32(value, &v.value.i32));
            break;
        case JSDRV_UNION_I64:
            VALIDATE(arg, 0 == jsdrv_cstr_to_i32(value, &v.value.i32));
            break;
        default:
            return JSDRV_ERROR_NOT_SUPPORTED;
    }

    jsdrv_topic_suffix_remove(&topic);
    int32_t rc = jsdrv_publish(self->context, topic.topic, &v, JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("publish failed with %d : %s\n", rc, jsdrv_error_code_description(rc));
    }
    return rc;
}

int on_set(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    while (argc) {
        if (argv[0][0] == '-') {
            // no options at this time
            return usage();
        } else if (NULL == device_filter) {
            device_filter = argv[0];
            if (strchr(device_filter, '=')) {
                return usage();
            }
            ROE(app_match(self, device_filter));
            ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME));
            ARG_CONSUME();
        } else {
            if (set_arg(self, self->device.topic, argv[0])) {
                return usage();
            }
            ARG_CONSUME();
        }
    }

    if (self->device.topic[0]) {
        jsdrv_close(self->context, self->device.topic);
    }

    return 0;
}
