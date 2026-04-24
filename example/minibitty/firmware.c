/*
 * Copyright 2025-2026 Jetperch LLC
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsdrv/os_event.h"
#include "jsdrv/os_thread.h"


#define PIPELINE_MAX        16U


// Host-side fwup command/response (matches jsdrv_prv/js320_fwup.h)

enum fwup_ctrl_op_e {
    FWUP_CTRL_OP_UPDATE = 1,
    FWUP_CTRL_OP_LAUNCH = 2,
    FWUP_CTRL_OP_ERASE  = 3,
};

struct fwup_ctrl_cmd_s {
    uint32_t transaction_id;
    uint8_t op;
    uint8_t image_slot;
    uint8_t pipeline_depth;
    uint8_t rsv;
    uint8_t data[];
};

struct fwup_rsp_s {
    uint32_t transaction_id;
    int32_t status;
};


struct firmware_s {
    struct app_s * app;
    uint32_t transaction_id;
    int32_t rsp_status;
    jsdrv_os_event_t event;
};

static struct firmware_s fw_;
static int verbose_;


static void on_fwup_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct fwup_rsp_s)) {
        printf("RSP ERROR: invalid response (type=%d, size=%u)\n",
               value->type, value->size);
        fw_.rsp_status = 1;
    } else {
        const struct fwup_rsp_s * rsp = (const struct fwup_rsp_s *) value->value.bin;
        fw_.rsp_status = rsp->status;
        if (verbose_) {
            printf("RSP: transaction_id=%u status=%d\n",
                   rsp->transaction_id, rsp->status);
        }
    }
    jsdrv_os_event_signal(fw_.event);
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
        printf("ERROR: empty or invalid file: %s\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t * data = (uint8_t *) malloc(file_size);
    if (!data) {
        printf("ERROR: could not allocate %ld bytes\n", file_size);
        fclose(f);
        return NULL;
    }
    size_t read_size = fread(data, 1, file_size, f);
    fclose(f);
    if ((long) read_size != file_size) {
        printf("ERROR: read %zu of %ld bytes\n", read_size, file_size);
        free(data);
        return NULL;
    }
    *size_out = (uint32_t) file_size;
    return data;
}

// --- Setup/teardown ---

static int setup(struct app_s * self) {
    struct jsdrv_topic_s topic;

    memset(&fw_, 0, sizeof(fw_));
    fw_.app = self;
    fw_.event = jsdrv_os_event_alloc();

    ROE(jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_RAW, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_thread_sleep_ms(500);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/fwup/ctrl/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_fwup_rsp, NULL, 0);
    return 0;
}

static int teardown(struct app_s * self, int rc) {
    struct jsdrv_topic_s topic;

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/fwup/ctrl/!rsp");
    jsdrv_unsubscribe(self->context, topic.topic, on_fwup_rsp, NULL, 0);

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    jsdrv_os_event_free(fw_.event);
    return rc;
}

// --- Send command and wait for response ---

static int fwup_ctrl_send(uint8_t op, uint8_t image_slot,
                           uint8_t pipeline_depth,
                           const uint8_t * image_data,
                           uint32_t image_size,
                           uint32_t timeout_ms) {
    uint32_t cmd_size = (uint32_t) sizeof(struct fwup_ctrl_cmd_s) + image_size;
    uint8_t * buf = (uint8_t *) malloc(cmd_size);
    if (!buf) {
        printf("ERROR: could not allocate %u bytes\n", cmd_size);
        return 1;
    }

    struct fwup_ctrl_cmd_s * cmd = (struct fwup_ctrl_cmd_s *) buf;
    cmd->transaction_id = ++fw_.transaction_id;
    cmd->op = op;
    cmd->image_slot = image_slot;
    cmd->pipeline_depth = pipeline_depth;
    cmd->rsv = 0;
    if (image_data && image_size > 0) {
        memcpy(cmd->data, image_data, image_size);
    }

    fw_.rsp_status = 1;
    jsdrv_os_event_reset(fw_.event);

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, fw_.app->device.topic);
    jsdrv_topic_append(&topic, "h/fwup/ctrl/!cmd");
    int32_t pub_rc = jsdrv_publish(fw_.app->context, topic.topic,
                                    &jsdrv_union_bin(buf, cmd_size), 0);
    free(buf);
    if (pub_rc) {
        printf("ERROR: jsdrv_publish failed: %d\n", pub_rc);
        return pub_rc;
    }

    int32_t result = jsdrv_os_event_wait(fw_.event, timeout_ms);
    if (result) {
        printf("ERROR: timeout waiting for response\n");
        return 1;
    }
    return fw_.rsp_status;
}

// --- Firmware update ---

static int do_firmware(struct app_s * self, const char * image_path,
                        uint32_t pipeline_depth, uint8_t image_slot) {
    (void) self;
    uint8_t * image = NULL;
    uint32_t image_size = 0;

    image = file_read(image_path, &image_size);
    if (!image) {
        return 1;
    }
    printf("Firmware: %s, %u bytes (pipeline=%u, image=%u)\n",
           image_path, image_size, pipeline_depth, image_slot);

    printf("Updating firmware (allocate, write, verify, commit)...\n");
    int rc = fwup_ctrl_send(FWUP_CTRL_OP_UPDATE, image_slot,
                             (uint8_t) pipeline_depth,
                             image, image_size, 60000);
    free(image);
    if (rc == 0) {
        printf("Firmware update completed successfully\n");
    } else {
        printf("ERROR: firmware update failed (status=%d)\n", rc);
    }
    return rc;
}

// --- Launch ---

static int do_launch(uint8_t image_slot) {
    printf("Sending launch command (image=%u)...\n", image_slot);
    int rc = fwup_ctrl_send(FWUP_CTRL_OP_LAUNCH, image_slot, 0,
                             NULL, 0, 10000);
    if (rc) {
        printf("ERROR: launch failed (status=%d)\n", rc);
    } else {
        printf("Launch command accepted\n");
    }
    return rc;
}

// --- Erase ---

static int do_erase(uint8_t image_slot) {
    printf("Sending erase command (image=%u)...\n", image_slot);
    int rc = fwup_ctrl_send(FWUP_CTRL_OP_ERASE, image_slot, 0,
                             NULL, 0, 10000);
    if (rc) {
        printf("ERROR: erase failed (status=%d)\n", rc);
    } else {
        printf("Erase command accepted\n");
    }
    return rc;
}

// --- Entry point ---

static int usage(void) {
    printf(
        "usage: minibitty firmware <subcommand> [options] [args]\n"
        "\n"
        "Subcommands:\n"
        "  update [-v] [-p N] [-i IMAGE] <path> [device_filter]\n"
        "      Update device firmware from an image file.\n"
        "      -v, --verbose            Show response details\n"
        "      -p, --pipeline N         Pipeline depth for write/verify (default 4, max %u)\n"
        "      -i, --image N            Target image slot (default 0, range 0-1)\n"
        "      path                     Path to .mbfw firmware image file\n"
        "\n"
        "  launch [-v] <image> [device_filter]\n"
        "      Launch a specific image slot.\n"
        "      image                    Image slot (0-3)\n"
        "\n"
        "  erase [-v] <image> [device_filter]\n"
        "      Erase a specific image slot.\n"
        "      image                    Image slot (0-1)\n",
        PIPELINE_MAX
        );
    return 1;
}

static int parse_u32(const char * str, uint32_t * out) {
    char * end = NULL;
    unsigned long val = strtoul(str, &end, 0);
    if (end == str || *end != '\0') {
        return 1;
    }
    *out = (uint32_t) val;
    return 0;
}

static int on_firmware_update(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    int rc;
    uint32_t pipeline_depth = 4;
    uint32_t image_slot = 0;

    while (argc > 0 && argv[0][0] == '-') {
        if (0 == strcmp(argv[0], "-v") || 0 == strcmp(argv[0], "--verbose")) {
            verbose_ = 1;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "-p") || 0 == strcmp(argv[0], "--pipeline")) {
            ARG_CONSUME();
            if (argc < 1 || parse_u32(argv[0], &pipeline_depth)) {
                printf("--pipeline requires a number\n");
                return usage();
            }
            if (pipeline_depth < 1) { pipeline_depth = 1; }
            if (pipeline_depth > PIPELINE_MAX) { pipeline_depth = PIPELINE_MAX; }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "-i") || 0 == strcmp(argv[0], "--image")) {
            ARG_CONSUME();
            if (argc < 1 || parse_u32(argv[0], &image_slot)) {
                printf("--image requires a number\n");
                return usage();
            }
            if (image_slot > 1) {
                printf("--image must be 0 or 1\n");
                return usage();
            }
            ARG_CONSUME();
        } else {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        }
    }

    if (argc < 1) {
        return usage();
    }

    const char * path = argv[0];
    ARG_CONSUME();
    if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }

    ROE(app_match(self, device_filter));
    rc = setup(self);
    if (!rc) {
        rc = do_firmware(self, path, pipeline_depth, (uint8_t) image_slot);
    }
    return teardown(self, rc);
}

static int on_firmware_launch(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    int rc;
    uint32_t image_slot;

    while (argc > 0 && argv[0][0] == '-') {
        if (0 == strcmp(argv[0], "-v") || 0 == strcmp(argv[0], "--verbose")) {
            verbose_ = 1;
            ARG_CONSUME();
        } else {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        }
    }

    if (argc < 1) {
        printf("launch requires an image slot\n");
        return usage();
    }
    if (parse_u32(argv[0], &image_slot) || image_slot > 3) {
        printf("image must be 0-3\n");
        return usage();
    }
    ARG_CONSUME();
    if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }

    ROE(app_match(self, device_filter));
    rc = setup(self);
    if (!rc) {
        rc = do_launch((uint8_t) image_slot);
    }
    return teardown(self, rc);
}

static int on_firmware_erase(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    int rc;
    uint32_t image_slot;

    while (argc > 0 && argv[0][0] == '-') {
        if (0 == strcmp(argv[0], "-v") || 0 == strcmp(argv[0], "--verbose")) {
            verbose_ = 1;
            ARG_CONSUME();
        } else {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        }
    }

    if (argc < 1) {
        printf("erase requires an image slot\n");
        return usage();
    }
    if (parse_u32(argv[0], &image_slot) || image_slot > 1) {
        printf("image must be 0 or 1\n");
        return usage();
    }
    ARG_CONSUME();
    if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }

    ROE(app_match(self, device_filter));
    rc = setup(self);
    if (!rc) {
        rc = do_erase((uint8_t) image_slot);
    }
    return teardown(self, rc);
}

int on_firmware(struct app_s * self, int argc, char * argv[]) {
    verbose_ = 0;

    if (argc < 1) {
        return usage();
    }

    const char * subcmd = argv[0];
    ARG_CONSUME();

    if (0 == strcmp(subcmd, "update")) {
        return on_firmware_update(self, argc, argv);
    } else if (0 == strcmp(subcmd, "launch")) {
        return on_firmware_launch(self, argc, argv);
    } else if (0 == strcmp(subcmd, "erase")) {
        return on_firmware_erase(self, argc, argv);
    } else {
        printf("unknown subcommand: %s\n", subcmd);
        return usage();
    }
}
