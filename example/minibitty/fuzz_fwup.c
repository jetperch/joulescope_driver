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
 * @brief JS320 data-block recovery fuzz tester.
 *
 * Verifies that the JS320 host open path + fwup path together recover
 * from any subset of the six persistent data blocks being missing or
 * invalid.  For each subset the tool erases the selected blocks,
 * power-cycles the DUT via a JS220 power supply, times the device
 * open, runs full fwup to restore, and verifies.
 */

#include "minibitty_exe_prv.h"
#include "mb/stdmsg.h"
#include "jsdrv/cstr.h"
#include "jsdrv/os_atomic.h"
#include "jsdrv/os_sem.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/time.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/devices/js320/js320_fwup_mgr.h"
#include "jsdrv_prv/platform.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define FLASH_BLOCK_64K         65536U
#define BLOCK_COUNT             6U

// JTAG custom mem operations (match js320_jtag.h / fpga_mem.c).
#define JTAG_OP_MEM_OPEN        (MB_STDMSG_MEM_OP_CUSTOM_START + 2)
#define JTAG_OP_MEM_CLOSE       (MB_STDMSG_MEM_OP_CUSTOM_START + 3)


// --- Block table ---------------------------------------------------

enum block_id_e {
    BLOCK_CTRL_FW       = 0,
    BLOCK_CTRL_PUBSUB   = 1,
    BLOCK_CTRL_TRACE    = 2,
    BLOCK_SENSOR_FPGA   = 3,
    BLOCK_SENSOR_PUBSUB = 4,
    BLOCK_SENSOR_TRACE  = 5,
};

struct block_info_s {
    const char * name;
    const char * cmd_topic;    ///< Memory command subtopic.
    uint32_t offset;
    uint32_t size;             ///< 0 = special (FPGA)
};

static const struct block_info_s BLOCKS[BLOCK_COUNT] = {
    // id                      name             topic           offset      size
    [BLOCK_CTRL_FW]       = {"ctrl_fw",       "c/xspi/!cmd",  0x000000, 0x80000},
    [BLOCK_CTRL_PUBSUB]   = {"ctrl_pubsub",   "c/xspi/!cmd",  0x080000, 0x20000},
    [BLOCK_CTRL_TRACE]    = {"ctrl_trace",    "c/xspi/!cmd",  0x0A0000, 0x20000},
    [BLOCK_SENSOR_FPGA]   = {"sensor_fpga",   "c/jtag/!cmd",  0x000000, 0},
    [BLOCK_SENSOR_PUBSUB] = {"sensor_pubsub", "s/flash/!cmd", 0x140000, 0x20000},
    [BLOCK_SENSOR_TRACE]  = {"sensor_trace",  "s/flash/!cmd", 0x160000, 0x20000},
};


// --- Config --------------------------------------------------------

struct cfg_s {
    const char * release_zip;
    const char * target_filter;
    const char * power_filter;
    const char * output_path;
    uint32_t depth;
    uint32_t open_timeout_ms;
    uint32_t fwup_timeout_ms;
    uint64_t max_duration_s;
    uint32_t repeat;
    int do_reset;           ///< --reset: soft reset between iterations
    int stop_on_fail;
    int dry_run;
    int verbose;
    uint32_t explicit_subsets[64];
    uint32_t explicit_subset_count;
};


// --- Per-iteration result ------------------------------------------

struct result_s {
    uint32_t mask;
    uint32_t erase_ms;
    uint32_t power_cycle_ms;
    uint32_t open_ms;
    int32_t  open_rc;
    uint32_t fwup_ms;
    int32_t  fwup_rc;
    int32_t  verify_rc;
    int      pass;
};


// --- Shared state --------------------------------------------------

// Mem command (single-depth, shared across all mem op dispatch).
static struct {
    uint32_t transaction_id;
    jsdrv_os_sem_t sem;
    jsdrv_os_atomic_t outstanding;
    int32_t last_status;
    int subscribed;
    struct jsdrv_topic_s rsp_topic;
} mem_ = { 0 };

// Fwup status tracking (fwup/NNN/status).
static volatile int fwup_done_ = 0;
static volatile int32_t fwup_rc_ = 0;
static char fwup_state_[32];
static int fwup_subscribed_ = 0;

// DUT presence tracking.
static const char * target_prefix_ = NULL;
static volatile bool target_present_ = false;


// --- Utility -------------------------------------------------------

static uint32_t ms_since(int64_t t_start) {
    int64_t now = jsdrv_time_utc();
    int64_t delta = now - t_start;
    if (delta < 0) { delta = 0; }
    return (uint32_t) (delta / JSDRV_TIME_MILLISECOND);
}

static uint32_t popcount6(uint32_t x) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < BLOCK_COUNT; ++i) {
        if (x & (1u << i)) { ++c; }
    }
    return c;
}

static int parse_u32(const char * str, uint32_t * out) {
    char * end = NULL;
    unsigned long v = strtoul(str, &end, 0);
    if (end == str || *end != '\0') { return 1; }
    *out = (uint32_t) v;
    return 0;
}

