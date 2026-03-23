/*
 * Copyright 2026 Jetperch LLC
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

/**
 * @file
 *
 * @brief Generic memory read/write/erase/verify command.
 *
 * Works with any memory task that implements the mb_stdmsg_mem_s protocol.
 * The command topic and target are specified as CLI arguments.
 */

#include "minibitty_exe_prv.h"
#include "mb/stdmsg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>


#define FLASH_PAGE_SIZE     256U
#define FLASH_BLOCK_64K     65536U
#define PIPELINE_MAX        16U


struct mem_s {
    struct app_s * app;
    char cmd_topic[128];
    uint8_t target;
    uint32_t transaction_id;
    volatile LONG outstanding;
    volatile LONG error_count;
    uint8_t * read_buf;
    uint32_t read_buf_offset;
    uint32_t read_buf_length;
    HANDLE semaphore;
    uint32_t pipeline_depth;
};

static struct mem_s mem_;
static int verbose_;


static const char * op_name(uint8_t op) {
    switch (op) {
        case MB_STDMSG_MEM_OP_READ:  return "READ";
        case MB_STDMSG_MEM_OP_WRITE: return "WRITE";
        case MB_STDMSG_MEM_OP_ERASE: return "ERASE";
        default:                     return "UNKNOWN";
    }
}

static void on_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    uint32_t hdr_offset = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_offset = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_offset + sizeof(struct mb_stdmsg_mem_s)) {
        printf("RSP ERROR: too short: %u bytes (need %u)\n", value->size,
               (uint32_t) (hdr_offset + sizeof(struct mb_stdmsg_mem_s)));
        return;
    }
    const struct mb_stdmsg_mem_s * rsp = (const struct mb_stdmsg_mem_s *)
        (value->value.bin + hdr_offset);

    uint32_t data_size = value->size - hdr_offset - (uint32_t) sizeof(struct mb_stdmsg_mem_s);
    if (verbose_) {
        printf("RSP: op=%s status=%d offset=0x%06X length=%u data_size=%u\n",
               op_name(rsp->operation), rsp->status, rsp->offset, rsp->length, data_size);
    }

    if (rsp->operation == MB_STDMSG_MEM_OP_READ) {
        uint32_t copy_size = rsp->length;
        if (copy_size > data_size) {
            copy_size = data_size;
        }
        if (mem_.read_buf && copy_size > 0) {
            uint32_t buf_offset = rsp->offset - mem_.read_buf_offset;
            if (buf_offset + copy_size <= mem_.read_buf_length) {
                memcpy(mem_.read_buf + buf_offset, rsp->data, copy_size);
            } else {
                printf("  READ buf_offset=%u + copy_size=%u > buf_length=%u, skipped\n",
                       buf_offset, copy_size, mem_.read_buf_length);
            }
        }
    }

    if (rsp->status != 0) {
        InterlockedIncrement(&mem_.error_count);
    }
    InterlockedDecrement(&mem_.outstanding);
    ReleaseSemaphore(mem_.semaphore, 1, NULL);
}

static int pipeline_send(uint8_t op, uint32_t offset, uint32_t length,
                          const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    while (mem_.outstanding >= (LONG) mem_.pipeline_depth) {
        DWORD result = WaitForSingleObject(mem_.semaphore, timeout_ms + 5000);
        if (result != WAIT_OBJECT_0) {
            printf("ERROR: timeout waiting for pipeline slot\n");
            return 1;
        }
    }
    if (mem_.error_count > 0) {
        return 1;
    }

    struct jsdrv_topic_s topic;
    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + FLASH_PAGE_SIZE];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    uint32_t msg_size = sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s) + data_size;

    memset(cmd, 0, sizeof(*cmd));
    cmd->transaction_id = ++mem_.transaction_id;
    cmd->target = mem_.target;
    cmd->operation = op;
    cmd->timeout_ms = (uint16_t) timeout_ms;
    cmd->offset = offset;
    cmd->length = length;
    if (data && data_size > 0) {
        memcpy(cmd->data, data, data_size);
    }

    InterlockedIncrement(&mem_.outstanding);
    jsdrv_topic_set(&topic, mem_.app->device.topic);
    jsdrv_topic_append(&topic, mem_.cmd_topic);
    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = msg_size;
    v.value.bin = buf;
    int32_t pub_rc = jsdrv_publish(mem_.app->context, topic.topic, &v, 0);
    if (pub_rc) {
        InterlockedDecrement(&mem_.outstanding);
        printf("ERROR: jsdrv_publish failed: %d\n", pub_rc);
        return pub_rc;
    }
    return 0;
}

