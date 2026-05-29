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
 * @brief minibitty subcommand: JS320 calibration.
 */

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"
#include "jsdrv/os_thread.h"
#include "jsdrv_prv/devices/js320/js320_cal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static volatile bool cal_done_;
static volatile int32_t cal_rsp_status_;
static volatile uint32_t cal_rsp_txn_id_;
static uint8_t cal_record_[JSDRV_CAL_RECORD_SIZE];
static volatile bool cal_record_received_;
static const char * cal_out_path_;
static uint32_t cal_txn_id_;


static int usage(void) {
    printf(
        "usage: minibitty cal <subcmd> [args] [device_filter]\n"
        "\n"
        "Subcommands:\n"
        "  read   <slot> [--out <path>] [device_filter]\n"
        "         Read the 1024-byte calibration record from slot.\n"
        "  copy   <src_slot> <dst_slot> [device_filter]\n"
        "         Copy src_slot -> dst_slot.  Allowed transitions:\n"
        "           any -> ACTIVE\n"
        "           ACTIVE -> TRIM1\n"
        "           ACTIVE -> TRIM2\n"
        "  offset_current [--samples N] [device_filter]\n"
        "         Sweep upper-triangular shunt/mux, measure current ADC\n"
        "         offsets for ADC0/1/2, write ACTIVE.  Caller must arrange\n"
        "         a zero-current input state (open at the current jacks).\n"
        "  offset_voltage [--samples N] [device_filter]\n"
        "         Measure voltage ADC offsets at both ranges (15V/2V),\n"
        "         write ACTIVE.  Caller must short v+ to v-.\n"
        "\n"
        "Slot tokens: ACTIVE | TRIM1 | TRIM2 | FIELD | LAB | FACTORY\n"
    );
    return 1;
}


static int slot_from_string(const char * s, uint8_t * out) {
    if (0 == strcmp(s, "ACTIVE"))  { *out = JSDRV_CAL_SLOT_ACTIVE;  return 0; }
    if (0 == strcmp(s, "TRIM2"))   { *out = JSDRV_CAL_SLOT_TRIM2;   return 0; }
    if (0 == strcmp(s, "TRIM1"))   { *out = JSDRV_CAL_SLOT_TRIM1;   return 0; }
    if (0 == strcmp(s, "FIELD"))   { *out = JSDRV_CAL_SLOT_FIELD;   return 0; }
    if (0 == strcmp(s, "LAB"))     { *out = JSDRV_CAL_SLOT_LAB;     return 0; }
    if (0 == strcmp(s, "FACTORY")) { *out = JSDRV_CAL_SLOT_FACTORY; return 0; }
    return 1;
}

static const char * slot_to_string(uint8_t slot) {
    switch (slot) {
        case JSDRV_CAL_SLOT_ACTIVE:  return "ACTIVE";
        case JSDRV_CAL_SLOT_TRIM2:   return "TRIM2";
        case JSDRV_CAL_SLOT_TRIM1:   return "TRIM1";
        case JSDRV_CAL_SLOT_FIELD:   return "FIELD";
        case JSDRV_CAL_SLOT_LAB:     return "LAB";
        case JSDRV_CAL_SLOT_FACTORY: return "FACTORY";
        default:                     return "?";
    }
}


static void on_status(void * user_data, const char * topic,
                      const struct jsdrv_union_s * value) {
    (void) user_data;
    if (value->type != JSDRV_UNION_STR) {
        return;
    }
    if (!strstr(topic, "/h/cal/!status")) {
        return;
    }
    printf("  [status] %s\n", value->value.str);
}

static void on_data(void * user_data, const char * topic,
                    const struct jsdrv_union_s * value) {
    (void) user_data;
    if (value->type != JSDRV_UNION_BIN) {
        return;
    }
    if (!strstr(topic, "/h/cal/!data")) {
        return;
    }
    uint32_t copy = value->size;
    if (copy > JSDRV_CAL_RECORD_SIZE) {
        copy = JSDRV_CAL_RECORD_SIZE;
    }
    memcpy(cal_record_, value->value.bin, copy);
    cal_record_received_ = true;
}

static void on_rsp(void * user_data, const char * topic,
                   const struct jsdrv_union_s * value) {
    (void) user_data;
    if (!strstr(topic, "/h/cal/!rsp")) {
        return;
    }
    if (value->type != JSDRV_UNION_BIN
            || value->size < sizeof(struct jsdrv_cal_rsp_s)) {
        cal_rsp_status_ = JSDRV_ERROR_MESSAGE_INTEGRITY;
        cal_done_ = true;
        return;
    }
    const struct jsdrv_cal_rsp_s * rsp =
        (const struct jsdrv_cal_rsp_s *) value->value.bin;
    cal_rsp_txn_id_ = rsp->transaction_id;
    cal_rsp_status_ = rsp->status;
    cal_done_ = true;
}


