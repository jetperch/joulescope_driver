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
#define PIPELINE_MAX        16U


enum jtag_mem_op_e {
    JTAG_MEM_OP_OPEN       = 1,
    JTAG_MEM_OP_CLOSE      = 2,
    JTAG_MEM_OP_ERASE_64K  = 3,
    JTAG_MEM_OP_ERASE_4K   = 4,
    JTAG_MEM_OP_WRITE      = 5,
    JTAG_MEM_OP_READ       = 6,
    JTAG_MEM_OP_PROG_AES_KEY = 7,
    JTAG_MEM_OP_READ_UID   = 8,
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
    volatile LONG outstanding;
    volatile LONG error_count;
    uint8_t * read_buf;
    uint32_t read_buf_offset;
    uint32_t read_buf_length;
    HANDLE semaphore;
    uint32_t pipeline_depth;
};

static struct fpga_mem_s fpga_mem_;
static int verbose_;


static const char * op_name(uint8_t op) {
    switch (op) {
        case JTAG_MEM_OP_OPEN:      return "OPEN";
        case JTAG_MEM_OP_CLOSE:     return "CLOSE";
        case JTAG_MEM_OP_ERASE_64K: return "ERASE_64K";
        case JTAG_MEM_OP_ERASE_4K:  return "ERASE_4K";
        case JTAG_MEM_OP_WRITE:     return "WRITE";
        case JTAG_MEM_OP_READ:      return "READ";
        case JTAG_MEM_OP_PROG_AES_KEY: return "PROG_AES_KEY";
        case JTAG_MEM_OP_READ_UID:  return "READ_UID";
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
    if (verbose_) {
        printf("RSP: op=%s status=%d offset=0x%06X length=%u value_size=%u data_size=%u\n",
               op_name(rsp->operation), rsp->status, rsp->offset, rsp->length, value->size, data_size);
        uint32_t raw_dump = (value->size > 32) ? 32 : value->size;
        printf("  RAW[%u]:", value->size);
        for (uint32_t i = 0; i < raw_dump; ++i) {
            printf(" %02X", ((const uint8_t *) value->value.bin)[i]);
        }
        printf("\n");

        if ((rsp->operation == JTAG_MEM_OP_READ || rsp->operation == JTAG_MEM_OP_READ_UID) && data_size > 0) {
            uint32_t dump = (data_size > 16) ? 16 : data_size;
            printf("  READ data[0:%u]:", dump);
            for (uint32_t i = 0; i < dump; ++i) {
                printf(" %02X", rsp->data[i]);
            }
            printf("\n");
        }
    }

    if (rsp->operation == JTAG_MEM_OP_READ || rsp->operation == JTAG_MEM_OP_READ_UID) {
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

    if (rsp->status != 0) {
        InterlockedIncrement(&fpga_mem_.error_count);
    }
    InterlockedDecrement(&fpga_mem_.outstanding);
    ReleaseSemaphore(fpga_mem_.semaphore, 1, NULL);
}

static int pipeline_send(struct fpga_mem_s * fm, uint8_t op, uint32_t offset, uint32_t length,
                          const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    // If pipeline is full, wait for one slot to free
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

    InterlockedIncrement(&fm->outstanding);
    jsdrv_topic_set(&topic, fm->app->device.topic);
    jsdrv_topic_append(&topic, "c/jtag/mem/!cmd");
    int32_t pub_rc = jsdrv_publish(fm->app->context, topic.topic, &jsdrv_union_bin(buf, msg_size), 0);
    if (pub_rc) {
        InterlockedDecrement(&fm->outstanding);
        printf("ERROR: jsdrv_publish failed: %d\n", pub_rc);
        return pub_rc;
    }
    return 0;
}

static int pipeline_drain(struct fpga_mem_s * fm, uint32_t timeout_ms) {
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

static int jtag_mem_cmd(struct fpga_mem_s * fm, uint8_t op, uint32_t offset, uint32_t length,
                        const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    fm->error_count = 0;
    int rc = pipeline_send(fm, op, offset, length, data, data_size, timeout_ms);
    if (rc) return rc;
    return pipeline_drain(fm, timeout_ms);
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

// --- Common setup/teardown ---

static int setup_common(struct app_s * self) {
    struct jsdrv_topic_s topic;

    if (verbose_) {
        printf("sizeof(jtag_mem_s) = %u\n", (uint32_t) sizeof(struct jtag_mem_s));
    }

    memset(&fpga_mem_, 0, sizeof(fpga_mem_));
    fpga_mem_.app = self;
    fpga_mem_.semaphore = CreateSemaphore(NULL, 0, PIPELINE_MAX, NULL);
    fpga_mem_.pipeline_depth = 1;  // default synchronous, overridden by callers

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(500);  // todo replace with controlled interlock when available

    // Activate JTAG mode
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/mode");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(2), 0);
    Sleep(100);  // allow mode switch to take effect

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/jtag/mem/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_rsp, NULL, 0);
    return 0;
}

static int teardown_common(struct app_s * self, int rc) {
    struct jsdrv_topic_s topic;

    // Restore normal operation
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/mode");
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(0), 0);

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    CloseHandle(fpga_mem_.semaphore);
    return rc;
}

static int setup(struct app_s * self) {
    int rc = setup_common(self);
    if (rc) return rc;

    // OPEN (enters SPI background mode)
    printf("Opening JTAG...\n");
    rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_OPEN, 0, 0, NULL, 0, 5000);
    if (rc) {
        printf("ERROR: JTAG open failed\n");
    } else {
        printf("JTAG opened\n");
    }
    return rc;
}

static int teardown(struct app_s * self, int rc) {
    printf("Closing JTAG...\n");
    fpga_mem_.pipeline_depth = 1;
    jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_CLOSE, 0, 0, NULL, 0, 5000);
    return teardown_common(self, rc);
}