static void mask_to_names(uint32_t mask, char * buf, size_t buf_size) {
    size_t pos = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < BLOCK_COUNT; ++i) {
        if (!(mask & (1u << i))) { continue; }
        const char * n = BLOCKS[i].name;
        if (pos > 0 && pos + 1 < buf_size) {
            buf[pos++] = ',';
        }
        while (*n && pos + 1 < buf_size) {
            buf[pos++] = *n++;
        }
    }
    if (pos < buf_size) { buf[pos] = '\0'; }
}

static uint8_t * file_read(const char * path, uint32_t * size_out) {
    FILE * f = fopen(path, "rb");
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t * data = (uint8_t *) malloc((size_t) sz);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, (size_t) sz, f);
    fclose(f);
    if ((long) n != sz) { free(data); return NULL; }
    *size_out = (uint32_t) sz;
    return data;
}


// --- Mem command -------------------------------------------------

static void on_mem_rsp(void * user_data, const char * topic,
                       const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    uint32_t hdr_off = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_off = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_off + sizeof(struct mb_stdmsg_mem_s)) {
        mem_.last_status = -1;
    } else {
        const struct mb_stdmsg_mem_s * rsp = (const struct mb_stdmsg_mem_s *)
            (value->value.bin + hdr_off);
        mem_.last_status = rsp->status;
    }
    JSDRV_OS_ATOMIC_DEC(&mem_.outstanding);
    jsdrv_os_sem_release(mem_.sem);
}

static int mem_setup(struct app_s * self) {
    if (mem_.subscribed) { return 0; }
    if (!mem_.sem) {
        mem_.sem = jsdrv_os_sem_alloc(0, 1);
    }
    mem_.transaction_id = 0;
    mem_.outstanding = 0;
    mem_.last_status = 0;
    jsdrv_topic_set(&mem_.rsp_topic, self->device.topic);
    jsdrv_topic_append(&mem_.rsp_topic, "h/!rsp");
    ROE(jsdrv_subscribe(self->context, mem_.rsp_topic.topic,
                        JSDRV_SFLAG_PUB, on_mem_rsp, NULL, 0));
    mem_.subscribed = 1;
    return 0;
}

static void mem_teardown(struct app_s * self) {
    if (mem_.subscribed) {
        jsdrv_unsubscribe(self->context, mem_.rsp_topic.topic,
                          on_mem_rsp, NULL, 0);
        mem_.subscribed = 0;
    }
}

static int mem_cmd_once(struct app_s * self, const char * cmd_subtopic,
                        uint8_t op, uint32_t offset, uint32_t length,
                        uint32_t timeout_ms) {
    struct jsdrv_topic_s topic;
    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s)];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);

    memset(buf, 0, sizeof(buf));
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    cmd->transaction_id = ++mem_.transaction_id;
    cmd->operation = op;
    cmd->timeout_ms = (uint16_t) ((timeout_ms > 0xFFFF) ? 0xFFFF : timeout_ms);
    cmd->offset = offset;
    cmd->length = length;

    mem_.last_status = 0;
    JSDRV_OS_ATOMIC_INC(&mem_.outstanding);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, cmd_subtopic);

    struct jsdrv_union_s v;
    memset(&v, 0, sizeof(v));
    v.type = JSDRV_UNION_STDMSG;
    v.size = (uint32_t) sizeof(buf);
    v.value.bin = buf;

    int32_t rc = jsdrv_publish(self->context, topic.topic, &v, 0);
    if (rc) {
        JSDRV_OS_ATOMIC_DEC(&mem_.outstanding);
        return rc;
    }
    int32_t wait_rc = jsdrv_os_sem_wait(mem_.sem, timeout_ms + 5000);
    if (wait_rc) { return 1; }
    return mem_.last_status;
}

static int mem_erase_range(struct app_s * self, const char * cmd_subtopic,
                           uint32_t offset, uint32_t size) {
    uint32_t end = offset + size;
    offset &= ~(FLASH_BLOCK_64K - 1);
    end = (end + FLASH_BLOCK_64K - 1) & ~(FLASH_BLOCK_64K - 1);
    for (uint32_t addr = offset; addr < end; addr += FLASH_BLOCK_64K) {
        if (quit_) { return 1; }
        int rc = mem_cmd_once(self, cmd_subtopic,
                              MB_STDMSG_MEM_OP_ERASE,
                              addr, FLASH_BLOCK_64K, 5000);
        if (rc) {
            printf("  ERROR: erase %s @ 0x%06X failed: %d\n",
                   cmd_subtopic, addr, rc);
            return rc;
        }
    }
    return 0;
}


// --- Block-4 FPGA erase --------------------------------------------

