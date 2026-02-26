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

#include <windows.h>


#define FW_PAGE_SIZE        256U
#define FW_CMD_HEADER_SIZE  16U
#define PIPELINE_MAX        16U


enum fw_cmd_op_e {
    FW_CMD_OP_ALLOCATE = 1,
    FW_CMD_OP_WRITE    = 2,
    FW_CMD_OP_READ     = 3,
    FW_CMD_OP_UPDATE   = 4,
    FW_CMD_OP_LAUNCH   = 5,
    FW_CMD_OP_ERASE    = 6,
};

struct fw_cmd_s {
    uint32_t id;
    uint8_t op;
    uint8_t status;
    uint8_t image;
    uint8_t rsv;
    uint32_t offset;
    uint32_t length;
};


struct firmware_s {
    struct app_s * app;
    uint32_t transaction_id;
    volatile LONG outstanding;
    volatile LONG error_count;
    uint8_t * read_buf;
    uint32_t read_buf_offset;
    uint32_t read_buf_length;
    HANDLE semaphore;
    uint32_t pipeline_depth;
};

static struct firmware_s fw_;
static int verbose_;


static const char * op_name(uint8_t op) {
    switch (op) {
        case FW_CMD_OP_ALLOCATE: return "ALLOCATE";
        case FW_CMD_OP_WRITE:    return "WRITE";
        case FW_CMD_OP_READ:     return "READ";
        case FW_CMD_OP_UPDATE:   return "UPDATE";
        case FW_CMD_OP_LAUNCH:   return "LAUNCH";
        case FW_CMD_OP_ERASE:    return "ERASE";
        default:                 return "UNKNOWN";
    }
}

static void on_fw_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const struct fw_cmd_s * rsp = (const struct fw_cmd_s *) value->value.bin;
    if (value->size < FW_CMD_HEADER_SIZE) {
        printf("RSP ERROR: too short: %u bytes (need %u)\n", value->size, FW_CMD_HEADER_SIZE);
        return;
    }

    uint32_t data_size = value->size - FW_CMD_HEADER_SIZE;
    if (verbose_) {
        printf("RSP: op=%s status=%d offset=0x%06X length=%u value_size=%u data_size=%u\n",
               op_name(rsp->op), rsp->status, rsp->offset, rsp->length, value->size, data_size);
        uint32_t raw_dump = (value->size > 32) ? 32 : value->size;
        printf("  RAW[%u]:", value->size);
        for (uint32_t i = 0; i < raw_dump; ++i) {
            printf(" %02X", ((const uint8_t *) value->value.bin)[i]);
        }
        printf("\n");

        if (rsp->op == FW_CMD_OP_READ && data_size > 0) {
            const uint8_t * rsp_data = ((const uint8_t *) value->value.bin) + FW_CMD_HEADER_SIZE;
            uint32_t dump = (data_size > 16) ? 16 : data_size;
            printf("  READ data[0:%u]:", dump);
            for (uint32_t i = 0; i < dump; ++i) {
                printf(" %02X", rsp_data[i]);
            }
            printf("\n");
        }
    }

    if (rsp->op == FW_CMD_OP_READ) {
        const uint8_t * rsp_data = ((const uint8_t *) value->value.bin) + FW_CMD_HEADER_SIZE;
        if (fw_.read_buf && data_size > 0) {
            uint32_t buf_offset = rsp->offset - fw_.read_buf_offset;
            if (buf_offset + data_size <= fw_.read_buf_length) {
                memcpy(fw_.read_buf + buf_offset, rsp_data, data_size);
            } else {
                printf("  READ buf_offset=%u + data_size=%u > buf_length=%u, skipped copy\n",
                       buf_offset, data_size, fw_.read_buf_length);
            }
        } else {
            printf("  READ no buffer (read_buf=%p data_size=%u)\n",
                   (void *) fw_.read_buf, data_size);
        }
    }

    if (rsp->status != 0) {
        InterlockedIncrement(&fw_.error_count);
    }
    InterlockedDecrement(&fw_.outstanding);
    ReleaseSemaphore(fw_.semaphore, 1, NULL);
}

static int pipeline_send(struct firmware_s * fm, uint8_t op, uint8_t image,
                          uint32_t offset, uint32_t length,
                          const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    while (fm->outstanding >= (LONG) fm->pipeline_depth) {
        DWORD result = WaitForSingleObject(fm->semaphore, timeout_ms + 5000);
        if (result != WAIT_OBJECT_0) {
            printf("ERROR: timeout waiting for pipeline slot\n");
            return 1;
        }
    }
    if (fm->error_count > 0) {
        return 1;
    }

    struct jsdrv_topic_s topic;
    uint8_t buf[FW_CMD_HEADER_SIZE + FW_PAGE_SIZE];
    struct fw_cmd_s * cmd = (struct fw_cmd_s *) buf;
    uint32_t msg_size = FW_CMD_HEADER_SIZE + data_size;

    cmd->id = ++fm->transaction_id;
    cmd->op = op;
    cmd->status = 0;
    cmd->image = image;
    cmd->rsv = 0;
    cmd->offset = offset;
    cmd->length = length;
    if (data && data_size > 0) {
        memcpy(buf + FW_CMD_HEADER_SIZE, data, data_size);
    }

    InterlockedIncrement(&fm->outstanding);
    jsdrv_topic_set(&topic, fm->app->device.topic);
    jsdrv_topic_append(&topic, "c/fwup/!cmd");
    int32_t pub_rc = jsdrv_publish(fm->app->context, topic.topic, &jsdrv_union_bin(buf, msg_size), 0);
    if (pub_rc) {
        InterlockedDecrement(&fm->outstanding);
        printf("ERROR: jsdrv_publish failed: %d\n", pub_rc);
        return pub_rc;
    }
    return 0;
}