// --- Subcommands ---

static int do_erase(struct app_s * self, uint32_t offset, uint32_t size) {
    (void) self;
    // Round up to 64 KB block boundary
    uint32_t end = offset + size;
    offset &= ~(FLASH_BLOCK_64K - 1);
    end = (end + FLASH_BLOCK_64K - 1) & ~(FLASH_BLOCK_64K - 1);
    uint32_t num_blocks = (end - offset) / FLASH_BLOCK_64K;

    printf("Erasing %u blocks from 0x%06X (%u bytes)...\n", num_blocks, offset, num_blocks * FLASH_BLOCK_64K);
    for (uint32_t block = 0; block < num_blocks; ++block) {
        if (quit_) { return 1; }
        uint32_t addr = offset + block * FLASH_BLOCK_64K;
        if (verbose_) {
            printf("  Erase block %u/%u (offset 0x%06X)\n", block + 1, num_blocks, addr);
        }
        int rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_ERASE_64K, addr, 0, NULL, 0, 5000);
        if (rc) {
            printf("ERROR: erase failed at offset 0x%06X\n", addr);
            return rc;
        }
    }
    printf("Erase complete\n");
    return 0;
}

static int do_write(struct app_s * self, uint32_t offset, uint32_t size) {
    (void) self;
    // Round up to page boundary
    uint32_t padded_size = (size + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    uint32_t num_pages = padded_size / FLASH_PAGE_SIZE;

    printf("Writing %u pages (%u bytes) at 0x%06X (pattern)...\n", num_pages, padded_size, offset);
    uint8_t page_buf[FLASH_PAGE_SIZE];
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) { return 1; }
        uint32_t addr = offset + page * FLASH_PAGE_SIZE;
        // Fill with known pattern: each byte = (addr + i) & 0xFF
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
            page_buf[i] = (uint8_t) ((addr + i) & 0xFF);
        }
        if (verbose_) {
            printf("  Write page %u/%u (offset 0x%06X)\n", page + 1, num_pages, addr);
        }
        int rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_WRITE, addr, FLASH_PAGE_SIZE, page_buf, FLASH_PAGE_SIZE, 1000);
        if (rc) {
            printf("ERROR: write failed at offset 0x%06X\n", addr);
            return rc;
        }
    }
    printf("Write complete\n");
    return 0;
}