static int fpga_erase_first_block(struct app_s * self) {
    struct jsdrv_topic_s topic;

    // Enter JTAG mode.
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/mode");
    int32_t rc = jsdrv_publish(self->context, topic.topic,
                               &jsdrv_union_u32(2), 0);
    if (rc) {
        printf("  ERROR: c/mode=2 publish failed: %d\n", rc);
        return rc;
    }
    jsdrv_thread_sleep_ms(100);

    // JTAG OPEN, ERASE first 64 KB, CLOSE.
    int op_rc = mem_cmd_once(self, "c/jtag/!cmd",
                             (uint8_t) JTAG_OP_MEM_OPEN, 0, 0, 5000);
    if (op_rc) {
        printf("  ERROR: JTAG OPEN failed: %d\n", op_rc);
    } else {
        op_rc = mem_cmd_once(self, "c/jtag/!cmd",
                             MB_STDMSG_MEM_OP_ERASE,
                             0, FLASH_BLOCK_64K, 5000);
        if (op_rc) {
            printf("  ERROR: JTAG erase failed: %d\n", op_rc);
        }
        int close_rc = mem_cmd_once(self, "c/jtag/!cmd",
                                    (uint8_t) JTAG_OP_MEM_CLOSE, 0, 0, 5000);
        if (!op_rc) { op_rc = close_rc; }
    }

    // Restore normal mode.
    jsdrv_publish(self->context, topic.topic, &jsdrv_union_u32(0), 0);
    return op_rc;
}


// --- Erase dispatcher ---------------------------------------------

static int erase_block(struct app_s * self, uint32_t block_id) {
    const struct block_info_s * b = &BLOCKS[block_id];
    if (block_id == BLOCK_SENSOR_FPGA) {
        return fpga_erase_first_block(self);
    }
    return mem_erase_range(self, b->cmd_topic, b->offset, b->size);
}

// Erase ctrl and sensor-flash blocks first; erase sensor_fpga last.
// Erasing the FPGA bitstream kills the sensor MCU (which is an FPGA
// soft-core), which would then block sensor-flash erases in the same
// subset.
static const uint32_t ERASE_ORDER[BLOCK_COUNT] = {
    BLOCK_CTRL_FW,
    BLOCK_CTRL_PUBSUB,
    BLOCK_CTRL_TRACE,
    BLOCK_SENSOR_PUBSUB,
    BLOCK_SENSOR_TRACE,
    BLOCK_SENSOR_FPGA,
};

static int erase_subset(struct app_s * self, uint32_t mask) {
    for (uint32_t k = 0; k < BLOCK_COUNT; ++k) {
        uint32_t i = ERASE_ORDER[k];
        if (!(mask & (1u << i))) { continue; }
        printf("  Erase %s\n", BLOCKS[i].name);
        int rc = erase_block(self, i);
        if (rc) { return rc; }
    }
    return 0;
}


// --- Device add/remove tracking -----------------------------------

static const char * device_prefix(const struct jsdrv_union_s * value) {
    if (value->type == JSDRV_UNION_STR) {
        return value->value.str;
    } else if (value->type == JSDRV_UNION_BIN && value->value.bin) {
        return (const char *) value->value.bin;
    }
    return NULL;
}

static void on_device_add(void * user_data, const char * topic,
                          const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const char * p = device_prefix(value);
    if (p && target_prefix_ && jsdrv_cstr_starts_with(p, target_prefix_)) {
        target_present_ = true;
    }
}

static void on_device_remove(void * user_data, const char * topic,
                             const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const char * p = device_prefix(value);
    if (p && target_prefix_ && jsdrv_cstr_starts_with(p, target_prefix_)) {
        target_present_ = false;
    }
}

static int wait_for_presence(bool desired, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!quit_ && waited < timeout_ms) {
        if (target_present_ == desired) { return 0; }
        jsdrv_thread_sleep_ms(10);
        waited += 10;
    }
    return 1;
}


// --- Reset (soft and power-cycle) ---------------------------------

static int publish_str(struct app_s * self, const char * device,
                       const char * subtopic, const char * value) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, device);
    jsdrv_topic_append(&topic, subtopic);
    return jsdrv_publish(self->context, topic.topic,
                         &jsdrv_union_cstr_r(value),
                         JSDRV_TIMEOUT_MS_DEFAULT);
}

// Soft reset via `h/fwup/ctrl/!cmd` LAUNCH op.  The device-side fwup
// task treats LAUNCH as "set boot cmd, send response, reset" (see
// minibitty/src/tasks/fwup.c MB_FWUP_OP_LAUNCH).  Target slot 0 asks
// the bootloader to launch the primary image after reset; if that
// slot is empty/invalid the bootloader's fallback path still runs,
// which is a valid scenario for this test.
static int soft_reset(struct app_s * self) {
    // fwup_ctrl_cmd_s: transaction_id(u32), op(u8), image_slot(u8),
    // pipeline_depth(u8), rsv(u8).
    uint8_t cmd[8];
    static uint32_t txn_id = 0;
    ++txn_id;
    memcpy(cmd, &txn_id, 4);
    cmd[4] = 2;       // FWUP_CTRL_OP_LAUNCH
    cmd[5] = 0;       // image_slot
    cmd[6] = 0;       // pipeline_depth
    cmd[7] = 0;       // rsv

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/fwup/ctrl/!cmd");
    // Fire-and-forget: the device replies to !rsp and then resets
    // ~100 ms later, so the publish's own ACK path can race with
    // the reset and return TIMED_OUT.  We verify the reset landed
    // by watching device add/remove events instead.
    int32_t rc = jsdrv_publish(self->context, topic.topic,
                               &jsdrv_union_bin(cmd, sizeof(cmd)),
                               0);
    if (rc) {
        printf("  WARN: soft reset publish failed: %d\n", rc);
    }
    if (wait_for_presence(false, 2500)) {
        printf("  ERROR: target did not disappear after soft reset\n");
        return 1;
    }
    if (wait_for_presence(true, 5000)) {
        printf("  ERROR: target did not re-enumerate after soft reset\n");
        return 1;
    }
    jsdrv_thread_sleep_ms(500);
    return 0;
}