static void print_record_summary(const uint8_t * rec) {
    static const uint8_t MAGIC[16] = {
        0x4a, 0x53, 0x33, 0x32, 0x30, 0x63, 0x61, 0x6c,
        0x0d, 0x0a, 0x20, 0x0a, 0x1a, 0xb2, 0x1c, 0x00
    };
    bool magic_ok = (0 == memcmp(rec, MAGIC, 16));
    uint8_t format_version = rec[16];
    uint16_t vendor_id  = (uint16_t)(rec[18] | (rec[19] << 8));
    uint16_t product_id = (uint16_t)(rec[20] | (rec[21] << 8));
    char serial[9] = {0};
    memcpy(serial, rec + 24, 8);
    int64_t create_time = 0;
    for (int i = 0; i < 8; ++i) {
        create_time |= ((int64_t)(rec[32 + i])) << (i * 8);
    }
    char source_info[33] = {0};
    memcpy(source_info, rec + 40, 32);
    uint32_t cal_source_version =
        (uint32_t)(rec[72]) | ((uint32_t)(rec[73]) << 8) |
        ((uint32_t)(rec[74]) << 16) | ((uint32_t)(rec[75]) << 24);
    char sig_magic[17] = {0};
    memcpy(sig_magic, rec + 952, 16);
    bool sig_is_jsdrv = (0 == memcmp(rec + 952, "JSDRV_OFFSET_CAL", 16));
    uint32_t check =
        (uint32_t)(rec[1020]) | ((uint32_t)(rec[1021]) << 8) |
        ((uint32_t)(rec[1022]) << 16) | ((uint32_t)(rec[1023]) << 24);

    printf("  header_magic     : %s\n", magic_ok ? "ok" : "BAD");
    printf("  format_version   : %u\n", (unsigned) format_version);
    printf("  vendor_id        : 0x%04x\n", vendor_id);
    printf("  product_id       : 0x%04x\n", product_id);
    printf("  serial_number    : %s\n", serial);
    printf("  create_time      : %lld\n", (long long) create_time);
    printf("  source_info      : %s\n", source_info);
    printf("  cal_source_ver   : 0x%08x\n", (unsigned) cal_source_version);
    printf("  signature[0..15] : '%s'%s\n", sig_magic,
           sig_is_jsdrv ? " (driver-generated)" : "");
    printf("  check32          : 0x%08x\n", (unsigned) check);
}


/// Subscribe to cal status/data/rsp under the device prefix, publish
/// the command, wait for !rsp, then unsubscribe.
static int run_cmd(struct app_s * self, struct jsdrv_cal_cmd_s * cmd) {
    struct jsdrv_topic_s topic;

    cal_done_ = false;
    cal_rsp_status_ = 0;
    cal_rsp_txn_id_ = 0;
    cal_record_received_ = false;
    memset(cal_record_, 0, sizeof(cal_record_));
    cmd->transaction_id = ++cal_txn_id_;

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!status");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_status, self, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!data");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_data, self, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB,
                    on_rsp, self, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!cmd");
    int32_t rc = jsdrv_publish(self->context, topic.topic,
        &jsdrv_union_bin((const uint8_t *) cmd, sizeof(*cmd)), 0);
    if (rc) {
        printf("ERROR: cal !cmd publish failed: %d\n", (int) rc);
        goto teardown;
    }

    while (!quit_ && !cal_done_) {
        jsdrv_thread_sleep_ms(50);
    }
    if (!cal_done_) {
        rc = JSDRV_ERROR_ABORTED;
        goto teardown;
    }
    rc = cal_rsp_status_;

teardown:
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!status");
    jsdrv_unsubscribe(self->context, topic.topic, on_status, self, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!data");
    jsdrv_unsubscribe(self->context, topic.topic, on_data, self, 0);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/cal/!rsp");
    jsdrv_unsubscribe(self->context, topic.topic, on_rsp, self, 0);
    return rc;
}


static int open_device(struct app_s * self, char * device_filter) {
    ROE(app_match(self, device_filter));
    printf("Target device: %s\n", self->device.topic);
    int32_t rc = jsdrv_open(self->context, self->device.topic,
                            JSDRV_DEVICE_OPEN_MODE_RESUME,
                            JSDRV_TIMEOUT_MS_DEFAULT);
    if (rc) {
        printf("ERROR: jsdrv_open failed: %d\n", (int) rc);
        return rc;
    }
    return 0;
}

static void close_device(struct app_s * self) {
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
}


