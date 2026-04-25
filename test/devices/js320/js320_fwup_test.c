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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/time.h"
#include "jsdrv_prv/devices/js320/js320_fwup.h"
#include "mb/stdmsg.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/devices/mb_device/mb_drv.h"
#include "jsdrv_prv/platform.h"


// =====================================================================
// Device-side protocol structs (mirrors static defs in js320_fwup.c)
// =====================================================================

#define FW_PAGE_SIZE        256U
#define STDMSG_HDR_SIZE     sizeof(struct mb_stdmsg_header_s)
#define DEV_CMD_SIZE        (STDMSG_HDR_SIZE + sizeof(struct mb_stdmsg_mem_s))
#define FLASH_BLOCK_64K     65536U

/// Fwup device op codes (mirrors mb_fwup_op_e + mb_stdmsg_mem_op_e).
enum fwup_dev_op_e {
    FWUP_DEV_OP_READ      = MB_STDMSG_MEM_OP_READ,
    FWUP_DEV_OP_WRITE     = MB_STDMSG_MEM_OP_WRITE,
    FWUP_DEV_OP_ERASE     = MB_STDMSG_MEM_OP_ERASE,
    FWUP_DEV_OP_ALLOCATE  = MB_STDMSG_MEM_OP_CUSTOM_START,
    FWUP_DEV_OP_UPDATE,
    FWUP_DEV_OP_LAUNCH,
};

/// JTAG device op codes (mirrors js320_jtag_op_e).
enum js320_jtag_op_e {
    JS320_JTAG_OP_MEM_OPEN      = MB_STDMSG_MEM_OP_CUSTOM_START + 2,
    JS320_JTAG_OP_MEM_CLOSE,
};


// =====================================================================
// Mock capture infrastructure
// =====================================================================

#define MAX_DEV_CMDS    64
#define MAX_CMD_SIZE    (DEV_CMD_SIZE + FW_PAGE_SIZE + 16)

struct captured_dev_cmd {
    char topic[128];
    uint8_t data[MAX_CMD_SIZE];
    uint32_t size;
};

struct test_ctx {
    int dummy_dev_storage;  // opaque; we only need a valid pointer
    struct js320_fwup_s * fwup;

    // Captured device commands (ring buffer)
    struct captured_dev_cmd dev_cmds[MAX_DEV_CMDS];
    uint32_t dev_cmd_wr;
    uint32_t dev_cmd_rd;

    // Last frontend response
    char fe_subtopic[128];
    struct fwup_rsp_s fe_rsp;
    int fe_rsp_count;

    // Last timeout
    int64_t last_timeout;
    int timeout_count;
};

static struct test_ctx g_ctx;

// --- Mock implementations ---

void jsdrvp_mb_dev_publish_to_device(
        struct jsdrvp_mb_dev_s * dev,
        const char * topic,
        const struct jsdrv_union_s * value) {
    (void) dev;
    struct test_ctx * ctx = &g_ctx;
    assert_true(ctx->dev_cmd_wr - ctx->dev_cmd_rd < MAX_DEV_CMDS);
    uint32_t idx = ctx->dev_cmd_wr % MAX_DEV_CMDS;
    strncpy(ctx->dev_cmds[idx].topic, topic,
            sizeof(ctx->dev_cmds[idx].topic) - 1);
    ctx->dev_cmds[idx].topic[sizeof(ctx->dev_cmds[idx].topic) - 1] = 0;
    uint32_t sz = value->size;
    if (sz > MAX_CMD_SIZE) {
        sz = MAX_CMD_SIZE;
    }
    memcpy(ctx->dev_cmds[idx].data, value->value.bin, sz);
    ctx->dev_cmds[idx].size = sz;
    ctx->dev_cmd_wr++;
}

void jsdrvp_mb_dev_send_to_frontend(
        struct jsdrvp_mb_dev_s * dev,
        const char * subtopic,
        const struct jsdrv_union_s * value) {
    (void) dev;
    struct test_ctx * ctx = &g_ctx;
    strncpy(ctx->fe_subtopic, subtopic,
            sizeof(ctx->fe_subtopic) - 1);
    ctx->fe_subtopic[sizeof(ctx->fe_subtopic) - 1] = 0;
    assert_true(value->size >= sizeof(struct fwup_rsp_s));
    memcpy(&ctx->fe_rsp, value->value.bin, sizeof(struct fwup_rsp_s));
    ctx->fe_rsp_count++;
}

void jsdrvp_mb_dev_set_timeout(
        struct jsdrvp_mb_dev_s * dev,
        int64_t timeout_utc) {
    (void) dev;
    struct test_ctx * ctx = &g_ctx;
    ctx->last_timeout = timeout_utc;
    ctx->timeout_count++;
}

// Not called by js320_fwup.c but needed by linker if mb_drv.h
// declares them and we link jsdrv_support_objlib.
void jsdrvp_mb_dev_send_to_device(
        struct jsdrvp_mb_dev_s * dev,
        enum mb_frame_service_type_e service_type,
        uint16_t metadata,
        const uint32_t * data,
        uint32_t length_u32) {
    (void) dev; (void) service_type; (void) metadata;
    (void) data; (void) length_u32;
}

struct jsdrv_context_s * jsdrvp_mb_dev_context(
        struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return NULL;
}

const char * jsdrvp_mb_dev_prefix(struct jsdrvp_mb_dev_s * dev) {
    (void) dev;
    return "u/js320/test";
}

void jsdrvp_mb_dev_backend_send(
        struct jsdrvp_mb_dev_s * dev,
        struct jsdrvp_msg_s * msg) {
    (void) dev; (void) msg;
}