static int power_cycle(struct app_s * self, const char * ps_topic) {
    int rc;
    publish_str(self, ps_topic, "s/i/range/mode", "off");
    rc = wait_for_presence(false, 2500);
    if (rc) {
        printf("  WARN: target did not disappear after power OFF\n");
    }
    jsdrv_thread_sleep_ms(500);

    publish_str(self, ps_topic, "s/i/range/mode", "manual");
    rc = wait_for_presence(true, 5000);
    if (rc) {
        printf("  ERROR: target did not appear after power ON\n");
        return 1;
    }
    // Small settle delay so the enumeration completes.
    jsdrv_thread_sleep_ms(500);
    return 0;
}



// --- Fwup ----------------------------------------------------------

static void on_fwup_status(void * user_data, const char * topic,
                           const struct jsdrv_union_s * value) {
    (void) user_data;
    if (value->type != JSDRV_UNION_STR) { return; }
    if (!strstr(topic, "/status")) { return; }
    // Pull out "state":"..." for logging.
    const char * s = strstr(value->value.str, "\"state\":\"");
    if (s) {
        s += strlen("\"state\":\"");
        size_t i = 0;
        while (s[i] && s[i] != '"' && i + 1 < sizeof(fwup_state_)) {
            fwup_state_[i] = s[i]; ++i;
        }
        fwup_state_[i] = '\0';
    }
    if (strstr(value->value.str, "\"state\":\"DONE\"")) {
        fwup_rc_ = 0;
        fwup_done_ = 1;
    } else if (strstr(value->value.str, "\"state\":\"ERROR\"")) {
        fwup_rc_ = 1;
        fwup_done_ = 1;
    }
}

static int fwup_subscribe(struct app_s * self) {
    if (fwup_subscribed_) { return 0; }
    int32_t rc = jsdrv_subscribe(self->context, "fwup/",
                                 JSDRV_SFLAG_PUB,
                                 on_fwup_status, self, 0);
    if (rc == 0) { fwup_subscribed_ = 1; }
    return rc;
}

static void fwup_unsubscribe(struct app_s * self) {
    if (fwup_subscribed_) {
        jsdrv_unsubscribe(self->context, "fwup/",
                          on_fwup_status, self, 0);
        fwup_subscribed_ = 0;
    }
}

static int fwup_run(struct app_s * self, const uint8_t * zip_data,
                    uint32_t zip_size, uint32_t timeout_ms) {
    uint32_t hdr_size = sizeof(struct jsdrv_fwup_add_header_s);
    uint32_t payload_size = hdr_size + zip_size;
    uint8_t * payload = (uint8_t *) malloc(payload_size);
    if (!payload) { return 1; }

    struct jsdrv_fwup_add_header_s * hdr =
        (struct jsdrv_fwup_add_header_s *) payload;
    memset(hdr, 0, hdr_size);
    jsdrv_cstr_copy(hdr->device_prefix, self->device.topic,
                    sizeof(hdr->device_prefix));
    hdr->flags = 0;
    hdr->zip_size = zip_size;
    memcpy(payload + hdr_size, zip_data, zip_size);

    fwup_done_ = 0;
    fwup_rc_ = 0;
    fwup_state_[0] = '\0';

    int32_t rc = jsdrv_publish(self->context, JSDRV_FWUP_MGR_TOPIC_ADD,
                               &jsdrv_union_bin(payload, payload_size),
                               10000);
    free(payload);
    if (rc) {
        printf("  ERROR: fwup publish failed: %d\n", rc);
        return rc;
    }

    uint32_t waited = 0;
    while (!quit_ && !fwup_done_ && waited < timeout_ms) {
        jsdrv_thread_sleep_ms(100);
        waited += 100;
    }
    if (!fwup_done_) {
        printf("  ERROR: fwup timeout at %u ms (state=%s)\n",
               waited, fwup_state_);
        return 1;
    }
    return fwup_rc_;
}


// --- Verify --------------------------------------------------------