static int on_cal_read(struct app_s * self, int argc, char * argv[]) {
    if (argc < 1) return usage();
    uint8_t slot = 0;
    if (slot_from_string(argv[0], &slot)) {
        printf("ERROR: bad slot '%s'\n", argv[0]);
        return usage();
    }
    ARG_CONSUME();

    cal_out_path_ = NULL;
    char * device_filter = NULL;
    while (argc) {
        if (0 == strcmp(argv[0], "--out") && argc >= 2) {
            cal_out_path_ = argv[1];
            ARG_CONSUME();
            ARG_CONSUME();
        } else if (argv[0][0] == '-') {
            printf("Unknown option: %s\n", argv[0]);
            return usage();
        } else {
            device_filter = argv[0];
            ARG_CONSUME();
        }
    }

    ROE(open_device(self, device_filter));

    struct jsdrv_cal_cmd_s cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = JSDRV_CAL_OP_SLOT_READ;
    cmd.src_slot = slot;
    int rc = run_cmd(self, &cmd);
    close_device(self);

    if (rc == 0 && cal_record_received_) {
        printf("\nCalibration record (1024 bytes):\n");
        print_record_summary(cal_record_);
        if (cal_out_path_) {
            FILE * f = fopen(cal_out_path_, "wb");
            if (!f) {
                printf("ERROR: cannot open %s for write\n", cal_out_path_);
                return 1;
            }
            size_t n = fwrite(cal_record_, 1, JSDRV_CAL_RECORD_SIZE, f);
            fclose(f);
            if (n != JSDRV_CAL_RECORD_SIZE) {
                printf("ERROR: short write to %s (%zu/%u)\n",
                       cal_out_path_, n, (unsigned) JSDRV_CAL_RECORD_SIZE);
                return 1;
            }
            printf("  -> wrote %s (%u bytes)\n",
                   cal_out_path_, (unsigned) JSDRV_CAL_RECORD_SIZE);
        }
    } else if (rc != 0) {
        printf("ERROR: slot_read failed: %d\n", rc);
    }
    return rc;
}


static int on_cal_copy(struct app_s * self, int argc, char * argv[]) {
    if (argc < 2) return usage();
    uint8_t src_slot = 0;
    uint8_t dst_slot = 0;
    if (slot_from_string(argv[0], &src_slot)) {
        printf("ERROR: bad src_slot '%s'\n", argv[0]);
        return usage();
    }
    if (slot_from_string(argv[1], &dst_slot)) {
        printf("ERROR: bad dst_slot '%s'\n", argv[1]);
        return usage();
    }
    ARG_CONSUME();
    ARG_CONSUME();

    char * device_filter = NULL;
    while (argc) {
        if (argv[0][0] == '-') {
            printf("Unknown option: %s\n", argv[0]);
            return usage();
        }
        device_filter = argv[0];
        ARG_CONSUME();
    }

    ROE(open_device(self, device_filter));
    printf("Copy: %s -> %s\n",
           slot_to_string(src_slot), slot_to_string(dst_slot));

    struct jsdrv_cal_cmd_s cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = JSDRV_CAL_OP_SLOT_COPY;
    cmd.src_slot = src_slot;
    cmd.dst_slot = dst_slot;
    int rc = run_cmd(self, &cmd);
    close_device(self);
    if (rc) {
        printf("ERROR: slot_copy failed: %d\n", rc);
    }
    return rc;
}


static int on_cal_offset(struct app_s * self, int argc, char * argv[],
                         uint8_t op, const char * label) {
    uint32_t samples_per_point = 0;
    char * device_filter = NULL;
    while (argc) {
        if (0 == strcmp(argv[0], "--samples") && argc >= 2) {
            samples_per_point = (uint32_t) strtoul(argv[1], NULL, 0);
            ARG_CONSUME();
            ARG_CONSUME();
        } else if (argv[0][0] == '-') {
            printf("Unknown option: %s\n", argv[0]);
            return usage();
        } else {
            device_filter = argv[0];
            ARG_CONSUME();
        }
    }

    ROE(open_device(self, device_filter));
    printf("Offset cal: %s (samples=%u)\n", label,
           samples_per_point ? samples_per_point : 200000);

    struct jsdrv_cal_cmd_s cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = op;
    cmd.samples_per_point = samples_per_point;
    int rc = run_cmd(self, &cmd);
    close_device(self);
    if (rc) {
        printf("ERROR: %s failed: %d\n", label, rc);
    }
    return rc;
}


int on_cal(struct app_s * self, int argc, char * argv[]) {
    if (argc < 1) return usage();
    char * subcmd = argv[0];
    ARG_CONSUME();

    if (0 == strcmp(subcmd, "read")) {
        return on_cal_read(self, argc, argv);
    }
    if (0 == strcmp(subcmd, "copy")) {
        return on_cal_copy(self, argc, argv);
    }
    if (0 == strcmp(subcmd, "offset_current")) {
        return on_cal_offset(self, argc, argv,
                             JSDRV_CAL_OP_CURRENT_OFFSET, "offset_current");
    }
    if (0 == strcmp(subcmd, "offset_voltage")) {
        return on_cal_offset(self, argc, argv,
                             JSDRV_CAL_OP_VOLTAGE_OFFSET, "offset_voltage");
    }
    printf("Unknown cal subcommand: %s\n", subcmd);
    return usage();
}