static int do_read(struct app_s * self, uint32_t offset, uint32_t size) {
    (void) self;
    uint8_t buf[FLASH_PAGE_SIZE];

    printf("Reading %u bytes at 0x%06X...\n", size, offset);
    uint32_t remaining = size;
    uint32_t addr = offset;
    while (remaining > 0) {
        if (quit_) { return 1; }
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        memset(buf, 0, sizeof(buf));
        int rc = jtag_mem_read(&fpga_mem_, addr, buf, chunk);
        if (rc) {
            printf("ERROR: read failed at offset 0x%06X\n", addr);
            return rc;
        }
        // Hex dump
        for (uint32_t i = 0; i < chunk; i += 16) {
            printf("  %06X:", addr + i);
            uint32_t line = ((chunk - i) > 16) ? 16 : (chunk - i);
            for (uint32_t j = 0; j < line; ++j) {
                printf(" %02X", buf[i + j]);
            }
            printf("\n");
        }
        addr += chunk;
        remaining -= chunk;
    }
    printf("Read complete\n");
    return 0;
}

static int do_program(struct app_s * self, const char * image_path, uint32_t pipeline_depth) {
    (void) self;
    uint8_t * image = NULL;
    uint32_t image_size = 0;
    int rc = 0;

    image = file_read(image_path, &image_size);
    if (!image) {
        return 1;
    }
    printf("Image: %s, %u bytes (pipeline=%u)\n", image_path, image_size, pipeline_depth);
    if (verbose_) {
        printf("Image first 16 bytes:");
        for (uint32_t i = 0; i < 16 && i < image_size; ++i) {
            printf(" %02X", image[i]);
        }
        printf("\n");
    }

    // ERASE (synchronous — flash serializes internally)
    uint32_t num_blocks = (image_size + FLASH_BLOCK_64K - 1) / FLASH_BLOCK_64K;
    printf("Erasing %u blocks (%u bytes)...\n", num_blocks, num_blocks * FLASH_BLOCK_64K);
    for (uint32_t block = 0; block < num_blocks; ++block) {
        if (quit_) { rc = 1; goto done; }
        uint32_t offset = block * FLASH_BLOCK_64K;
        if (verbose_) {
            printf("  Erase block %u/%u (offset 0x%06X)\n", block + 1, num_blocks, offset);
        }
        rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_ERASE_64K, offset, 0, NULL, 0, 5000);
        if (rc) {
            printf("ERROR: erase failed at offset 0x%06X\n", offset);
            goto done;
        }
    }
    printf("Erase complete\n");

    // WRITE (pipelined)
    uint32_t num_pages = (image_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    printf("Writing %u pages (%u bytes)...\n", num_pages, image_size);
    fpga_mem_.pipeline_depth = pipeline_depth;
    fpga_mem_.error_count = 0;
    uint8_t page_buf[FLASH_PAGE_SIZE];
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) { rc = 1; goto write_drain; }
        uint32_t offset = page * FLASH_PAGE_SIZE;
        uint32_t remaining = image_size - offset;
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        const uint8_t * src;
        if (chunk < FLASH_PAGE_SIZE) {
            memcpy(page_buf, image + offset, chunk);
            memset(page_buf + chunk, 0xFF, FLASH_PAGE_SIZE - chunk);
            src = page_buf;
        } else {
            src = image + offset;
        }
        if (verbose_ && ((page % 64) == 0)) {
            printf("  Write page %u/%u (offset 0x%06X)\n", page + 1, num_pages, offset);
        }
        rc = pipeline_send(&fpga_mem_, JTAG_MEM_OP_WRITE, offset, FLASH_PAGE_SIZE, src, FLASH_PAGE_SIZE, 1000);
        if (rc) {
            printf("ERROR: write failed at offset 0x%06X\n", offset);
            goto write_drain;
        }
    }
