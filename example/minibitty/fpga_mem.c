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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>


#define FLASH_PAGE_SIZE     256U
#define FLASH_BLOCK_64K     65536U


enum jtag_mem_op_e {
    JTAG_MEM_OP_OPEN       = 1,
    JTAG_MEM_OP_CLOSE      = 2,
    JTAG_MEM_OP_ERASE_64K  = 3,
    JTAG_MEM_OP_ERASE_4K   = 4,
    JTAG_MEM_OP_WRITE      = 5,
    JTAG_MEM_OP_READ       = 6,
};

struct jtag_mem_s {
    uint32_t transaction_id;
    uint8_t operation;
    uint8_t status;
    uint16_t timeout_ms;
    uint32_t offset;
    uint32_t length;
    uint8_t data[];
};


struct fpga_mem_s {
    struct app_s * app;
    uint32_t transaction_id;
    volatile uint8_t rsp_status;
    volatile uint32_t rsp_offset;
    volatile uint32_t rsp_length;
    uint8_t * read_buf;
    uint32_t read_buf_offset;
    uint32_t read_buf_length;
    HANDLE event;
};

static struct fpga_mem_s fpga_mem_;


static const char * op_name(uint8_t op) {
    switch (op) {
        case JTAG_MEM_OP_OPEN:      return "OPEN";
        case JTAG_MEM_OP_CLOSE:     return "CLOSE";
        case JTAG_MEM_OP_ERASE_64K: return "ERASE_64K";
        case JTAG_MEM_OP_ERASE_4K:  return "ERASE_4K";
        case JTAG_MEM_OP_WRITE:     return "WRITE";
        case JTAG_MEM_OP_READ:      return "READ";
        default:                    return "UNKNOWN";
    }
}

static void on_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const struct jtag_mem_s * rsp = (const struct jtag_mem_s *) value->value.bin;
    if (value->size < sizeof(struct jtag_mem_s)) {
        printf("RSP ERROR: too short: %u bytes (need %u)\n", value->size, (uint32_t) sizeof(struct jtag_mem_s));
        return;
    }

    uint32_t data_size = value->size - (uint32_t) sizeof(struct jtag_mem_s);
    printf("RSP: op=%s status=%d offset=0x%06X length=%u value_size=%u data_size=%u\n",
           op_name(rsp->operation), rsp->status, rsp->offset, rsp->length, value->size, data_size);
    uint32_t raw_dump = (value->size > 32) ? 32 : value->size;
    printf("  RAW[%u]:", value->size);
    for (uint32_t i = 0; i < raw_dump; ++i) {
        printf(" %02X", ((const uint8_t *) value->value.bin)[i]);
    }
    printf("\n");

    if (rsp->operation == JTAG_MEM_OP_READ && data_size > 0) {
        uint32_t dump = (data_size > 16) ? 16 : data_size;
        printf("  READ data[0:%u]:", dump);
        for (uint32_t i = 0; i < dump; ++i) {
            printf(" %02X", rsp->data[i]);
        }
        printf("\n");
    }

    fpga_mem_.rsp_status = rsp->status;
    fpga_mem_.rsp_offset = rsp->offset;
    fpga_mem_.rsp_length = rsp->length;

    if (rsp->operation == JTAG_MEM_OP_READ) {
        if (fpga_mem_.read_buf && data_size > 0) {
            uint32_t buf_offset = rsp->offset - fpga_mem_.read_buf_offset;
            if (buf_offset + data_size <= fpga_mem_.read_buf_length) {
                memcpy(fpga_mem_.read_buf + buf_offset, rsp->data, data_size);
            } else {
                printf("  READ buf_offset=%u + data_size=%u > buf_length=%u, skipped copy\n",
                       buf_offset, data_size, fpga_mem_.read_buf_length);
            }
        } else {
            printf("  READ no buffer (read_buf=%p data_size=%u)\n",
                   (void *) fpga_mem_.read_buf, data_size);
        }
    }
    SetEvent(fpga_mem_.event);
}