static int verify_version_topic(struct app_s * self, const char * subtopic) {
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, subtopic);
    struct jsdrv_union_s v = jsdrv_union_null();
    int32_t rc = jsdrv_query(self->context, topic.topic, &v, 1000);
    if (rc) {
        printf("  verify: %s query failed: %d\n", topic.topic, rc);
        return rc;
    }
    if (v.type == JSDRV_UNION_U32) {
        char s[32];
        jsdrv_version_u32_to_str(v.value.u32, s, sizeof(s));
        printf("  verify: %s = %s\n", topic.topic, s);
    } else {
        printf("  verify: %s responded (type=%d)\n", topic.topic, v.type);
    }
    return 0;
}

static int verify_device(struct app_s * self) {
    // c/fw/version is a topic whose string must resolve through the
    // controller pubsub metadata, so a successful query proves the
    // metadata blob loaded correctly.
    return verify_version_topic(self, "c/fw/version");
}


// --- Iteration -----------------------------------------------------

struct sweep_ctx_s {
    struct app_s * self;
    const struct cfg_s * cfg;
    const char * power_topic;
    const uint8_t * zip_data;
    uint32_t zip_size;
};

static int iterate_once(struct sweep_ctx_s * ctx, uint32_t mask,
                        struct result_s * r) {
    struct app_s * self = ctx->self;
    const struct cfg_s * cfg = ctx->cfg;
    int32_t rc;

    memset(r, 0, sizeof(*r));
    r->mask = mask;

    // Match DUT (may fail if device not enumerated).
    rc = app_match(self, cfg->target_filter);
    if (rc) {
        printf("  ERROR: target device not found\n");
        r->open_rc = rc;
        return 1;
    }

    // Phase 1: open prior-state device, erase selected blocks.
    int use_power_cycle = (ctx->power_topic && ctx->power_topic[0]);
    int use_soft_reset = (!use_power_cycle && cfg->do_reset);
    int do_reset = use_power_cycle || use_soft_reset;

    int64_t t0 = jsdrv_time_utc();
    int erase_rc = jsdrv_open(self->context, self->device.topic,
                              JSDRV_DEVICE_OPEN_MODE_RESUME,
                              cfg->open_timeout_ms);
    int device_open = (erase_rc == 0);
    if (erase_rc) {
        printf("  WARN: pre-erase open failed (%d); skipping erase\n",
               erase_rc);
    } else {
        int setup_rc = mem_setup(self);
        if (setup_rc == 0) {
            erase_rc = erase_subset(self, mask);
        }
        mem_teardown(self);
    }
    // For power cycle we need the device closed before cutting power.
    // For soft reset we leave it open so LAUNCH can be delivered.
    if (device_open && use_power_cycle) {
        jsdrv_close(self->context, self->device.topic, 5000);
        device_open = 0;
    }
    r->erase_ms = ms_since(t0);
    if (erase_rc) {
        // Keep going — the whole point is to test recovery from
        // broken states, so proceed even if erase was incomplete.
        printf("  WARN: erase returned %d; continuing\n", erase_rc);
    }

    // Phase 2 (optional): reset between erase and fwup.
    // Without --reset or --power, the device keeps running with its
    // in-RAM metadata while flash is erased; fwup will re-read the
    // flash during its own open and restore.  With a reset, we force
    // a cold boot so fwup sees a freshly-booted broken device.
    if (do_reset) {
        int64_t t_pc = jsdrv_time_utc();
        if (use_power_cycle) {
            rc = power_cycle(self, ctx->power_topic);
        } else if (device_open) {
            rc = soft_reset(self);
            jsdrv_close(self->context, self->device.topic, 5000);
            device_open = 0;
        } else {
            // No soft-reset path available; device is closed.  Wait
            // briefly in case something else brings it back.
            printf("  WARN: cannot soft-reset (device not open); "
                   "waiting on re-enum\n");
            rc = wait_for_presence(true, 5000);
        }
        r->power_cycle_ms = ms_since(t_pc);
        if (rc) {
            r->open_rc = rc;
            return 1;
        }

        // Rematch after re-enumeration.
        rc = app_match(self, cfg->target_filter);
        if (rc) {
            printf("  ERROR: target did not re-enumerate after reset\n");
            r->open_rc = rc;
            return 1;
        }

        // Phase 3: measure device open (system-under-test on a
        // freshly-booted broken device).
        int64_t t_open = jsdrv_time_utc();
        r->open_rc = jsdrv_open(self->context, self->device.topic,
                                JSDRV_DEVICE_OPEN_MODE_RESUME,
                                cfg->open_timeout_ms);
        r->open_ms = ms_since(t_open);
        jsdrv_close(self->context, self->device.topic, 5000);
    } else if (device_open) {
        // No reset requested: just close cleanly so fwup can open.
        jsdrv_close(self->context, self->device.topic, 5000);
        device_open = 0;
    }

    // Phase 4: fwup (restore + under-test path).
    int64_t t_fw = jsdrv_time_utc();
    r->fwup_rc = fwup_run(self, ctx->zip_data, ctx->zip_size,
                          cfg->fwup_timeout_ms);
    r->fwup_ms = ms_since(t_fw);

    // Phase 5: verify version queries.
    if (r->fwup_rc == 0) {
        rc = jsdrv_open(self->context, self->device.topic,
                        JSDRV_DEVICE_OPEN_MODE_RESUME,
                        cfg->open_timeout_ms);
        if (rc) {
            r->verify_rc = rc;
        } else {
            r->verify_rc = verify_device(self);
            jsdrv_close(self->context, self->device.topic, 5000);
        }
    } else {
        r->verify_rc = -1;  // SKIPPED
    }

    r->pass = (r->open_rc == 0 && r->fwup_rc == 0 && r->verify_rc == 0);
    return 0;
}

