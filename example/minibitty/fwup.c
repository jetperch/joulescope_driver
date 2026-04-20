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
#include "jsdrv_prv/devices/js320/js320_fwup_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static volatile bool fwup_done_ = false;
static volatile int32_t fwup_rc_ = 0;

static int usage(void) {
    printf(
        "usage: minibitty fwup <package.zip> [device_filter]\n"
        "\n"
        "Perform full JS320 firmware update from manufacturing package.\n"
        "\n"
        "Options:\n"
        "  --skip-ctrl       Skip ctrl firmware update\n"
        "  --skip-fpga       Skip FPGA bitstream programming\n"
        "  --skip-resources  Skip metadata/trace resource writes\n"
    );
    return 1;
}

static uint8_t * file_read(const char * path, uint32_t * size_out) {
    FILE * f = fopen(path, "rb");
    if (!f) {
        printf("ERROR: could not open file: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t * data = (uint8_t *) malloc(file_size);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(data, 1, file_size, f);
    fclose(f);
    if ((long) n != file_size) {
        free(data);
        return NULL;
    }
    *size_out = (uint32_t) file_size;
    return data;
}

static void on_status(void * user_data, const char * topic,
                      const struct jsdrv_union_s * value) {
    (void) user_data;
    if (value->type != JSDRV_UNION_STR) {
        return;
    }
    // Only process status topics (fwup/NNN/status), not list etc.
    if (!strstr(topic, "/status")) {
        return;
    }
    printf("  [%s] %s\n", topic, value->value.str);

    if (strstr(value->value.str, "\"state\":\"DONE\"")) {
        fwup_rc_ = 0;
        fwup_done_ = true;
    } else if (strstr(value->value.str, "\"state\":\"ERROR\"")) {
        fwup_rc_ = 1;
        fwup_done_ = true;
    }
}

int on_fwup(struct app_s * self, int argc, char * argv[]) {
    uint32_t flags = 0;
    const char * zip_path = NULL;
    char * device_filter = NULL;

    while (argc) {
        if (0 == strcmp(argv[0], "--skip-ctrl")) {
            flags |= JSDRV_FWUP_FLAG_SKIP_CTRL;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--skip-fpga")) {
            flags |= JSDRV_FWUP_FLAG_SKIP_FPGA;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--skip-resources")) {
            flags |= JSDRV_FWUP_FLAG_SKIP_RESOURCES;
            ARG_CONSUME();
        } else if (argv[0][0] == '-') {
            printf("Unknown option: %s\n", argv[0]);
            return usage();
        } else if (!zip_path) {
            zip_path = argv[0];
            ARG_CONSUME();
        } else {
            device_filter = argv[0];
            ARG_CONSUME();
        }
    }

    if (!zip_path) {
        printf("ERROR: package.zip path is required.\n");
        return usage();
    }

    ROE(app_match(self, device_filter));
    printf("Target device: %s\n", self->device.topic);

    // Read ZIP
    uint32_t zip_size = 0;
    uint8_t * zip_data = file_read(zip_path, &zip_size);
    if (!zip_data) {
        printf("ERROR: could not read %s\n", zip_path);
        return 1;
    }
    printf("Package: %s (%u bytes)\n", zip_path, zip_size);

    // Subscribe to status updates from all fwup instances
    jsdrv_subscribe(self->context, "fwup/", JSDRV_SFLAG_PUB,
                    on_status, self, 0);

    // Build payload: header + ZIP data
    uint32_t hdr_size = sizeof(struct jsdrv_fwup_add_header_s);
    uint32_t payload_size = hdr_size + zip_size;
    uint8_t * payload = (uint8_t *) malloc(payload_size);
    if (!payload) {
        free(zip_data);
        printf("ERROR: malloc failed\n");
        return 1;
    }

    struct jsdrv_fwup_add_header_s * hdr =
        (struct jsdrv_fwup_add_header_s *) payload;
    memset(hdr, 0, hdr_size);
    jsdrv_cstr_copy(hdr->device_prefix, self->device.topic,
                    sizeof(hdr->device_prefix));
    hdr->flags = flags;
    hdr->zip_size = zip_size;
    memcpy(payload + hdr_size, zip_data, zip_size);
    free(zip_data);

    // Publish to fwup/@/!add — the manager handles it
    fwup_done_ = false;
    fwup_rc_ = 0;
    printf("Starting firmware update...\n");
    int32_t rc = jsdrv_publish(self->context, JSDRV_FWUP_MGR_TOPIC_ADD,
                               &jsdrv_union_bin(payload, payload_size),
                               10000);
    free(payload);
    if (rc) {
        printf("ERROR: fwup add failed: %d\n", rc);
        jsdrv_unsubscribe(self->context, "fwup/", on_status, self, 0);
        return rc;
    }

    // Wait for completion
    while (!quit_ && !fwup_done_) {
        jsdrv_thread_sleep_ms(100);
    }

    jsdrv_unsubscribe(self->context, "fwup/", on_status, self, 0);

    if (fwup_done_) {
        printf("\nFirmware update %s.\n",
               fwup_rc_ ? "FAILED" : "completed successfully");
    } else {
        printf("\nInterrupted.\n");
        fwup_rc_ = 1;
    }
    return fwup_rc_;
}