static int jtag_mem_cmd(struct fpga_mem_s * fm, uint8_t op, uint32_t offset, uint32_t length,
                        const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    struct jsdrv_topic_s topic;
    uint8_t buf[sizeof(struct jtag_mem_s) + FLASH_PAGE_SIZE];
    struct jtag_mem_s * cmd = (struct jtag_mem_s *) buf;
    uint32_t msg_size = sizeof(struct jtag_mem_s) + data_size;

    cmd->transaction_id = ++fm->transaction_id;
    cmd->operation = op;
    cmd->status = 0;
    cmd->timeout_ms = (uint16_t) timeout_ms;
    cmd->offset = offset;
    cmd->length = length;
    if (data && data_size > 0) {
        memcpy(cmd->data, data, data_size);
    }

    ResetEvent(fm->event);
    jsdrv_topic_set(&topic, fm->app->device.topic);
    jsdrv_topic_append(&topic, "c/jtag/mem/!cmd");
    int32_t pub_rc = jsdrv_publish(fm->app->context, topic.topic, &jsdrv_union_bin(buf, msg_size), 0);
    if (pub_rc) {
        printf("ERROR: jsdrv_publish failed: %d\n", pub_rc);
        return pub_rc;
    }

    DWORD wait_ms = timeout_ms + 5000;
    DWORD result = WaitForSingleObject(fm->event, wait_ms);
    if (result != WAIT_OBJECT_0) {
        printf("ERROR: timeout waiting for response\n");
        return 1;
    }
    if (fm->rsp_status != 0) {
        printf("ERROR: operation %d failed with status %d\n", op, fm->rsp_status);
        return 1;
    }
    return 0;
}