static int pipeline_drain(uint32_t timeout_ms) {
    while (mem_.outstanding > 0) {
        DWORD result = WaitForSingleObject(mem_.semaphore, timeout_ms + 5000);
        if (result != WAIT_OBJECT_0) {
            printf("ERROR: timeout waiting for pipeline drain (%ld outstanding)\n", mem_.outstanding);
            return 1;
        }
    }
    if (mem_.error_count > 0) {
        printf("ERROR: %ld commands failed during pipeline\n", mem_.error_count);
        return 1;
    }
    return 0;
}

static int mem_cmd(uint8_t op, uint32_t offset, uint32_t length,
                   const uint8_t * data, uint32_t data_size, uint32_t timeout_ms) {
    mem_.error_count = 0;
    int rc = pipeline_send(op, offset, length, data, data_size, timeout_ms);
    if (rc) return rc;
    return pipeline_drain(timeout_ms);
}

static int mem_read(uint32_t offset, uint8_t * buf, uint32_t length) {
    mem_.read_buf = buf;
    mem_.read_buf_offset = offset;
    mem_.read_buf_length = length;

    uint32_t remaining = length;
    uint32_t addr = offset;
    while (remaining > 0) {
        if (quit_) { mem_.read_buf = NULL; return 1; }
        uint32_t chunk = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        int rc = mem_cmd(MB_STDMSG_MEM_OP_READ, addr, chunk, NULL, 0, 1000);
        if (rc) {
            mem_.read_buf = NULL;
            return rc;
        }
        addr += chunk;
        remaining -= chunk;
    }
    mem_.read_buf = NULL;
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

static int parse_u32(const char * str, uint32_t * out) {
    char * end = NULL;
    unsigned long val = strtoul(str, &end, 0);
    if (end == str || *end != '\0') {
        return 1;
    }
    *out = (uint32_t) val;
    return 0;
}


// --- Setup / teardown ---

static int setup(struct app_s * self) {
    struct jsdrv_topic_s topic;

    memset(&mem_, 0, sizeof(mem_));
    mem_.app = self;
    mem_.semaphore = CreateSemaphore(NULL, 0, PIPELINE_MAX, NULL);
    mem_.pipeline_depth = 1;

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(500);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_rsp, NULL, 0);
    return 0;
}

static int teardown(struct app_s * self, int rc) {
    struct jsdrv_topic_s topic;

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_unsubscribe(self->context, topic.topic, on_rsp, NULL, 0);

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    CloseHandle(mem_.semaphore);
    return rc;
}


// --- Subcommands ---

static int do_erase(uint32_t offset, uint32_t size) {
    uint32_t end = offset + size;
    offset &= ~(FLASH_BLOCK_64K - 1);
    end = (end + FLASH_BLOCK_64K - 1) & ~(FLASH_BLOCK_64K - 1);
    uint32_t num_blocks = (end - offset) / FLASH_BLOCK_64K;

    mem_.pipeline_depth = 1;
    printf("Erasing %u blocks from 0x%06X (%u bytes)...\n", num_blocks, offset, num_blocks * FLASH_BLOCK_64K);
    for (uint32_t block = 0; block < num_blocks; ++block) {
        if (quit_) { return 1; }
        uint32_t addr = offset + block * FLASH_BLOCK_64K;
        if (verbose_) {
            printf("  Erase block %u/%u (offset 0x%06X)\n", block + 1, num_blocks, addr);
        }
        int rc = mem_cmd(MB_STDMSG_MEM_OP_ERASE, addr, FLASH_BLOCK_64K, NULL, 0, 5000);
        if (rc) {
            printf("ERROR: erase failed at offset 0x%06X\n", addr);
            return rc;
        }
    }
    printf("Erase complete\n");
    return 0;
}