write_drain:
    if (!rc) {
        rc = pipeline_drain(&fpga_mem_, 1000);
    } else {
        pipeline_drain(&fpga_mem_, 1000);
    }
    fpga_mem_.pipeline_depth = 1;
    if (rc) { goto done; }
    printf("Write complete\n");

    // READ + VERIFY (pipelined in batches)
    printf("Verifying %u bytes...\n", image_size);
    uint32_t batch = pipeline_depth;
    uint32_t batch_bytes = batch * FLASH_PAGE_SIZE;
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
        uint32_t base_offset = base * FLASH_PAGE_SIZE;

        // Build expected data for this batch
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t page = base + i;
            uint32_t offset = page * FLASH_PAGE_SIZE;
            uint32_t remaining = image_size - offset;
            uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
            memcpy(expect_buf + i * FLASH_PAGE_SIZE, image + offset, chunk);
            if (chunk < FLASH_PAGE_SIZE) {
                memset(expect_buf + i * FLASH_PAGE_SIZE + chunk, 0xFF, FLASH_PAGE_SIZE - chunk);
            }
        }

        // Set up read buffer and send pipelined reads
        fpga_mem_.read_buf = verify_buf;
        fpga_mem_.read_buf_offset = base_offset;
        fpga_mem_.read_buf_length = count * FLASH_PAGE_SIZE;
        fpga_mem_.pipeline_depth = pipeline_depth;
        fpga_mem_.error_count = 0;
        memset(verify_buf, 0, count * FLASH_PAGE_SIZE);

        if (verbose_ && ((base % 64) == 0)) {
            printf("  Verify page %u/%u (offset 0x%06X)\n", base + 1, num_pages, base_offset);
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t offset = (base + i) * FLASH_PAGE_SIZE;
            rc = pipeline_send(&fpga_mem_, JTAG_MEM_OP_READ, offset, FLASH_PAGE_SIZE, NULL, 0, 1000);
            if (rc) {
                printf("ERROR: read send failed at offset 0x%06X\n", offset);
                break;
            }
        }
        if (!rc) {
            rc = pipeline_drain(&fpga_mem_, 1000);
        } else {
            pipeline_drain(&fpga_mem_, 1000);
        }
        fpga_mem_.read_buf = NULL;
        fpga_mem_.pipeline_depth = 1;
        if (rc) { break; }

        // Compare this batch
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t buf_off = i * FLASH_PAGE_SIZE;
            if (memcmp(verify_buf + buf_off, expect_buf + buf_off, FLASH_PAGE_SIZE) != 0) {
                uint32_t offset = (base + i) * FLASH_PAGE_SIZE;
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

done:
    free(image);
    if (rc == 0) {
        printf("FPGA flash programmed successfully\n");
    }
    return rc;
}

static int parse_hex_key(const char * hex, uint8_t * key) {
    if (strlen(hex) != 32) {
        return 1;
    }
    for (int i = 0; i < 16; ++i) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char * end;
        unsigned long val = strtoul(byte_str, &end, 16);
        if (*end != '\0') {
            return 1;
        }
        key[i] = (uint8_t) val;
    }
    return 0;
}

static int do_prog_aes_key(struct app_s * self, const uint8_t * key,
                           uint32_t flags) {
    (void) self;

    printf("WARNING: This will permanently burn OTP fuses!\n");
    printf("  Key: ");
    for (int i = 0; i < 16; ++i) {
        printf("%02X", key[i]);
    }
    printf("\n");
    printf("  Flags:%s%s\n",
           (flags & 0x01) ? " KEY_LOCK" : "",
           (flags & 0x02) ? " ENCRYPT_ONLY" : "");

    printf("Programming AES key...\n");
    int rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_PROG_AES_KEY,
                          flags, 16, key, 16, 15000);
    if (rc) {
        printf("ERROR: AES key programming failed\n");
        return rc;
    }
    printf("AES key programmed successfully\n");
    return 0;
}

static int do_read_uid(struct app_s * self) {
    (void) self;
    uint8_t uid[8];
    memset(uid, 0, sizeof(uid));

    fpga_mem_.read_buf = uid;
    fpga_mem_.read_buf_offset = 0;
    fpga_mem_.read_buf_length = 8;

    int rc = jtag_mem_cmd(&fpga_mem_, JTAG_MEM_OP_READ_UID, 0, 0, NULL, 0, 1000);
    fpga_mem_.read_buf = NULL;

    if (rc) {
        printf("ERROR: read UID failed\n");
        return rc;
    }

    printf("Flash Unique ID: ");
    for (int i = 0; i < 8; ++i) {
        printf("%02X", uid[i]);
    }
    printf("\n");
    return 0;
}