// --- Stubs for unresolved symbols from jsdrv_support_objlib ---

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(
        struct jsdrv_context_s * context,
        const char * topic,
        const struct jsdrv_union_s * value) {
    (void) context; (void) topic; (void) value;
    return NULL;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context,
                      struct jsdrvp_msg_s * msg) {
    (void) context; (void) msg;
}


// =====================================================================
// Helpers
// =====================================================================

static uint32_t dev_cmd_pending(void) {
    return g_ctx.dev_cmd_wr - g_ctx.dev_cmd_rd;
}

static struct captured_dev_cmd * pop_dev_cmd(void) {
    assert_true(g_ctx.dev_cmd_rd < g_ctx.dev_cmd_wr);
    uint32_t idx = g_ctx.dev_cmd_rd % MAX_DEV_CMDS;
    g_ctx.dev_cmd_rd++;
    return &g_ctx.dev_cmds[idx];
}

static struct mb_stdmsg_mem_s * pop_fw_cmd(const char * expected_topic) {
    struct captured_dev_cmd * c = pop_dev_cmd();
    assert_string_equal(c->topic, expected_topic);
    assert_true(c->size >= DEV_CMD_SIZE);
    return (struct mb_stdmsg_mem_s *) (c->data + STDMSG_HDR_SIZE);
}

static struct captured_dev_cmd * pop_jtag_cmd_raw(
        const char * expected_topic) {
    struct captured_dev_cmd * c = pop_dev_cmd();
    assert_string_equal(c->topic, expected_topic);
    assert_true(c->size >= DEV_CMD_SIZE);
    return c;
}

static struct mb_stdmsg_mem_s * pop_jtag_cmd(const char * expected_topic) {
    struct captured_dev_cmd * c = pop_jtag_cmd_raw(expected_topic);
    return (struct mb_stdmsg_mem_s *) (c->data + STDMSG_HDR_SIZE);
}

/// Build a ctrl cmd with appended image data.
static void build_ctrl_cmd(uint8_t * buf, uint32_t * out_size,
                            uint32_t txn_id, uint8_t op,
                            uint8_t image_slot, uint8_t pipeline_depth,
                            const uint8_t * image, uint32_t image_size) {
    struct fwup_ctrl_cmd_s * cmd = (struct fwup_ctrl_cmd_s *) buf;
    cmd->transaction_id = txn_id;
    cmd->op = op;
    cmd->image_slot = image_slot;
    cmd->pipeline_depth = pipeline_depth;
    cmd->rsv = 0;
    if (image && image_size > 0) {
        memcpy(cmd->data, image, image_size);
    }
    *out_size = sizeof(struct fwup_ctrl_cmd_s) + image_size;
}

/// Build an FPGA cmd with appended image data.
static void build_fpga_cmd(uint8_t * buf, uint32_t * out_size,
                            uint32_t txn_id, uint8_t op,
                            uint8_t pipeline_depth,
                            const uint8_t * image, uint32_t image_size) {
    struct fwup_fpga_cmd_s * cmd = (struct fwup_fpga_cmd_s *) buf;
    cmd->transaction_id = txn_id;
    cmd->op = op;
    cmd->pipeline_depth = pipeline_depth;
    cmd->rsv = 0;
    if (image && image_size > 0) {
        memcpy(cmd->data, image, image_size);
    }
    *out_size = sizeof(struct fwup_fpga_cmd_s) + image_size;
}

/// Build a device fwup response (header only or with read data).
static void build_fw_rsp(uint8_t * buf, uint32_t * out_size,
                          uint32_t id, uint8_t op, uint8_t status,
                          uint32_t offset, uint32_t length,
                          const uint8_t * data, uint32_t data_size) {
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * r = (struct mb_stdmsg_mem_s *) (hdr + 1);
    memset(r, 0, sizeof(*r));
    r->transaction_id = id;
    r->operation = op;
    r->status = status;
    r->offset = offset;
    r->length = length;
    if (data && data_size > 0) {
        memcpy(buf + STDMSG_HDR_SIZE + sizeof(struct mb_stdmsg_mem_s), data, data_size);
    }
    *out_size = (uint32_t)(STDMSG_HDR_SIZE + sizeof(struct mb_stdmsg_mem_s) + data_size);
}

/// Build a jtag_mem response (header only or with read data).
static void build_jtag_rsp(uint8_t * buf, uint32_t * out_size,
                            uint32_t txn_id, uint8_t op, uint8_t status,
                            uint32_t offset, uint32_t length,
                            const uint8_t * data, uint32_t data_size) {
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * r = (struct mb_stdmsg_mem_s *) (hdr + 1);
    memset(r, 0, sizeof(*r));
    r->transaction_id = txn_id;
    r->operation = op;
    r->status = status;
    r->offset = offset;
    r->length = length;
    if (data && data_size > 0) {
        memcpy(buf + STDMSG_HDR_SIZE + sizeof(struct mb_stdmsg_mem_s), data, data_size);
    }
    *out_size = (uint32_t)(STDMSG_HDR_SIZE + sizeof(struct mb_stdmsg_mem_s) + data_size);
}

/// Send a jsdrv_union_s STDMSG to handle_publish.
static bool send_publish_stdmsg(const char * subtopic,
                                 uint8_t * buf, uint32_t size) {
    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = size;
    v.value.bin = buf;
    return js320_fwup_handle_publish(g_ctx.fwup, subtopic, &v);
}

/// Pop the pre-update ERASE of slot 0 (locked-mode workaround) and
/// reply with success.  Call immediately after sending FWUP_CTRL_OP_UPDATE.
static void drive_ctrl_pre_erase(void) {
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ERASE, fc->operation);
    assert_int_equal(0, fc->target);
    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ERASE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
}