static int pipeline_drain(struct firmware_s * fm, uint32_t timeout_ms) {
    while (fm->outstanding > 0) {
        DWORD result = WaitForSingleObject(fm->semaphore, timeout_ms + 5000);
        if (result != WAIT_OBJECT_0) {
            printf("ERROR: timeout waiting for pipeline drain (%ld outstanding)\n", fm->outstanding);
            return 1;
        }
    }
    if (fm->error_count > 0) {
        printf("ERROR: %ld commands failed during pipeline\n", fm->error_count);
        return 1;
    }
    return 0;
}

static int fw_cmd(struct firmware_s * fm, uint8_t op, uint8_t image,
                   uint32_t offset, uint32_t length,
                   const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    fm->error_count = 0;
    int rc = pipeline_send(fm, op, image, offset, length, data, data_size, timeout_ms);
    if (rc) return rc;
    return pipeline_drain(fm, timeout_ms);
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
    fw_.semaphore = CreateSemaphore(NULL, 0, PIPELINE_MAX, NULL);
    fw_.pipeline_depth = 1;

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(500);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/fwup/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_fw_rsp, NULL, 0);

    return 0;
}

static int teardown(struct app_s * self, int rc) {
    struct jsdrv_topic_s topic;

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/fwup/!rsp");
    jsdrv_unsubscribe(self->context, topic.topic, on_fw_rsp, NULL, 0);

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    CloseHandle(fw_.semaphore);
    return rc;
}

// --- Firmware update ---

static int do_firmware(struct app_s * self, const char * image_path,
                        uint32_t pipeline_depth, uint8_t image_slot) {
    (void) self;
    uint8_t * image = NULL;
    uint32_t image_size = 0;
    int rc = 0;

    image = file_read(image_path, &image_size);
    if (!image) {
        return 1;
    }
    printf("Firmware: %s, %u bytes (pipeline=%u, image=%u)\n",
           image_path, image_size, pipeline_depth, image_slot);
    if (verbose_) {
        printf("Image first 16 bytes:");
        for (uint32_t i = 0; i < 16 && i < image_size; ++i) {
            printf(" %02X", image[i]);
        }
        printf("\n");
    }

    // ALLOCATE
    printf("Allocating staging buffer (%u bytes)...\n", image_size);
    rc = fw_cmd(&fw_, FW_CMD_OP_ALLOCATE, 0, 0, image_size, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: allocate failed\n");
        goto done;
    }
    printf("Allocate complete\n");

    // WRITE (pipelined)
    uint32_t num_pages = (image_size + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;
    printf("Writing %u pages (%u bytes)...\n", num_pages, image_size);
    fw_.pipeline_depth = pipeline_depth;
    fw_.error_count = 0;
    uint8_t page_buf[FW_PAGE_SIZE];
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) { rc = 1; goto write_drain; }
        uint32_t offset = page * FW_PAGE_SIZE;
        uint32_t remaining = image_size - offset;
        uint32_t chunk = (remaining > FW_PAGE_SIZE) ? FW_PAGE_SIZE : remaining;
        const uint8_t * src;
        if (chunk < FW_PAGE_SIZE) {
            memcpy(page_buf, image + offset, chunk);
            memset(page_buf + chunk, 0xFF, FW_PAGE_SIZE - chunk);
            src = page_buf;
        } else {
            src = image + offset;
        }
        if (verbose_ && ((page % 64) == 0)) {
            printf("  Write page %u/%u (offset 0x%06X)\n", page + 1, num_pages, offset);
        }
        rc = pipeline_send(&fw_, FW_CMD_OP_WRITE, 0, offset, FW_PAGE_SIZE, src, FW_PAGE_SIZE, 1000);
        if (rc) {
            printf("ERROR: write failed at offset 0x%06X\n", offset);
            goto write_drain;
        }
    }