static void record_log(const struct result_s * r, uint32_t iter,
                       FILE * jsonl) {
    char names[128];
    mask_to_names(r->mask, names, sizeof(names));
    printf("iter=%u mask=0x%02X subset=[%s]\n"
           "  erase_ms=%u power_cycle_ms=%u open_ms=%u open_rc=%d\n"
           "  fwup_ms=%u fwup_rc=%d verify_rc=%d final=%s\n",
           iter, r->mask, names,
           r->erase_ms, r->power_cycle_ms, r->open_ms, r->open_rc,
           r->fwup_ms, r->fwup_rc, r->verify_rc,
           r->pass ? "pass" : "FAIL");
    if (jsonl) {
        fprintf(jsonl,
                "{\"iter\":%u,\"mask\":\"0x%02X\",\"subset\":\"%s\","
                "\"erase_ms\":%u,\"power_cycle_ms\":%u,"
                "\"open_ms\":%u,\"open_rc\":%d,"
                "\"fwup_ms\":%u,\"fwup_rc\":%d,"
                "\"verify_rc\":%d,\"final\":\"%s\"}\n",
                iter, r->mask, names,
                r->erase_ms, r->power_cycle_ms,
                r->open_ms, r->open_rc,
                r->fwup_ms, r->fwup_rc,
                r->verify_rc, r->pass ? "pass" : "fail");
        fflush(jsonl);
    }
}


// --- Sweep ---------------------------------------------------------

static int run_sweep(struct sweep_ctx_s * ctx,
                     const uint32_t * masks, uint32_t mask_count,
                     FILE * jsonl,
                     uint32_t * failed_out, uint32_t * failed_count) {
    int overall_rc = 0;
    uint32_t iter = 0;
    int64_t t_start = jsdrv_time_utc();
    int64_t deadline =
        t_start + (int64_t) ctx->cfg->max_duration_s * JSDRV_TIME_SECOND;

    for (uint32_t i = 0; i < mask_count; ++i) {
        uint32_t mask = masks[i];
        for (uint32_t r = 0; r < ctx->cfg->repeat; ++r) {
            if (quit_) { return 1; }
            if (jsdrv_time_utc() > deadline) {
                printf("### max-duration reached; stopping ###\n");
                return 1;
            }
            ++iter;
            printf("\n--- iter %u mask=0x%02X (repeat %u/%u) ---\n",
                   iter, mask, r + 1, ctx->cfg->repeat);

            struct result_s res;
            iterate_once(ctx, mask, &res);
            record_log(&res, iter, jsonl);

            if (!res.pass) {
                if (*failed_count < 64) {
                    failed_out[(*failed_count)++] = mask;
                }
                overall_rc = 1;
                if (ctx->cfg->stop_on_fail) {
                    printf("\n### --stop-on-fail set; halting sweep ###\n");
                    return overall_rc;
                }
            }
        }
    }
    return overall_rc;
}


// --- Subset generation --------------------------------------------

static uint32_t gen_subsets(uint32_t depth, uint32_t * out_masks,
                            uint32_t out_cap) {
    // Non-empty subsets of the 6 blocks with popcount <= depth.
    // depth >= 6 => full exhaustive (1..63).
    uint32_t n = 0;
    for (uint32_t m = 1; m < (1u << BLOCK_COUNT); ++m) {
        if (depth < BLOCK_COUNT && popcount6(m) > depth) { continue; }
        if (n < out_cap) { out_masks[n++] = m; }
    }
    return n;
}


// --- Entry point --------------------------------------------------