static int do_write(uint32_t offset, const char * path, uint32_t pipeline_depth, int verify) {
    uint32_t file_size = 0;
    uint8_t * data = file_read(path, &file_size);
    if (!data) { return 1; }

    uint32_t padded_size = (file_size + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    uint32_t num_pages = padded_size / FLASH_PAGE_SIZE;

    mem_.pipeline_depth = pipeline_depth;
    mem_.error_count = 0;
    printf("Writing %u pages (%u bytes) at 0x%06X from %s...\n", num_pages, file_size, offset, path);

    uint8_t page_buf[FLASH_PAGE_SIZE];
    for (uint32_t page = 0; page < num_pages; ++page) {
        if (quit_) { free(data); return 1; }
        uint32_t addr = offset + page * FLASH_PAGE_SIZE;
        uint32_t src_offset = page * FLASH_PAGE_SIZE;
        uint32_t chunk = file_size - src_offset;
        if (chunk > FLASH_PAGE_SIZE) { chunk = FLASH_PAGE_SIZE; }

        memset(page_buf, 0xFF, FLASH_PAGE_SIZE);
        memcpy(page_buf, data + src_offset, chunk);

        if (verbose_) {
            printf("  Write page %u/%u (offset 0x%06X)\n", page + 1, num_pages, addr);
        }
        int rc = pipeline_send(MB_STDMSG_MEM_OP_WRITE, addr, FLASH_PAGE_SIZE, page_buf, FLASH_PAGE_SIZE, 1000);
        if (rc) {
            printf("ERROR: write failed at offset 0x%06X\n", addr);
            free(data);
            return rc;
        }
    }
    int rc = pipeline_drain(1000);
    if (rc) {
        free(data);
        printf("ERROR: write pipeline drain failed\n");
        return rc;
    }
    printf("Write complete\n");

    if (verify) {
        printf("Verifying %u bytes at 0x%06X...\n", file_size, offset);
        uint8_t * dev_data = (uint8_t *) malloc(file_size);
        if (!dev_data) {
            printf("ERROR: could not allocate %u bytes for verify\n", file_size);
            free(data);
            return 1;
        }
        memset(dev_data, 0, file_size);
        rc = mem_read(offset, dev_data, file_size);
        if (rc) {
            printf("ERROR: read failed during verify\n");
            free(data);
            free(dev_data);
            return rc;
        }
        if (memcmp(data, dev_data, file_size) != 0) {
            uint32_t mismatch = 0;
            for (uint32_t i = 0; i < file_size; ++i) {
                if (data[i] != dev_data[i]) {
                    mismatch = i;
                    break;
                }
            }
            printf("VERIFY FAILED: first mismatch at offset 0x%06X "
                   "(file=0x%02X device=0x%02X)\n",
                   offset + mismatch, data[mismatch], dev_data[mismatch]);
            rc = 1;
        } else {
            printf("Verify OK (%u bytes match)\n", file_size);
        }
        free(dev_data);
    }

    free(data);
    return rc;
}

static int do_read(uint32_t offset, uint32_t size, const char * out_path) {
    uint8_t * buf = (uint8_t *) malloc(size);
    if (!buf) {
        printf("ERROR: could not allocate %u bytes\n", size);
        return 1;
    }
    memset(buf, 0, size);

    printf("Reading %u bytes at 0x%06X...\n", size, offset);
    int rc = mem_read(offset, buf, size);
    if (rc) {
        printf("ERROR: read failed\n");
        free(buf);
        return rc;
    }

    if (out_path) {
        FILE * f = fopen(out_path, "wb");
        if (!f) {
            printf("ERROR: could not open output file: %s\n", out_path);
            free(buf);
            return 1;
        }
        size_t written = fwrite(buf, 1, size, f);
        fclose(f);
        if (written != size) {
            printf("ERROR: wrote %zu of %u bytes to %s\n", written, size, out_path);
            free(buf);
            return 1;
        }
        printf("Read %u bytes to %s\n", size, out_path);
    } else {
        for (uint32_t i = 0; i < size; i += 16) {
            printf("  %06X:", offset + i);
            uint32_t line = ((size - i) > 16) ? 16 : (size - i);
            for (uint32_t j = 0; j < line; ++j) {
                printf(" %02X", buf[i + j]);
            }
            printf("\n");
        }
        printf("Read complete\n");
    }
    free(buf);
    return 0;
}

static int do_verify(uint32_t offset, const char * path) {
    uint32_t file_size = 0;
    uint8_t * file_data = file_read(path, &file_size);
    if (!file_data) { return 1; }

    uint8_t * dev_data = (uint8_t *) malloc(file_size);
    if (!dev_data) {
        printf("ERROR: could not allocate %u bytes\n", file_size);
        free(file_data);
        return 1;
    }
    memset(dev_data, 0, file_size);

    printf("Verifying %u bytes at 0x%06X against %s...\n", file_size, offset, path);
    int rc = mem_read(offset, dev_data, file_size);
    if (rc) {
        printf("ERROR: read failed during verify\n");
        free(file_data);
        free(dev_data);
        return rc;
    }

    if (memcmp(file_data, dev_data, file_size) != 0) {
        uint32_t mismatch = 0;
        for (uint32_t i = 0; i < file_size; ++i) {
            if (file_data[i] != dev_data[i]) {
                mismatch = i;
                break;
            }
        }
        printf("VERIFY FAILED: first mismatch at offset 0x%06X "
               "(file=0x%02X device=0x%02X)\n",
               offset + mismatch, file_data[mismatch], dev_data[mismatch]);
        rc = 1;
    } else {
        printf("Verify OK (%u bytes match)\n", file_size);
        rc = 0;
    }

    free(file_data);
    free(dev_data);
    return rc;
}


// --- Command entry point ---

static int usage(void) {
    printf(
        "usage: minibitty mem [-v] [-p N] <topic> <target> <subcommand>\n"
        "                     [args] [device_filter]\n"
        "\n"
        "Options:\n"
        "  -v, --verbose            Show per-transaction details\n"
        "  -p, --pipeline N         Pipeline depth (default 4, max %u)\n"
        "\n"
        "Positional:\n"
        "  <topic>                  Memory command topic (e.g. s/flash/!cmd)\n"
        "  <target>                 Memory target ID (integer, e.g. 0)\n"
        "\n"
        "Subcommands:\n"
        "  erase <offset> <size>             Erase (rounded to 64KB blocks)\n"
        "  write <offset> <file> [--verify]   Write binary file contents\n"
        "  read <offset> <size> [--out f]    Read (hex dump or file output)\n"
        "  verify <offset> <file>            Read and compare against file\n"
        "\n"
        "Examples:\n"
        "  minibitty mem s/flash/!cmd 0 erase 0x140000 0x20000\n"
        "  minibitty mem s/flash/!cmd 0 write 0x140000 meta.bin\n"
        "  minibitty mem s/flash/!cmd 0 read 0x140000 0x1000 --out dump.bin\n"
        "  minibitty mem s/flash/!cmd 0 verify 0x140000 meta.bin\n",
        PIPELINE_MAX
        );
    return 1;
}

int on_mem(struct app_s * self, int argc, char * argv[]) {
    char * device_filter = NULL;
    int rc;
    verbose_ = 0;
    uint32_t pipeline_depth = 4;

    // Parse options
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

    // Parse topic
    if (argc < 1) { printf("missing <topic>\n"); return usage(); }
    const char * cmd_topic = argv[0];
    ARG_CONSUME();

    // Parse target
    if (argc < 1) { printf("missing <target>\n"); return usage(); }
    uint32_t target_u32;
    if (parse_u32(argv[0], &target_u32) || target_u32 > 255) {
        printf("invalid target: %s\n", argv[0]);
        return usage();
    }
    ARG_CONSUME();

    // Parse subcommand
    if (argc < 1) { return usage(); }
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
        if (rc) return rc;
        snprintf(mem_.cmd_topic, sizeof(mem_.cmd_topic), "%s", cmd_topic);
        mem_.target = (uint8_t) target_u32;
        rc = do_erase(offset, size);
        return teardown(self, rc);

    } else if (0 == strcmp(subcmd, "write")) {
        if (argc < 2) { printf("write requires <offset> <file>\n"); return usage(); }
        uint32_t offset;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        const char * path = argv[0];
        ARG_CONSUME();
        int verify = 0;
        if (argc > 0 && (0 == strcmp(argv[0], "--verify"))) {
            verify = 1;
            ARG_CONSUME();
        }
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (rc) return rc;
        snprintf(mem_.cmd_topic, sizeof(mem_.cmd_topic), "%s", cmd_topic);
        mem_.target = (uint8_t) target_u32;
        rc = do_write(offset, path, pipeline_depth, verify);
        return teardown(self, rc);

    } else if (0 == strcmp(subcmd, "read")) {
        if (argc < 2) { printf("read requires <offset> <size>\n"); return usage(); }
        uint32_t offset, size;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        if (parse_u32(argv[0], &size)) { printf("invalid size: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        const char * out_path = NULL;
        if (argc > 0 && (0 == strcmp(argv[0], "--out") || 0 == strcmp(argv[0], "-o"))) {
            ARG_CONSUME();
            if (argc < 1) { printf("--out requires a filename\n"); return usage(); }
            out_path = argv[0];
            ARG_CONSUME();
        }
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (rc) return rc;
        snprintf(mem_.cmd_topic, sizeof(mem_.cmd_topic), "%s", cmd_topic);
        mem_.target = (uint8_t) target_u32;
        rc = do_read(offset, size, out_path);
        return teardown(self, rc);

    } else if (0 == strcmp(subcmd, "verify")) {
        if (argc < 2) { printf("verify requires <offset> <file>\n"); return usage(); }
        uint32_t offset;
        if (parse_u32(argv[0], &offset)) { printf("invalid offset: %s\n", argv[0]); return usage(); }
        ARG_CONSUME();
        const char * path = argv[0];
        ARG_CONSUME();
        if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }
        ROE(app_match(self, device_filter));
        rc = setup(self);
        if (rc) return rc;
        snprintf(mem_.cmd_topic, sizeof(mem_.cmd_topic), "%s", cmd_topic);
        mem_.target = (uint8_t) target_u32;
        rc = do_verify(offset, path);
        return teardown(self, rc);

    } else {
        printf("unknown subcommand: %s\n", subcmd);
        return usage();
    }
}