// --- Entry point ---

static int usage(void) {
    printf(
        "usage: minibitty fpga_mem [-v] [-p N] <subcommand> [args] [device_filter]\n"
        "\n"
        "Options:\n"
        "  -v, --verbose            Show per-transaction details\n"
        "  -p, --pipeline N         Pipeline depth for write/verify (default 4, max %u)\n"
        "\n"
        "Subcommands:\n"
        "  erase <offset> <size>    Erase flash (size rounded up to 64 KB blocks)\n"
        "  write <offset> <size>    Write test pattern (size rounded up to 256 B pages)\n"
        "  read <offset> <size>     Read and hex dump\n"
        "  program <path>           Erase, write, and verify image file\n"
        "  uid                      Read the SPI flash 64-bit unique ID\n"
        "  aes_key <hex> [options]  Program ECP5 AES-128 encryption key (OTP!)\n"
        "    --lock                   Set key lock fuse (prevents readback)\n"
        "    --encrypt-only           Require encrypted bitstreams only\n"
        "\n"
        "Offset and size accept hex (0x...) or decimal.\n",
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

int on_fpga_mem(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    int rc;
    verbose_ = 0;
    uint32_t pipeline_depth = 4;

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
        } else {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        }
    }

    if (argc < 1) {
        return usage();
    }

    const char * subcmd = argv[0];
    ARG_CONSUME();

    if (0 == strcmp(subcmd, "erase")) {
        if (argc < 2) { printf("erase requires <offset> <size>\n"); return usage(); }
        uint32_t offset, size;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (parse_u32(argv[0], &size)) { printf("invalid size: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (!rc) { rc = do_erase(self, offset, size); }
        return teardown(self, rc);
    } else if (0 == strcmp(subcmd, "write")) {
        if (argc < 2) { printf("write requires <offset> <size>\n"); return usage(); }
        uint32_t offset, size;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (parse_u32(argv[0], &size)) { printf("invalid size: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (!rc) { rc = do_write(self, offset, size); }
        return teardown(self, rc);
    } else if (0 == strcmp(subcmd, "read")) {
        if (argc < 2) { printf("read requires <offset> <size>\n"); return usage(); }
        uint32_t offset, size;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (parse_u32(argv[0], &size)) { printf("invalid size: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (!rc) { rc = do_read(self, offset, size); }
        return teardown(self, rc);
    } else if (0 == strcmp(subcmd, "program")) {
        if (argc < 1) { printf("program requires <path>\n"); return usage(); }
        const char * path = argv[0];
        ARG_CONSUME();
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (!rc) { rc = do_program(self, path, pipeline_depth); }
        return teardown(self, rc);
    } else if (0 == strcmp(subcmd, "uid")) {
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (!rc) { rc = do_read_uid(self); }
        return teardown(self, rc);
    } else if (0 == strcmp(subcmd, "aes_key")) {
        if (argc < 1) {
            printf("aes_key requires <key_hex> (32 hex chars)\n");
            return usage();
        }
        uint8_t key[16];
        if (parse_hex_key(argv[0], key)) {
            printf("invalid key: expected 32 hex chars, got '%s'\n",
                   argv[0]);
            return usage();
        }
        ARG_CONSUME();
        uint32_t flags = 0;
        while (argc > 0 && argv[0][0] == '-') {
            if (0 == strcmp(argv[0], "--lock")) {
                flags |= 0x01;
                ARG_CONSUME();
            } else if (0 == strcmp(argv[0], "--encrypt-only")) {
                flags |= 0x02;
                ARG_CONSUME();
            } else {
                break;
            }
        }
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup_common(self);
        if (!rc) { rc = do_prog_aes_key(self, key, flags); }
        return teardown_common(self, rc);
    } else {
        printf("unknown subcommand: %s\n", subcmd);
        return usage();
    }
}