static int jtag_mem_read(struct fpga_mem_s * fm, uint32_t offset, uint8_t * buf, uint32_t length) {
    fm->read_buf = buf;
    fm->read_buf_offset = offset;
    fm->read_buf_length = length;

    uint32_t remaining = length;
    uint32_t addr = offset;
    while (remaining > 0) {
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        int rc = jtag_mem_cmd(fm, JTAG_MEM_OP_READ, addr, chunk, NULL, 0, 1000);
        if (rc) {
            fm->read_buf = NULL;
            return rc;
        }
        addr += chunk;
        remaining -= chunk;
    }
    fm->read_buf = NULL;
    return 0;
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

static int run(struct app_s * self, const char * image_path) {
    struct jsdrv_topic_s topic;
    uint8_t * image = NULL;
    uint32_t image_size = 0;
    int rc = 0;

    printf("sizeof(jtag_mem_s) = %u\n", (uint32_t) sizeof(struct jtag_mem_s));

    image = file_read(image_path, &image_size);
    if (!image) {
        return 1;
    }
    printf("Image: %s, %u bytes\n", image_path, image_size);
    printf("Image first 16 bytes:");
    for (uint32_t i = 0; i < 16 && i < image_size; ++i) {
        printf(" %02X", image[i]);
    }
    printf("\n");

    memset(&fpga_mem_, 0, sizeof(fpga_mem_));
    fpga_mem_.app = self;
    fpga_mem_.event = CreateEvent(NULL, TRUE, FALSE, NULL);

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(500);  // todo replace with controlled interlock when available

    // Activate JTAG mode
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/mode");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(2), 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/jtag/mem/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_rsp, NULL, 0);

    // OPEN
    printf("Opening JTAG...\n");
    rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_OPEN, 0, 0, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: JTAG open failed\n");
        goto cleanup;
    }
    printf("JTAG opened\n");

    // ERASE (limited to 1 block for debugging)
    uint32_t num_blocks = 1; // (image_size + FLASH_BLOCK_64K - 1) / FLASH_BLOCK_64K;
    printf("Erasing %u blocks (%u bytes)...\n", num_blocks, num_blocks * FLASH_BLOCK_64K);
    for (uint32_t block = 0; block < num_blocks; ++block) {
        if (quit_) {
            rc = 1;
            goto close_jtag;
        }
        uint32_t offset = block * FLASH_BLOCK_64K;
        printf("  Erase block %u/%u (offset 0x%06X)\n", block + 1, num_blocks, offset);
        rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_ERASE_64K, offset, 0, NULL, 0, 5000);
        if (rc) {
            printf("ERROR: erase failed at offset 0x%06X\n", offset);
            goto close_jtag;
        }
    }
    printf("Erase complete\n");

    // Read-after-erase check: read just 16 bytes to minimize output
    {
        uint8_t erase_check[16];
        memset(erase_check, 0, sizeof(erase_check));
        printf("Read-after-erase check (16 bytes)...\n");
        rc = jtag_mem_read(&fpga_mem_, 0, erase_check, 16);
        if (rc) {
            printf("ERROR: read-after-erase failed\n");
            goto close_jtag;
        }
        printf("  Erase check bytes:");
        for (uint32_t i = 0; i < 16; ++i) {
            printf(" %02X", erase_check[i]);
        }
        printf("\n");
    }

    // DEBUG: stop here to analyze erase/read results
    printf("DEBUG: stopping after erase/read check\n");
    goto close_jtag;

    // WRITE
    uint32_t num_pages = (image_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    printf("Writing %u pages (%u bytes)...\n", num_pages, image_size);
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) {
            rc = 1;
            goto close_jtag;
        }
        uint32_t offset = page * FLASH_PAGE_SIZE;
        uint32_t remaining = image_size - offset;
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        if ((page % 64) == 0) {
            printf("  Write page %u/%u (offset 0x%06X)\n", page + 1, num_pages, offset);
        }
        rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_WRITE, offset, chunk, image + offset, chunk, 1000);
        if (rc) {
            printf("ERROR: write failed at offset 0x%06X\n", offset);
            goto close_jtag;
        }
    }
    printf("Write complete\n");

    // READ + VERIFY
    printf("Verifying %u bytes...\n", image_size);
    uint8_t * verify_buf = (uint8_t *) malloc(FLASH_PAGE_SIZE);
    if (!verify_buf) {
        printf("ERROR: could not allocate verify buffer\n");
        rc = 1;
        goto close_jtag;
    }
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) {
            rc = 1;
            free(verify_buf);
            goto close_jtag;
        }
        uint32_t offset = page * FLASH_PAGE_SIZE;
        uint32_t remaining = image_size - offset;
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        if ((page % 64) == 0) {
            printf("  Verify page %u/%u (offset 0x%06X)\n", page + 1, num_pages, offset);
        }
        rc = jtag_mem_read(&fpga_mem_, offset, verify_buf, chunk);
        if (rc) {
            printf("ERROR: read failed at offset 0x%06X\n", offset);
            free(verify_buf);
            goto close_jtag;
        }
        if (memcmp(verify_buf, image + offset, chunk) != 0) {
            printf("ERROR: verify mismatch at offset 0x%06X\n", offset);
            uint32_t dump = (chunk > 32) ? 32 : chunk;
            printf("  Expected:");
            for (uint32_t i = 0; i < dump; ++i) {
                printf(" %02X", image[offset + i]);
            }
            printf("\n  Got:     ");
            for (uint32_t i = 0; i < dump; ++i) {
                printf(" %02X", verify_buf[i]);
            }
            printf("\n");
            rc = 1;
            free(verify_buf);
            goto close_jtag;
        }
    }
    free(verify_buf);
    printf("Verify complete\n");

close_jtag:
    printf("Closing JTAG...\n");
    jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_CLOSE, 0, 0, NULL, 0, 5000);

cleanup:
    // Restore normal operation
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/mode");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(0), 0);

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    CloseHandle(fpga_mem_.event);
    free(image);

    if (rc == 0) {
        printf("FPGA flash programmed successfully\n");
    }
    return rc;
}

static int usage(void) {
    printf(
        "usage: minibitty fpga_mem <image_file> [device_filter]\n"
        "\n"
        "Program the FPGA flash memory via JTAG.\n"
        "Erases, writes, and verifies the image.\n"
        );
    return 1;
}

int on_fpga_mem(struct app_s * self, int argc, char * argv[]) {
    char * image_path = NULL;
    char * device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            if (!image_path) {
                image_path = argv[0];
            } else {
                device_filter = argv[0];
            }
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (!image_path) {
        printf("image_file required\n");
        return usage();
    }

    ROE(app_match(self, device_filter));
    return run(self, image_path);
}