write_drain:
    if (!rc) {
        rc = pipeline_drain(&fw_, 1000);
    } else {
        pipeline_drain(&fw_, 1000);
    }
    fw_.pipeline_depth = 1;
    if (rc) { goto done; }
    printf("Write complete\n");

    // READ + VERIFY (pipelined in batches)
    printf("Verifying %u bytes...\n", image_size);
    uint32_t batch = pipeline_depth;
    uint32_t batch_bytes = batch * FW_PAGE_SIZE;
    uint8_t * verify_buf = (uint8_t *) malloc(batch_bytes);
    uint8_t * expect_buf = (uint8_t *) malloc(batch_bytes);
    if (!verify_buf || !expect_buf) {
        printf("ERROR: could not allocate verify buffers\n");
        free(verify_buf);
        free(expect_buf);
        rc = 1;
        goto done;
    }
    for (uint32_t base = 0; base < num_pages; base += batch) {
        if (quit_) { rc = 1; break; }
        uint32_t count = num_pages - base;
        if (count > batch) { count = batch; }
        uint32_t base_offset = base * FW_PAGE_SIZE;

        // Build expected data for this batch
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t p = base + i;
            uint32_t offset = p * FW_PAGE_SIZE;
            uint32_t remaining = image_size - offset;
            uint32_t chunk = (remaining > FW_PAGE_SIZE) ? FW_PAGE_SIZE : remaining;
            memcpy(expect_buf + i * FW_PAGE_SIZE, image + offset, chunk);
            if (chunk < FW_PAGE_SIZE) {
                memset(expect_buf + i * FW_PAGE_SIZE + chunk, 0xFF, FW_PAGE_SIZE - chunk);
            }
        }

        // Set up read buffer and send pipelined reads
        fw_.read_buf = verify_buf;
        fw_.read_buf_offset = base_offset;
        fw_.read_buf_length = count * FW_PAGE_SIZE;
        fw_.pipeline_depth = pipeline_depth;
        fw_.error_count = 0;
        memset(verify_buf, 0, count * FW_PAGE_SIZE);

        if (verbose_ && ((base % 64) == 0)) {
            printf("  Verify page %u/%u (offset 0x%06X)\n", base + 1, num_pages, base_offset);
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t offset = (base + i) * FW_PAGE_SIZE;
            rc = pipeline_send(&fw_, FW_CMD_OP_READ, 0, offset, FW_PAGE_SIZE, NULL, 0, 1000);
            if (rc) {
                printf("ERROR: read send failed at offset 0x%06X\n", offset);
                break;
            }
        }
        if (!rc) {
            rc = pipeline_drain(&fw_, 1000);
        } else {
            pipeline_drain(&fw_, 1000);
        }
        fw_.read_buf = NULL;
        fw_.pipeline_depth = 1;
        if (rc) { break; }

        // Compare this batch
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t buf_off = i * FW_PAGE_SIZE;
            if (memcmp(verify_buf + buf_off, expect_buf + buf_off, FW_PAGE_SIZE) != 0) {
                uint32_t offset = (base + i) * FW_PAGE_SIZE;
                printf("ERROR: verify mismatch at offset 0x%06X\n", offset);
                printf("  Expected:");
                for (uint32_t j = 0; j < 32; ++j) {
                    printf(" %02X", expect_buf[buf_off + j]);
                }
                printf("\n  Got:     ");
                for (uint32_t j = 0; j < 32; ++j) {
                    printf(" %02X", verify_buf[buf_off + j]);
                }
                printf("\n");
                rc = 1;
                break;
            }
        }
        if (rc) { break; }
    }
    free(verify_buf);
    free(expect_buf);
    if (!rc) {
        printf("Verify complete\n");
    }
    if (rc) { goto done; }

    // UPDATE
    printf("Sending update command (image=%u)...\n", image_slot);
    rc = fw_cmd(&fw_, FW_CMD_OP_UPDATE, image_slot, 0, 0, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: update command failed\n");
        goto done;
    }
    printf("Update command accepted, device will reset in ~100 ms\n");
    Sleep(200);

done:
    free(image);
    if (rc == 0) {
        printf("Firmware update initiated successfully\n");
    }
    return rc;
}

// --- Launch ---

static int do_launch(uint8_t image_slot) {
    printf("Sending launch command (image=%u)...\n", image_slot);
    int rc = fw_cmd(&fw_, FW_CMD_OP_LAUNCH, image_slot, 0, 0, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: launch command failed\n");
        return rc;
    }
    printf("Launch command accepted, device will reset in ~100 ms\n");
    Sleep(200);
    return 0;
}

// --- Erase ---

static int do_erase(uint8_t image_slot) {
    printf("Sending erase command (image=%u)...\n", image_slot);
    int rc = fw_cmd(&fw_, FW_CMD_OP_ERASE, image_slot, 0, 0, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: erase command failed\n");
        return rc;
    }
    printf("Erase command accepted, device will reset in ~100 ms\n");
    Sleep(200);
    return 0;
}

// --- Entry point ---

static int usage(void) {
    printf(
        "usage: minibitty firmware <subcommand> [options] [args]\n"
        "\n"
        "Subcommands:\n"
        "  update [-v] [-p N] [-i IMAGE] <path> [device_filter]\n"
        "      Update device firmware from an image file.\n"
        "      -v, --verbose            Show per-transaction details\n"
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