/// Get expected page data (with 0xFF padding).
static void get_expected_page(const uint8_t * image, uint32_t image_size,
                               uint32_t page_idx, uint8_t * out) {
    uint32_t offset = page_idx * FW_PAGE_SIZE;
    uint32_t remaining = image_size - offset;
    uint32_t chunk = (remaining > FW_PAGE_SIZE) ? FW_PAGE_SIZE : remaining;
    memcpy(out, image + offset, chunk);
    if (chunk < FW_PAGE_SIZE) {
        memset(out + chunk, 0xFF, FW_PAGE_SIZE - chunk);
    }
}


// =====================================================================
// Setup / Teardown
// =====================================================================

static int setup(void ** state) {
    (void) state;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.fwup = js320_fwup_alloc();
    assert_non_null(g_ctx.fwup);
    js320_fwup_on_open(g_ctx.fwup, (struct jsdrvp_mb_dev_s *) &g_ctx.dummy_dev_storage);
    return 0;
}

static int teardown(void ** state) {
    (void) state;
    js320_fwup_free(g_ctx.fwup);
    g_ctx.fwup = NULL;
    return 0;
}


// =====================================================================
// Ctrl Tests
// =====================================================================

static void test_ctrl_update_success(void ** state) {
    (void) state;
    // 512-byte image = 2 pages, pipeline_depth=2
    uint8_t image[512];
    memset(image, 0xAB, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 42, FWUP_CTRL_OP_UPDATE,
                   1, 2, image, sizeof(image));

    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    bool handled = js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);
    assert_true(handled);

    // Pre-update erase of slot 0 (locked-mode workaround)
    assert_int_equal(1, dev_cmd_pending());
    drive_ctrl_pre_erase();

    // Should have sent ALLOCATE
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ALLOCATE, fc->operation);
    assert_int_equal(512, fc->length);

    // Send ALLOCATE response
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, 512, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent 2 WRITE commands (pipeline=2)
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w0 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w0->operation);
    assert_int_equal(0, w0->offset);
    struct mb_stdmsg_mem_s * w1 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w1->operation);
    assert_int_equal(256, w1->offset);

    // Send 2 WRITE responses
    build_fw_rsp(rsp_buf, &rsp_size, w0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    build_fw_rsp(rsp_buf, &rsp_size, w1->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 256, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent 2 READ commands
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r0 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r0->operation);
    assert_int_equal(0, r0->offset);
    struct mb_stdmsg_mem_s * r1 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r1->operation);
    assert_int_equal(256, r1->offset);

    // Send 2 READ responses with correct data
    uint8_t page[FW_PAGE_SIZE];
    memset(page, 0xAB, FW_PAGE_SIZE);
    build_fw_rsp(rsp_buf, &rsp_size, r0->transaction_id, FWUP_DEV_OP_READ,
                 0, 0, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    build_fw_rsp(rsp_buf, &rsp_size, r1->transaction_id, FWUP_DEV_OP_READ,
                 0, 256, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent UPDATE
    assert_int_equal(1, dev_cmd_pending());
    fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_UPDATE, fc->operation);
    assert_int_equal(1, fc->target);

    // Send UPDATE response
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_UPDATE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Verify frontend response
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(42, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

static void test_ctrl_launch_success(void ** state) {
    (void) state;
    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 10, FWUP_CTRL_OP_LAUNCH,
                   1, 0, NULL, 0);

    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_LAUNCH, fc->operation);
    assert_int_equal(1, fc->target);

    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_LAUNCH,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(10, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

static void test_ctrl_erase_success(void ** state) {
    (void) state;
    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 20, FWUP_CTRL_OP_ERASE,
                   0, 0, NULL, 0);

    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ERASE, fc->operation);
    assert_int_equal(0, fc->target);

    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ERASE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(20, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

static void test_ctrl_busy_rejection(void ** state) {
    (void) state;
    // Start an UPDATE operation
    uint8_t image[256];
    memset(image, 0x55, sizeof(image));
    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 1, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Drain the pre-update ERASE command
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();

    // Send another command while busy
    build_ctrl_cmd(cmd_buf, &cmd_size, 99, FWUP_CTRL_OP_LAUNCH,
                   0, 0, NULL, 0);
    v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Should get BUSY response
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(99, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_BUSY, g_ctx.fe_rsp.status);
}

static void test_ctrl_device_error_write(void ** state) {
    (void) state;
    uint8_t image[512];
    memset(image, 0xCD, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 50, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, 512, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // 2 WRITE commands sent
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w0 = pop_fw_cmd("c/fwup/!cmd");
    struct mb_stdmsg_mem_s * w1 = pop_fw_cmd("c/fwup/!cmd");

    // First WRITE succeeds
    build_fw_rsp(rsp_buf, &rsp_size, w0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Second WRITE fails with device error (status=1)
    build_fw_rsp(rsp_buf, &rsp_size, w1->transaction_id, FWUP_DEV_OP_WRITE,
                 1, 256, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Pipeline drained (recv_idx >= send_idx), should finish with IO error
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(50, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}

static void test_ctrl_verify_mismatch(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0xAA, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 60, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, 256, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // 1 WRITE (only 1 page)
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w0 = pop_fw_cmd("c/fwup/!cmd");
    build_fw_rsp(rsp_buf, &rsp_size, w0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // 1 READ
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r0 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r0->operation);

    // Send wrong data
    uint8_t bad_page[FW_PAGE_SIZE];
    memset(bad_page, 0xBB, FW_PAGE_SIZE);
    build_fw_rsp(rsp_buf, &rsp_size, r0->transaction_id, FWUP_DEV_OP_READ,
                 0, 0, FW_PAGE_SIZE, bad_page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Pipeline drained, should get IO error
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(60, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}

static void test_ctrl_invalid_cmd(void ** state) {
    (void) state;
    // Send wrong type (not BIN)
    struct jsdrv_union_s v = jsdrv_union_u32(0);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, g_ctx.fe_rsp.status);
}

static void test_ctrl_update_no_image(void ** state) {
    (void) state;
    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 70, FWUP_CTRL_OP_UPDATE,
                   0, 0, NULL, 0);
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(70, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, g_ctx.fe_rsp.status);
}

/// Pre-update ERASE of slot 0 must run first regardless of caller's
/// image_slot, and the final UPDATE commit must always target slot 1.
static void test_ctrl_update_locked_mode_workaround(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0x5A, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    // Caller requests slot 0 — workaround must override to slot 1.
    build_ctrl_cmd(cmd_buf, &cmd_size, 71, FWUP_CTRL_OP_UPDATE,
                   0, 1, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // First device cmd is ERASE on slot 0 (not ALLOCATE)
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ERASE, fc->operation);
    assert_int_equal(0, fc->target);

    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ERASE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // ALLOCATE → WRITE → READ → UPDATE
    fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ALLOCATE, fc->operation);
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, sizeof(image), NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    struct mb_stdmsg_mem_s * w0 = pop_fw_cmd("c/fwup/!cmd");
    build_fw_rsp(rsp_buf, &rsp_size, w0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    struct mb_stdmsg_mem_s * r0 = pop_fw_cmd("c/fwup/!cmd");
    uint8_t page[FW_PAGE_SIZE];
    memset(page, 0x5A, FW_PAGE_SIZE);
    build_fw_rsp(rsp_buf, &rsp_size, r0->transaction_id, FWUP_DEV_OP_READ,
                 0, 0, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Final UPDATE commit must target slot 1, ignoring caller's slot 0.
    fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_UPDATE, fc->operation);
    assert_int_equal(1, fc->target);
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_UPDATE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    assert_int_equal(0, g_ctx.fe_rsp.status);
    assert_int_equal(71, (int) g_ctx.fe_rsp.transaction_id);
}

/// If the pre-update ERASE of slot 0 fails, UPDATE must abort
/// without sending ALLOCATE/WRITE.
static void test_ctrl_update_pre_erase_fails(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0x5B, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 72, FWUP_CTRL_OP_UPDATE,
                   1, 1, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pop the ERASE and respond with a device error.
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ERASE, fc->operation);
    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ERASE,
                 1, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have aborted: no further device cmds, IO error response.
    assert_int_equal(0, dev_cmd_pending());
    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(72, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}


// =====================================================================
// FPGA Tests
// =====================================================================

/// Helper: drive the FPGA state machine from MODE_SWITCH through OPEN.
static void fpga_drive_to_open(void) {
    // Should have published sensor disconnect + c/mode=2 and set timeout
    assert_int_equal(2, dev_cmd_pending());
    struct captured_dev_cmd * disc_cmd = pop_dev_cmd();
    assert_string_equal("c/comm/sensor/!req", disc_cmd->topic);
    struct captured_dev_cmd * mode_cmd = pop_dev_cmd();
    assert_string_equal("c/mode", mode_cmd->topic);
    assert_true(g_ctx.timeout_count > 0);

    // Fire timeout -> should send OPEN
    js320_fwup_on_timeout(g_ctx.fwup);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_OPEN, jc->operation);

    // Send OPEN response
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_OPEN, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
}

static void test_fpga_program_success(void ** state) {
    (void) state;
    uint8_t image[512];
    memset(image, 0xEE, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 100, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // Should have sent ERASE (1 block for 512 bytes)
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_ERASE, jc->operation);
    assert_int_equal(0, (int) jc->offset);

    // Send ERASE response
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent 2 WRITE commands (pipeline=2)
    assert_int_equal(2, dev_cmd_pending());
    struct captured_dev_cmd * wc0 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw0 = (struct mb_stdmsg_mem_s *) (wc0->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw0->operation);
    assert_int_equal(0, (int) jw0->offset);
    struct captured_dev_cmd * wc1 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw1 = (struct mb_stdmsg_mem_s *) (wc1->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw1->operation);
    assert_int_equal(256, (int) jw1->offset);

    // Send 2 WRITE responses
    build_jtag_rsp(rsp_buf, &rsp_size, jw0->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    build_jtag_rsp(rsp_buf, &rsp_size, jw1->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 256, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent 2 READ commands
    assert_int_equal(2, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_READ, jc->operation);
    assert_int_equal(0, (int) jc->offset);
    struct mb_stdmsg_mem_s * jr1 = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_READ, jr1->operation);
    assert_int_equal(256, (int) jr1->offset);

    // Send correct read data
    uint8_t page[FW_PAGE_SIZE];
    memset(page, 0xEE, FW_PAGE_SIZE);
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 0, FW_PAGE_SIZE,
                   page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    build_jtag_rsp(rsp_buf, &rsp_size, jr1->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 256, FW_PAGE_SIZE,
                   page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent CLOSE
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_CLOSE, jc->operation);

    // Send CLOSE response
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_CLOSE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should publish c/mode=0 and set timeout
    assert_int_equal(1, dev_cmd_pending());
    struct captured_dev_cmd * mode_cmd = pop_dev_cmd();
    assert_string_equal("c/mode", mode_cmd->topic);

    int timeout_before = g_ctx.timeout_count;
    // Fire timeout -> finish
    js320_fwup_on_timeout(g_ctx.fwup);
    (void) timeout_before;

    // Verify frontend response
    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(100, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

static void test_fpga_program_multi_erase(void ** state) {
    (void) state;
    // 65537 bytes = 2 erase blocks (65536 + 1)
    uint32_t img_size = FLASH_BLOCK_64K + 1;
    uint8_t * image = malloc(img_size);
    assert_non_null(image);
    memset(image, 0xDD, img_size);

    uint8_t * cmd_buf = malloc(sizeof(struct fwup_fpga_cmd_s) + img_size);
    assert_non_null(cmd_buf);
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 200, FWUP_FPGA_OP_PROGRAM,
                   2, image, img_size);
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // Erase block 0
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_ERASE, jc->operation);
    assert_int_equal(0, (int) jc->offset);

    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Erase block 1
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_ERASE, jc->operation);
    assert_int_equal(FLASH_BLOCK_64K, jc->offset);

    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, FLASH_BLOCK_64K, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should now be in WRITE phase — verify we got WRITE commands
    assert_true(dev_cmd_pending() > 0);
    struct mb_stdmsg_mem_s * jw = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw->operation);

    free(image);
    free(cmd_buf);
}

static void test_fpga_error_cleanup(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0xCC, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 300, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // ERASE
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // 1 WRITE command (pipeline=2, 1 page)
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jc->operation);

    // WRITE fails
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 1, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should enter error cleanup: CLOSE
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_CLOSE, jc->operation);

    // Send CLOSE rsp
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_CLOSE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should publish c/mode=0 and set timeout
    assert_int_equal(1, dev_cmd_pending());
    struct captured_dev_cmd * mode_cmd = pop_dev_cmd();
    assert_string_equal("c/mode", mode_cmd->topic);

    // Fire timeout -> finish with error
    js320_fwup_on_timeout(g_ctx.fwup);

    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(300, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}

static void test_fpga_busy_rejection(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0x11, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 400, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    // Drain the mode command
    pop_dev_cmd();

    // Send another cmd while busy
    build_fpga_cmd(cmd_buf, &cmd_size, 401, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(401, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_BUSY, g_ctx.fe_rsp.status);
}

static void test_fpga_verify_mismatch(void ** state) {
    (void) state;
    uint8_t image[256];
    memset(image, 0xAA, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 500, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // ERASE
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // WRITE (1 page)
    jc = pop_jtag_cmd("c/jtag/!cmd");
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // READ
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_READ, jc->operation);

    // Send wrong data
    uint8_t bad_page[FW_PAGE_SIZE];
    memset(bad_page, 0xFF, FW_PAGE_SIZE);
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 0, FW_PAGE_SIZE,
                   bad_page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Error cleanup -> CLOSE
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_CLOSE, jc->operation);

    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_CLOSE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // MODE_RESTORE
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();  // c/mode=0
    js320_fwup_on_timeout(g_ctx.fwup);
    assert_int_equal(1, dev_cmd_pending());
    struct captured_dev_cmd * reconn = pop_dev_cmd();
    assert_string_equal("c/comm/sensor/!req", reconn->topic);

    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(500, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}


// =====================================================================
// Edge Case Tests
// =====================================================================

static void test_pipeline_depth_default(void ** state) {
    (void) state;
    // pipeline_depth=0 should use default of 4
    // Use image of 4*256 = 1024 bytes = 4 pages
    uint8_t image[1024];
    memset(image, 0x77, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 80, FWUP_CTRL_OP_UPDATE,
                   0, 0, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, 1024, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent exactly 4 WRITE commands (default pipeline)
    assert_int_equal(4, dev_cmd_pending());
}

static void test_pipeline_depth_clamp(void ** state) {
    (void) state;
    // pipeline_depth=255 should clamp to 16
    // Use 17 pages = 4352 bytes so we can see clamping at 16
    uint32_t img_size = 17 * FW_PAGE_SIZE;
    uint8_t * image = malloc(img_size);
    assert_non_null(image);
    memset(image, 0x88, img_size);

    uint8_t * cmd_buf = malloc(sizeof(struct fwup_ctrl_cmd_s) + img_size);
    assert_non_null(cmd_buf);
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 81, FWUP_CTRL_OP_UPDATE,
                   0, 255, image, img_size);
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, img_size, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Should have sent 16 (not 17 or 255) WRITE commands
    assert_int_equal(16, dev_cmd_pending());

    free(image);
    free(cmd_buf);
}

static void test_last_page_padding(void ** state) {
    (void) state;
    // 300 bytes = 2 pages, second page is 44 bytes + 212 bytes of 0xFF
    uint8_t image[300];
    memset(image, 0x33, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 90, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, 300, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // 2 WRITE commands
    assert_int_equal(2, dev_cmd_pending());
    struct captured_dev_cmd * wc0 = pop_dev_cmd();
    struct captured_dev_cmd * wc1 = pop_dev_cmd();

    // Verify second WRITE has padded data
    uint8_t * write_data = wc1->data + DEV_CMD_SIZE;
    // First 44 bytes should be 0x33
    for (uint32_t i = 0; i < 44; i++) {
        assert_int_equal(0x33, write_data[i]);
    }
    // Remaining 212 bytes should be 0xFF
    for (uint32_t i = 44; i < FW_PAGE_SIZE; i++) {
        assert_int_equal(0xFF, write_data[i]);
    }

    // Send WRITE responses
    struct mb_stdmsg_mem_s * fw0 = (struct mb_stdmsg_mem_s *) (wc0->data + STDMSG_HDR_SIZE);
    struct mb_stdmsg_mem_s * fw1 = (struct mb_stdmsg_mem_s *) (wc1->data + STDMSG_HDR_SIZE);
    build_fw_rsp(rsp_buf, &rsp_size, fw0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    build_fw_rsp(rsp_buf, &rsp_size, fw1->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 256, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // READ phase: verify padded page matches in verification
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r0 = pop_fw_cmd("c/fwup/!cmd");
    struct mb_stdmsg_mem_s * r1 = pop_fw_cmd("c/fwup/!cmd");

    // Page 0: all 0x33
    uint8_t expected_p0[FW_PAGE_SIZE];
    get_expected_page(image, sizeof(image), 0, expected_p0);
    build_fw_rsp(rsp_buf, &rsp_size, r0->transaction_id, FWUP_DEV_OP_READ,
                 0, 0, FW_PAGE_SIZE, expected_p0, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Page 1: 44 bytes of 0x33 + 212 bytes of 0xFF
    uint8_t expected_p1[FW_PAGE_SIZE];
    get_expected_page(image, sizeof(image), 1, expected_p1);
    build_fw_rsp(rsp_buf, &rsp_size, r1->transaction_id, FWUP_DEV_OP_READ,
                 0, 256, FW_PAGE_SIZE, expected_p1, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // UPDATE
    assert_int_equal(1, dev_cmd_pending());
    fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_UPDATE, fc->operation);
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_UPDATE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    assert_int_equal(0, g_ctx.fe_rsp.status);
}

static void test_topic_routing(void ** state) {
    (void) state;
    struct jsdrv_union_s v = jsdrv_union_u32(0);

    assert_false(js320_fwup_handle_cmd(g_ctx.fwup,
                 "h/ctrl/range", &v));
    assert_false(js320_fwup_handle_cmd(g_ctx.fwup,
                 "c/fwup/!rsp", &v));
    assert_false(js320_fwup_handle_publish(g_ctx.fwup,
                 "h/fwup/ctrl/!cmd", &v));
    assert_false(js320_fwup_handle_publish(g_ctx.fwup,
                 "c/something/else", &v));
}

static void test_on_close_aborts(void ** state) {
    (void) state;
    // Start a ctrl operation
    uint8_t image[256];
    memset(image, 0x44, sizeof(image));
    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 999, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);
    pop_dev_cmd();  // drain pre-update ERASE

    // Close during operation
    js320_fwup_on_close(g_ctx.fwup);

    // Re-open and verify the state is clean
    js320_fwup_on_open(g_ctx.fwup, (struct jsdrvp_mb_dev_s *) &g_ctx.dummy_dev_storage);

    // Should be able to start a new operation
    build_ctrl_cmd(cmd_buf, &cmd_size, 1000, FWUP_CTRL_OP_LAUNCH,
                   0, 0, NULL, 0);
    v = jsdrv_union_bin(cmd_buf, cmd_size);
    g_ctx.fe_rsp_count = 0;
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Should have sent LAUNCH (not BUSY)
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_LAUNCH, fc->operation);
    // No BUSY response should have been sent
    assert_int_equal(0, g_ctx.fe_rsp_count);
}


// =====================================================================
// Pipeline Refill Tests
// =====================================================================

/// Ctrl pipeline refill: 4 pages with pipeline_depth=2.
/// Verifies that each response triggers exactly one refill command
/// and that write/verify data is correct for each page.
static void test_ctrl_pipeline_refill(void ** state) {
    (void) state;
    // 4 distinct pages so we can verify per-page data
    uint8_t image[4 * FW_PAGE_SIZE];
    for (uint32_t i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)(i & 0xFF);
    }

    uint8_t cmd_buf[sizeof(struct fwup_ctrl_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_ctrl_cmd(cmd_buf, &cmd_size, 200, FWUP_CTRL_OP_UPDATE,
                   0, 2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/ctrl/!cmd", &v);

    // Pre-update erase of slot 0
    drive_ctrl_pre_erase();

    // ALLOCATE
    struct mb_stdmsg_mem_s * fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_ALLOCATE, fc->operation);
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_ALLOCATE,
                 0, 0, sizeof(image), NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // --- WRITE phase: initial fill sends pages 0,1 ---
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w0 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w0->operation);
    assert_int_equal(0 * FW_PAGE_SIZE, w0->offset);
    struct mb_stdmsg_mem_s * w1 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w1->operation);
    assert_int_equal(1 * FW_PAGE_SIZE, w1->offset);

    // Respond to page 0 -> should refill with page 2
    build_fw_rsp(rsp_buf, &rsp_size, w0->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 0 * FW_PAGE_SIZE, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w2 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w2->operation);
    assert_int_equal(2 * FW_PAGE_SIZE, w2->offset);

    // Respond to page 1 -> should refill with page 3
    build_fw_rsp(rsp_buf, &rsp_size, w1->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 1 * FW_PAGE_SIZE, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * w3 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_WRITE, w3->operation);
    assert_int_equal(3 * FW_PAGE_SIZE, w3->offset);

    // Respond to page 2 -> no more pages, no refill
    build_fw_rsp(rsp_buf, &rsp_size, w2->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 2 * FW_PAGE_SIZE, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(0, dev_cmd_pending());

    // Respond to page 3 -> all writes done, enter VERIFY
    build_fw_rsp(rsp_buf, &rsp_size, w3->transaction_id, FWUP_DEV_OP_WRITE,
                 0, 3 * FW_PAGE_SIZE, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // --- VERIFY phase: initial fill sends reads 0,1 ---
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r0 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r0->operation);
    assert_int_equal(0 * FW_PAGE_SIZE, r0->offset);
    struct mb_stdmsg_mem_s * r1 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r1->operation);
    assert_int_equal(1 * FW_PAGE_SIZE, r1->offset);

    // Respond to read 0 (correct data) -> refill with read 2
    uint8_t page[FW_PAGE_SIZE];
    get_expected_page(image, sizeof(image), 0, page);
    build_fw_rsp(rsp_buf, &rsp_size, r0->transaction_id, FWUP_DEV_OP_READ,
                 0, 0 * FW_PAGE_SIZE, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r2 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r2->operation);
    assert_int_equal(2 * FW_PAGE_SIZE, r2->offset);

    // Respond to read 1 -> refill with read 3
    get_expected_page(image, sizeof(image), 1, page);
    build_fw_rsp(rsp_buf, &rsp_size, r1->transaction_id, FWUP_DEV_OP_READ,
                 0, 1 * FW_PAGE_SIZE, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * r3 = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_READ, r3->operation);
    assert_int_equal(3 * FW_PAGE_SIZE, r3->offset);

    // Respond to read 2 -> no refill
    get_expected_page(image, sizeof(image), 2, page);
    build_fw_rsp(rsp_buf, &rsp_size, r2->transaction_id, FWUP_DEV_OP_READ,
                 0, 2 * FW_PAGE_SIZE, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(0, dev_cmd_pending());

    // Respond to read 3 -> all verified, send UPDATE
    get_expected_page(image, sizeof(image), 3, page);
    build_fw_rsp(rsp_buf, &rsp_size, r3->transaction_id, FWUP_DEV_OP_READ,
                 0, 3 * FW_PAGE_SIZE, FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // UPDATE
    assert_int_equal(1, dev_cmd_pending());
    fc = pop_fw_cmd("c/fwup/!cmd");
    assert_int_equal(FWUP_DEV_OP_UPDATE, fc->operation);
    build_fw_rsp(rsp_buf, &rsp_size, fc->transaction_id, FWUP_DEV_OP_UPDATE,
                 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    assert_string_equal("h/fwup/ctrl/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(200, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

/// FPGA pipeline refill: 4 pages with pipeline_depth=2.
/// Verifies refill during WRITE and VERIFY phases, plus
/// correct write payload data for each page.
static void test_fpga_pipeline_refill(void ** state) {
    (void) state;
    uint8_t image[4 * FW_PAGE_SIZE];
    for (uint32_t i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)((i * 3 + 7) & 0xFF);
    }

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 600, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // ERASE (1 block)
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_ERASE, jc->operation);
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // --- WRITE phase: initial fill sends pages 0,1 ---
    assert_int_equal(2, dev_cmd_pending());
    struct captured_dev_cmd * wc0 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw0 = (struct mb_stdmsg_mem_s *) (wc0->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw0->operation);
    assert_int_equal(0 * FW_PAGE_SIZE, (int) jw0->offset);
    // Verify write payload for page 0
    uint8_t expected[FW_PAGE_SIZE];
    get_expected_page(image, sizeof(image), 0, expected);
    assert_memory_equal(wc0->data + DEV_CMD_SIZE,
                        expected, FW_PAGE_SIZE);

    struct captured_dev_cmd * wc1 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw1 = (struct mb_stdmsg_mem_s *) (wc1->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw1->operation);
    assert_int_equal(1 * FW_PAGE_SIZE, (int) jw1->offset);
    get_expected_page(image, sizeof(image), 1, expected);
    assert_memory_equal(wc1->data + DEV_CMD_SIZE,
                        expected, FW_PAGE_SIZE);

    // Respond to page 0 -> refill with page 2
    build_jtag_rsp(rsp_buf, &rsp_size, jw0->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 0 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct captured_dev_cmd * wc2 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw2 = (struct mb_stdmsg_mem_s *) (wc2->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw2->operation);
    assert_int_equal(2 * FW_PAGE_SIZE, (int) jw2->offset);
    get_expected_page(image, sizeof(image), 2, expected);
    assert_memory_equal(wc2->data + DEV_CMD_SIZE,
                        expected, FW_PAGE_SIZE);

    // Respond to page 1 -> refill with page 3
    build_jtag_rsp(rsp_buf, &rsp_size, jw1->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 1 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct captured_dev_cmd * wc3 = pop_jtag_cmd_raw("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw3 = (struct mb_stdmsg_mem_s *) (wc3->data + STDMSG_HDR_SIZE);
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw3->operation);
    assert_int_equal(3 * FW_PAGE_SIZE, (int) jw3->offset);
    get_expected_page(image, sizeof(image), 3, expected);
    assert_memory_equal(wc3->data + DEV_CMD_SIZE,
                        expected, FW_PAGE_SIZE);

    // Respond to page 2 -> no refill
    build_jtag_rsp(rsp_buf, &rsp_size, jw2->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 2 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(0, dev_cmd_pending());

    // Respond to page 3 -> enter VERIFY
    build_jtag_rsp(rsp_buf, &rsp_size, jw3->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 3 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // --- VERIFY phase: initial fill sends reads 0,1 ---
    assert_int_equal(2, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_READ, jc->operation);
    assert_int_equal(0 * FW_PAGE_SIZE, (int) jc->offset);
    struct mb_stdmsg_mem_s * jr1 = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(1 * FW_PAGE_SIZE, (int) jr1->offset);

    // Respond read 0 -> refill read 2
    uint8_t page[FW_PAGE_SIZE];
    get_expected_page(image, sizeof(image), 0, page);
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 0 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jr2 = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(2 * FW_PAGE_SIZE, (int) jr2->offset);

    // Respond read 1 -> refill read 3
    get_expected_page(image, sizeof(image), 1, page);
    build_jtag_rsp(rsp_buf, &rsp_size, jr1->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 1 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jr3 = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(3 * FW_PAGE_SIZE, (int) jr3->offset);

    // Respond read 2 -> no refill
    get_expected_page(image, sizeof(image), 2, page);
    build_jtag_rsp(rsp_buf, &rsp_size, jr2->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 2 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    assert_int_equal(0, dev_cmd_pending());

    // Respond read 3 -> enter CLOSE
    get_expected_page(image, sizeof(image), 3, page);
    build_jtag_rsp(rsp_buf, &rsp_size, jr3->transaction_id,
                   MB_STDMSG_MEM_OP_READ, 0, 3 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, page, FW_PAGE_SIZE);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // CLOSE
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_CLOSE, jc->operation);
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_CLOSE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // MODE_RESTORE
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();  // c/mode=0
    js320_fwup_on_timeout(g_ctx.fwup);
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();  // c/comm/sensor/!req=1

    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(600, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(0, g_ctx.fe_rsp.status);
}

/// FPGA pipeline error during write with pipeline_depth=2
/// and page_count=4. Verifies that after error the pipeline
/// drains (no new sends) before cleanup.
static void test_fpga_pipeline_error_drain(void ** state) {
    (void) state;
    uint8_t image[4 * FW_PAGE_SIZE];
    memset(image, 0xBB, sizeof(image));

    uint8_t cmd_buf[sizeof(struct fwup_fpga_cmd_s) + sizeof(image)];
    uint32_t cmd_size;
    build_fpga_cmd(cmd_buf, &cmd_size, 700, FWUP_FPGA_OP_PROGRAM,
                   2, image, sizeof(image));
    struct jsdrv_union_s v = jsdrv_union_bin(cmd_buf, cmd_size);
    js320_fwup_handle_cmd(g_ctx.fwup, "h/fwup/fpga/!cmd", &v);

    fpga_drive_to_open();

    // ERASE
    struct mb_stdmsg_mem_s * jc = pop_jtag_cmd("c/jtag/!cmd");
    uint8_t rsp_buf[DEV_CMD_SIZE + FW_PAGE_SIZE];
    uint32_t rsp_size;
    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   MB_STDMSG_MEM_OP_ERASE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // WRITE: 2 initial sends (pages 0,1)
    assert_int_equal(2, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jw0 = pop_jtag_cmd("c/jtag/!cmd");
    struct mb_stdmsg_mem_s * jw1 = pop_jtag_cmd("c/jtag/!cmd");

    // Page 0 succeeds -> would normally refill page 2
    build_jtag_rsp(rsp_buf, &rsp_size, jw0->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 0, FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    // Page 2 was sent as refill
    assert_int_equal(1, dev_cmd_pending());
    struct mb_stdmsg_mem_s * jw2 = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(MB_STDMSG_MEM_OP_WRITE, jw2->operation);

    // Page 1 FAILS -> error detected, no more refills
    build_jtag_rsp(rsp_buf, &rsp_size, jw1->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 1, FW_PAGE_SIZE, FW_PAGE_SIZE,
                   NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);
    // Should NOT have refilled (error draining)
    assert_int_equal(0, dev_cmd_pending());

    // Page 2 succeeds -> drain complete, enter error cleanup
    build_jtag_rsp(rsp_buf, &rsp_size, jw2->transaction_id,
                   MB_STDMSG_MEM_OP_WRITE, 0, 2 * FW_PAGE_SIZE,
                   FW_PAGE_SIZE, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // Error cleanup -> CLOSE
    assert_int_equal(1, dev_cmd_pending());
    jc = pop_jtag_cmd("c/jtag/!cmd");
    assert_int_equal(JS320_JTAG_OP_MEM_CLOSE, jc->operation);

    build_jtag_rsp(rsp_buf, &rsp_size, jc->transaction_id,
                   JS320_JTAG_OP_MEM_CLOSE, 0, 0, 0, NULL, 0);
    send_publish_stdmsg("h/!rsp", rsp_buf, rsp_size);

    // MODE_RESTORE
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();  // c/mode=0
    js320_fwup_on_timeout(g_ctx.fwup);
    assert_int_equal(1, dev_cmd_pending());
    pop_dev_cmd();  // c/comm/sensor/!req=1

    assert_string_equal("h/fwup/fpga/!rsp", g_ctx.fe_subtopic);
    assert_int_equal(700, (int) g_ctx.fe_rsp.transaction_id);
    assert_int_equal(JSDRV_ERROR_IO, g_ctx.fe_rsp.status);
}


// =====================================================================
// Main
// =====================================================================

int main(void) {
    const struct CMUnitTest tests[] = {
        // Ctrl tests
        cmocka_unit_test_setup_teardown(test_ctrl_update_success,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_launch_success,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_erase_success,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_busy_rejection,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_device_error_write,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_verify_mismatch,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_invalid_cmd,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_update_no_image,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_update_locked_mode_workaround,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_update_pre_erase_fails,
                                        setup, teardown),
        // FPGA tests
        cmocka_unit_test_setup_teardown(test_fpga_program_success,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_program_multi_erase,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_error_cleanup,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_busy_rejection,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_verify_mismatch,
                                        setup, teardown),
        // Edge cases
        cmocka_unit_test_setup_teardown(test_pipeline_depth_default,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_pipeline_depth_clamp,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_last_page_padding,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_topic_routing,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_on_close_aborts,
                                        setup, teardown),
        // Pipeline refill tests
        cmocka_unit_test_setup_teardown(test_ctrl_pipeline_refill,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_pipeline_refill,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_fpga_pipeline_error_drain,
                                        setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