static int usage(void) {
    printf(
        "usage: minibitty fuzz_fwup [options] <release_zip> [device_filter]\n"
        "\n"
        "Verify JS320 open+fwup recovery for broken data-block subsets.\n"
        "\n"
        "By default, each iteration erases the selected blocks and then\n"
        "invokes fwup to recover; fwup handles its own opens internally.\n"
        "--reset inserts a soft reset (h/fwup/ctrl/!cmd LAUNCH) between\n"
        "erase and fwup so the open path is measured against a freshly\n"
        "booted broken device.  --power <filter> uses a JS220 power\n"
        "cycle instead (overrides --reset) for deeper recovery.\n"
        "\n"
        "Options:\n"
        "  --reset                 Soft-reset target between erase and\n"
        "                            fwup; also measures open time on\n"
        "                            the freshly-booted broken device.\n"
        "  --power <filter>        JS220 power supply filter.  When set,\n"
        "                            each iteration hard power-cycles\n"
        "                            the target via the JS220 instead\n"
        "                            of a soft reset.  Optional.\n"
        "  --depth <N>             Subset Hamming max (default: 1):\n"
        "                            1=singles (6),  2=pairs (21),\n"
        "                            3..6=up through that weight,\n"
        "                            7+=full exhaustive (63).\n"
        "  --subsets <masks>       Comma-separated explicit 6-bit masks\n"
        "                            (hex or decimal), e.g. \"0x12,0x25\".\n"
        "                            Overrides --depth.  Useful for rerun.\n"
        "  --repeat <N>            Run each selected subset N times (default 1).\n"
        "  --stop-on-fail          Halt on first failing iteration.\n"
        "  --open-timeout-ms <N>   Per-open budget (default 10000).\n"
        "  --fwup-timeout-ms <N>   Per-fwup budget (default 180000).\n"
        "  --max-duration-s <N>    Global wall-clock kill (default 7200).\n"
        "  --output <path>         Append JSONL records to this file.\n"
        "  --dry-run               Show plan; do not touch hardware.\n"
        "  -v, --verbose           Per-iteration detail.\n"
        "\n"
        "Block indices: 0=ctrl_fw, 1=ctrl_pubsub, 2=ctrl_trace,\n"
        "               3=sensor_fpga, 4=sensor_pubsub, 5=sensor_trace.\n"
        "Mask bit i selects block i.\n"
    );
    return 1;
}

static int parse_subset_masks(const char * arg, struct cfg_s * cfg) {
    cfg->explicit_subset_count = 0;
    const char * p = arg;
    while (*p && cfg->explicit_subset_count < 64) {
        char buf[16];
        size_t i = 0;
        while (*p && *p != ',' && i + 1 < sizeof(buf)) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        if (*p == ',') { ++p; }
        uint32_t v;
        if (parse_u32(buf, &v) || v == 0 || v > ((1u << BLOCK_COUNT) - 1)) {
            printf("invalid mask: %s\n", buf);
            return 1;
        }
        cfg->explicit_subsets[cfg->explicit_subset_count++] = v;
    }
    return 0;
}

