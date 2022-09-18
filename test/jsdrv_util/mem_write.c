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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>


#define WRITE_TIMEOUT_MS (30000)


static int usage() {
    printf("usage: jsdrv_util mem_write [--device {device_path}] [--timeout {timeout_ms}] {region} {file}\n");
    return 1;
}

static int32_t file_read(const char * filename, uint8_t ** data, uint32_t * size) {
    FILE * fh = fopen(filename, "rb");
    if (NULL == fh) {
        printf("Could not open file: %s\n", filename);
        return 1;
    }
    if (fseek(fh, 0, SEEK_END)) {
        printf("Could not seek to end\n");
        return 1;
    }
    long sz = ftell(fh);
    uint8_t * p = malloc(sz);
    if (NULL == p) {
        printf("Could not allocate memory for file data\n");
        return 1;
    }
    if (fseek(fh, 0, SEEK_SET)) {
        printf("Could not seek to beginning\n");
        return 1;
    }

    size_t sz_read = fread(p, 1, sz, fh);
    fclose(fh);

    if (sz_read != (size_t) sz) {
        printf("Could not read entire file: %d != %d\n", (int) sz_read, (int) sz);
        return 1;
    }

    *data = p;
    *size = (uint32_t) sz;
    return 0;
}


int on_mem_write(struct app_s * self, int argc, char * argv[]) {
    char * device = NULL;
    char * region = NULL;
    int npos = 0;
    uint32_t write_timeout_ms = WRITE_TIMEOUT_MS;

    while (argc) {
        if (argv[0][0] != '-') {
            switch (npos) {
                case 0: region = argv[0]; break;
                case 1: self->filename = argv[0]; break;
                default:
                    printf("Too many positional arguments\n");
                    return usage();
            }
            ARG_CONSUME();
            ++npos;
        } else if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--timeout")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u32(argv[0], &write_timeout_ms)) {
                printf("Could not parse timeout\n");
                return usage();
            }
            device = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (!js220_is_mem_region_valid(region) || (NULL == self->filename)) {
        return usage();
    }

    ROE(app_match(self, device));

    uint8_t * data;
    uint32_t data_size;
    if (file_read(self->filename, &data, &data_size)) {
        printf("File read failed\n");
        return usage();
    }

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_OPEN);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(JSDRV_DEVICE_OPEN_MODE_RESUME), JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/mem");
    jsdrv_topic_append(&topic, region);
    jsdrv_topic_append(&topic, "!write");
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_bin(data, data_size), write_timeout_ms));
    free(data);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_CLOSE);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}
