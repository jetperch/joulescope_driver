

#include "jtag.h"
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/topic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>   // todo remove


#define MAX_DEVICES_LENGTH (4096U)
#define BITS_SHIFT_MAX (256U * 8)


enum js320_jtag_op_e {
    JS320_JTAG_OP_GOTO_STATE    = 1,
    JS320_JTAG_OP_SHIFT         = 2,
};

struct js320_jtag_cmd_s {
    uint32_t transaction_id;
    uint8_t operation;
    uint8_t flags;
    uint16_t timeout_ms;
    uint32_t offset;
    uint32_t length;
    uint32_t delay_us;
    uint32_t rsv1_u32;
};

struct op_s {
    struct js320_jtag_cmd_s cmd;
    uint8_t * data;
    uint32_t size;
    volatile bool done;
};

static struct jsdrv_context_s * context;
static char devices[MAX_DEVICES_LENGTH];
static struct op_s op_;
static struct jsdrv_topic_s topic_cmd_;
static struct jsdrv_topic_s topic_done_;


static void on_done(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct js320_jtag_cmd_s)) {
        fprintf(stderr, "type/size mismatch\n");
        return;
    }
    const struct js320_jtag_cmd_s * rsp = (const struct js320_jtag_cmd_s *) value->value.bin;
    if (rsp->transaction_id != op_.cmd.transaction_id) {
        fprintf(stderr, "id mismatch: %u %u\n", rsp->transaction_id, op_.cmd.transaction_id);
        return;
    }
    if (rsp->operation != op_.cmd.operation) {
        fprintf(stderr, "op mismatch\n");
        return;
    }
    const uint8_t * data = value->value.bin + sizeof(struct js320_jtag_cmd_s);

    switch (rsp->operation) {
        case JS320_JTAG_OP_GOTO_STATE: break;
        case JS320_JTAG_OP_SHIFT: {
            memcpy(op_.data, data, op_.size);
        }
        default: break;
    }
    op_.done = true;
}

void jtag_init(int ifnum, const char *devstr, int clkdiv) {
    (void)ifnum;
    (void)devstr;
    (void)clkdiv;

    int32_t rc = jsdrv_initialize(&context, NULL, 1000);
    if (rc) {
        fprintf(stderr, "jsdrv_initialize failed: %d %s %s\n", rc,
               jsdrv_error_code_name(rc),
               jsdrv_error_code_description(rc));
        exit(1);
    }

    struct jsdrv_union_s devices_value;
    memset(&devices_value, 0, sizeof(devices_value));
    devices_value.type = JSDRV_UNION_STR;
    devices_value.value.str = devices;
    devices_value.size = sizeof(devices);
    rc = jsdrv_query(context, JSDRV_MSG_DEVICE_LIST, &devices_value, 0);
    if (rc) {
        fprintf(stderr, "jsdrv_query failed: %d\n", rc);
        exit(1);
    }

    if (0 == devices[0]) {
        fprintf(stderr, "No devices found\n");
        exit(1);
    }

    rc = jsdrv_open(context, devices, JSDRV_DEVICE_OPEN_MODE_RESUME, 0);
    if (rc) {
        fprintf(stderr, "jsdrv_open failed: %d\n", rc);
        exit(1);
    }

    jsdrv_topic_set(&topic_cmd_, devices);
    jsdrv_topic_append(&topic_cmd_, "c/jtag/!cmd");
    jsdrv_topic_set(&topic_done_, devices);
    jsdrv_topic_append(&topic_done_, "c/jtag/!done");
    jsdrv_subscribe(context, topic_done_.topic, JSDRV_SFLAG_PUB, on_done, NULL, 0);

    Sleep(10);
    jtag_go_to_state(STATE_TEST_LOGIC_RESET);
}

void jtag_deinit(void) {
    jtag_go_to_state(STATE_TEST_LOGIC_RESET);
    jsdrv_unsubscribe(context, topic_done_.topic, on_done, NULL, 0);
    jsdrv_close(context, devices, 100);
    jsdrv_finalize(context, 1000);
    context = NULL;
}

void jtag_tap_shift(
    uint8_t *input_data,
    uint8_t *output_data,
    uint32_t data_bits,
    bool must_end) {

    uint8_t payload[sizeof(struct js320_jtag_cmd_s) + 256];
    uint32_t offset = 0;  // in bytes

    while (data_bits) {
        uint32_t next_id = op_.cmd.transaction_id + 1;
        memset(&op_.cmd, 0, sizeof(op_.cmd));
        op_.cmd.transaction_id = next_id;
        op_.cmd.operation = JS320_JTAG_OP_SHIFT;
        op_.done = false;
        uint32_t bits = data_bits;
        if (data_bits > BITS_SHIFT_MAX) {
            bits = BITS_SHIFT_MAX;
        } else {
            op_.cmd.flags = must_end ? 1 : 0;
        }
        op_.cmd.length = bits;
        uint32_t payload_size = (bits + 7) / 8;
        op_.data = output_data + offset;
        op_.size = payload_size;
        memcpy(payload, &op_.cmd, sizeof(op_.cmd));
        memcpy(payload + sizeof(op_.cmd), input_data + offset, payload_size);

        uint32_t sz = sizeof(op_.cmd) + payload_size;
        jsdrv_publish(context, topic_cmd_.topic, &jsdrv_union_bin(payload, sz), 0);
        while (!op_.done) {
            Sleep(1);  // todo
        }
        offset += payload_size;
        data_bits -= bits;
    }
}

void jtag_wait_time(uint32_t microseconds) {
    uint32_t next_id = op_.cmd.transaction_id + 1;
    memset(&op_.cmd, 0, sizeof(op_.cmd));
    op_.cmd.transaction_id = next_id;
    op_.cmd.operation = JS320_JTAG_OP_GOTO_STATE;
    op_.cmd.offset = STATE_RUN_TEST_IDLE;
    op_.cmd.delay_us = microseconds;
    jsdrv_publish(context, topic_cmd_.topic,
        &jsdrv_union_bin((uint8_t *) &op_.cmd, sizeof(op_.cmd)), 0);
}

void jtag_go_to_state(unsigned state) {
    uint32_t next_id = op_.cmd.transaction_id + 1;
    memset(&op_.cmd, 0, sizeof(op_.cmd));
    op_.cmd.transaction_id = next_id;
    op_.cmd.operation = JS320_JTAG_OP_GOTO_STATE;
    op_.cmd.offset = state;
    jsdrv_publish(context, topic_cmd_.topic,
        &jsdrv_union_bin((uint8_t *) &op_.cmd, sizeof(op_.cmd)), 0);
}
