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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>


const char * JS220_MEM_REGIONS[] = {
        "c/app",
        "c/upd1",
        "c/upd2",
        "c/storage",
        "c/log",
        "c/acfg",
        "c/bcfg",
        "c/pers",
        "s/app1",
        "s/app2",
        "s/cal_t",
        "s/cal_a",
        "s/cal_f",
        "s/pers",
        "s/id",
        NULL
};

bool js220_is_mem_region_valid(const char * region) {
    int idx = 0;
    if (region == NULL || jsdrv_cstr_to_index(region, JS220_MEM_REGIONS, &idx)) {
        printf("Invalid region: %s\n", region);
        printf("The available regions are:\n");
        for (char const ** s = JS220_MEM_REGIONS; *s != NULL; ++s) {
            printf("    %s\n", *s);
        }
        return false;
    }
    return true;
}

static int usage() {
    printf("usage: jsdrv_util mem_read [--device {device_path}] {region} [--size {sz}] [--out {file}]\n");
    return 1;
}

void on_mem_rdata(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    struct app_s * self = (struct app_s *) user_data;
    if (JSDRV_UNION_BIN != value->type) {
        printf("Read data not in BIN format\n");
    } else if (NULL != self->filename) {
        FILE * fh = fopen(self->filename, "wb");
        if (NULL == fh) {
            printf("Could not open output file\n");
        } else {
            fwrite(value->value.bin, value->size, 1, fh);
            fclose(fh);
        }
    } else {
        printf("Read %d bytes\n  %08x: ", (int) value->size, 0);
        for (uint32_t i = 0; i < value->size; ++i) {
            if (i && (0 == (i & 0xf))) {
                printf("\n  %08x: ", i);
            }
            printf("%02x ", value->value.bin[i]);
        }
    }
}

int on_mem_read(struct app_s * self, int argc, char * argv[]) {
    char * device = NULL;
    char * region = NULL;
    uint32_t size = 0;

    while (argc) {
        if (argv[0][0] != '-') {
            if (NULL == region) {
                region = argv[0];
                ARG_CONSUME();
            } else {
                return usage();
            }
        } else if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--size")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u32(argv[0], &size)) {
                printf("Invalid size: %s\n", argv[0]);
                return usage();
            }
            ARG_CONSUME();

        } else if (0 == strcmp(argv[0], "--out")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            self->filename = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (!js220_is_mem_region_valid(region)) {
        return usage();
    }

    ROE(app_match(self, device));

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_OPEN);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(JSDRV_DEVICE_OPEN_MODE_RESUME), JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/mem");
    jsdrv_topic_append(&topic, region);
    jsdrv_topic_append(&topic, "!rdata");
    ROE(jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_mem_rdata, self, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_topic_remove(&topic);
    jsdrv_topic_append(&topic, "!read");
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(size), JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_CLOSE);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