int on_fuzz_fwup(struct app_s * self, int argc, char * argv[]) {
    struct cfg_s cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.depth = 1;
    cfg.open_timeout_ms = 10000;
    cfg.fwup_timeout_ms = 180000;
    cfg.max_duration_s = 7200;
    cfg.repeat = 1;

    while (argc) {
        if (0 == strcmp(argv[0], "--power") && argc > 1) {
            ARG_CONSUME();
            cfg.power_filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--depth") && argc > 1) {
            ARG_CONSUME();
            if (parse_u32(argv[0], &cfg.depth)) { return usage(); }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--subsets") && argc > 1) {
            ARG_CONSUME();
            if (parse_subset_masks(argv[0], &cfg)) { return usage(); }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--repeat") && argc > 1) {
            ARG_CONSUME();
            if (parse_u32(argv[0], &cfg.repeat) || cfg.repeat == 0) {
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--reset")) {
            cfg.do_reset = 1;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--stop-on-fail")) {
            cfg.stop_on_fail = 1;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--open-timeout-ms") && argc > 1) {
            ARG_CONSUME();
            if (parse_u32(argv[0], &cfg.open_timeout_ms)) { return usage(); }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--fwup-timeout-ms") && argc > 1) {
            ARG_CONSUME();
            if (parse_u32(argv[0], &cfg.fwup_timeout_ms)) { return usage(); }
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--max-duration-s") && argc > 1) {
            ARG_CONSUME();
            uint32_t v;
            if (parse_u32(argv[0], &v)) { return usage(); }
            cfg.max_duration_s = v;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--output") && argc > 1) {
            ARG_CONSUME();
            cfg.output_path = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--dry-run")) {
            cfg.dry_run = 1;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "-v") ||
                   0 == strcmp(argv[0], "--verbose")) {
            cfg.verbose = 1;
            ARG_CONSUME();
        } else if (argv[0][0] == '-') {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        } else if (!cfg.release_zip) {
            cfg.release_zip = argv[0];
            ARG_CONSUME();
        } else if (!cfg.target_filter) {
            cfg.target_filter = argv[0];
            ARG_CONSUME();
        } else {
            printf("unexpected argument: %s\n", argv[0]);
            return usage();
        }
    }

    if (!cfg.release_zip) {
        printf("missing release zip\n");
        return usage();
    }

    // Build the subset list.
    uint32_t masks[64];
    uint32_t mask_count = 0;
    if (cfg.explicit_subset_count) {
        mask_count = cfg.explicit_subset_count;
        memcpy(masks, cfg.explicit_subsets, sizeof(uint32_t) * mask_count);
    } else {
        mask_count = gen_subsets(cfg.depth, masks, 64);
    }

    printf("fuzz_fwup:\n");
    printf("  release: %s\n", cfg.release_zip);
    const char * reset_mode =
        cfg.power_filter ? "JS220 power cycle" :
        cfg.do_reset     ? "soft reset (LAUNCH)" :
                           "none";
    printf("  reset:   %s\n", reset_mode);
    if (cfg.power_filter) {
        printf("  power:   %s\n", cfg.power_filter);
    }
    printf("  target:  %s\n", cfg.target_filter ? cfg.target_filter : "(first)");
    printf("  depth=%u repeat=%u stop_on_fail=%d\n",
           cfg.depth, cfg.repeat, cfg.stop_on_fail);
    printf("  open_timeout_ms=%u fwup_timeout_ms=%u max_duration_s=%" PRIu64 "\n",
           cfg.open_timeout_ms, cfg.fwup_timeout_ms,
           (uint64_t) cfg.max_duration_s);
    printf("  subsets (%u):", mask_count);
    for (uint32_t i = 0; i < mask_count; ++i) {
        printf(" 0x%02X", masks[i]);
    }
    printf("\n");

    if (cfg.dry_run) {
        printf("\n(dry run — no hardware actions)\n");
        return 0;
    }

    // Load release zip.
    uint32_t zip_size = 0;
    uint8_t * zip_data = file_read(cfg.release_zip, &zip_size);
    if (!zip_data) {
        printf("ERROR: could not read release zip: %s\n", cfg.release_zip);
        return 1;
    }

    struct jsdrv_topic_s power_topic;
    jsdrv_topic_clear(&power_topic);
    if (cfg.power_filter) {
        ROE(app_match(self, cfg.power_filter));
        jsdrv_topic_set(&power_topic, self->device.topic);
        printf("Power device: %s\n", power_topic.topic);
    }

    // Target prefix for add/remove tracking:
    target_prefix_ = cfg.target_filter ? cfg.target_filter : "u/js320/";

    // Initial target presence.
    target_present_ = (0 == app_match(self, cfg.target_filter));

    // Subscribe to add/remove events.
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                        JSDRV_SFLAG_PUB, on_device_add, self, 0));
    ROE(jsdrv_subscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                        JSDRV_SFLAG_PUB, on_device_remove, self, 0));

    int32_t rc = 0;
    if (cfg.power_filter) {
        rc = jsdrv_open(self->context, power_topic.topic,
                        JSDRV_DEVICE_OPEN_MODE_RESUME,
                        JSDRV_TIMEOUT_MS_DEFAULT);
        if (rc) {
            printf("ERROR: power open failed: %d\n", rc);
            free(zip_data);
            return rc;
        }
        publish_str(self, power_topic.topic, "s/i/range/select", "10 A");
    }

    // Subscribe to fwup status updates (once).
    fwup_subscribe(self);

    // Optional JSONL output.
    FILE * jsonl = NULL;
    if (cfg.output_path) {
        jsonl = fopen(cfg.output_path, "a");
        if (!jsonl) {
            printf("WARN: could not open output file: %s\n", cfg.output_path);
        }
    }

    struct sweep_ctx_s ctx = {
        .self = self,
        .cfg = &cfg,
        .power_topic = cfg.power_filter ? power_topic.topic : NULL,
        .zip_data = zip_data,
        .zip_size = zip_size,
    };

    uint32_t failed[64];
    uint32_t failed_count = 0;
    int sweep_rc = run_sweep(&ctx, masks, mask_count, jsonl,
                             failed, &failed_count);

    // Teardown.
    fwup_unsubscribe(self);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_ADD,
                      on_device_add, self, 0);
    jsdrv_unsubscribe(self->context, JSDRV_MSG_DEVICE_REMOVE,
                      on_device_remove, self, 0);
    if (cfg.power_filter) {
        jsdrv_close(self->context, power_topic.topic,
                    JSDRV_TIMEOUT_MS_DEFAULT);
    }
    if (mem_.sem) {
        jsdrv_os_sem_free(mem_.sem);
        mem_.sem = NULL;
    }
    if (jsonl) { fclose(jsonl); }
    free(zip_data);

    // Summary.
    printf("\n=== fuzz_fwup summary ===\n");
    printf("  iterations: %u\n", mask_count * cfg.repeat);
    printf("  failures:   %u\n", failed_count);
    if (failed_count) {
        printf("  FAILED masks:");
        for (uint32_t i = 0; i < failed_count; ++i) {
            printf(" 0x%02X", failed[i]);
        }
        printf("\n  rerun: minibitty fuzz_fwup ");
        if (cfg.power_filter) {
            printf("--power %s ", cfg.power_filter);
        } else if (cfg.do_reset) {
            printf("--reset ");
        }
        printf("--subsets ");
        for (uint32_t i = 0; i < failed_count; ++i) {
            printf("%s0x%02X", (i ? "," : ""), failed[i]);
        }
        printf(" %s", cfg.release_zip);
        if (cfg.target_filter) { printf(" %s", cfg.target_filter); }
        printf("\n");
    }

    return sweep_rc;
}
