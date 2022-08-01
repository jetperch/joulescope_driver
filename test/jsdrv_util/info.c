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
#include "jsdrv/version.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static void on_meta(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    printf("%s => %s\n", topic, value->value.str);
}

static void on_pub(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s * ) user_data;
    if (self->verbose) {
        printf("  ");
    }
    printf("%-32s ", topic);
    switch (value->type) {
        case JSDRV_UNION_NULL: printf("null"); break;
        case JSDRV_UNION_STR:  printf("str %s", value->value.str); break;
        case JSDRV_UNION_JSON: printf("str %s", value->value.str); break;
        case JSDRV_UNION_BIN:  printf("bin length=%d", value->size); break;
        case JSDRV_UNION_RSV0: printf("rsv0"); break;
        case JSDRV_UNION_RSV1: printf("rsv1"); break;
        case JSDRV_UNION_F32:  printf("f32 %g", (double) value->value.f32); break;
        case JSDRV_UNION_F64:  printf("f64 %g", value->value.f64); break;
        case JSDRV_UNION_U8:   printf("u8  %" PRIu8 , value->value.u8); break;
        case JSDRV_UNION_U16:  printf("u16 %" PRIu16, value->value.u16); break;
        case JSDRV_UNION_U32:
            if (jsdrv_cstr_ends_with(topic, "/version")) {
                char version_str[32];
                jsdrv_version_u32_to_str(value->value.u32, version_str, sizeof(version_str));
                printf("u32 %s", version_str);
            } else {
                printf("u32 %" PRIu32, value->value.u32);
            }
            break;
        case JSDRV_UNION_U64:  printf("u64 %" PRIu64, value->value.u64); break;
        case JSDRV_UNION_I8:   printf("i8  %" PRId8 , value->value.i8); break;
        case JSDRV_UNION_I16:  printf("i16 %" PRId16, value->value.i16); break;
        case JSDRV_UNION_I32:  printf("i32 %" PRId32, value->value.i32); break;
        case JSDRV_UNION_I64:  printf("i64 %" PRId64, value->value.i64); break;
        default: printf("unknown type %d", (int) value->type); break;
    }
    printf("\n");
}

static int device_info(struct app_s * self, const char * device) {
    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME));
    // printf("device_info start");
    if (self->verbose) {
        printf("device: %s\n", device);
        printf("metadata:\n");
        uint8_t flags = JSDRV_SFLAG_METADATA_RSP | JSDRV_SFLAG_RETAIN;
        ROE(jsdrv_subscribe(self->context, device, flags, on_meta, self, JSDRV_TIMEOUT_MS_DEFAULT));
        printf("values:\n");
        ROE(jsdrv_unsubscribe(self->context, device, on_meta, self, JSDRV_TIMEOUT_MS_DEFAULT));
    }

    if (1) {
        uint8_t flags = JSDRV_SFLAG_PUB | JSDRV_SFLAG_RETAIN;
        ROE(jsdrv_subscribe(self->context, device, flags, on_pub, self, JSDRV_TIMEOUT_MS_DEFAULT));
        ROE(jsdrv_unsubscribe(self->context, device, on_pub, self, JSDRV_TIMEOUT_MS_DEFAULT));
    }
    //printf("device_info done");

    return jsdrv_close(self->context, device);
}

static int usage() {
    printf("usage: jsdrv_util info [--verbose] device_path\n");
    return 1;
}

int on_info(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, device_filter));
    return device_info(self, self->device.topic);
}
