

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


enum js320_jtag_events_e {
    JS320_JTAG_EVENT_NONE = 0,
    JS320_JTAG_EVENT_ERROR = 1,         // error indication
    JS320_JTAG_EVENT_GOTO_STATE = 2,    // use STATE_TEST_LOGIC_RESET to reset TAP (but not device)
    JS320_JTAG_EVENT_WAIT = 3,
    JS320_JTAG_EVENT_SHIFT = 4,
};

struct jtag_hdr_s {
    uint32_t id;
    uint8_t cmd;
    uint8_t arg1;
    uint16_t arg0;
};

struct op_s {
    struct jtag_hdr_s hdr;
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
    const struct jtag_hdr_s * hdr = (struct jtag_hdr_s *) value->value.bin;
    if (value->type != JSDRV_UNION_BIN) {
        fprintf(stderr, "type mismatch\n");
        return;
    }
    if (hdr->id != op_.hdr.id) {
        fprintf(stderr, "id mismatch: %d %d\n", hdr->id, op_.hdr.id);
        return;
    }
    if (hdr->cmd != op_.hdr.cmd) {
        fprintf(stderr, "cmd mismatch\n");
        return;
    }
    const uint8_t * data = value->value.bin + sizeof(*hdr);

    switch (hdr->cmd) {
        case JS320_JTAG_EVENT_GOTO_STATE: break;
        case JS320_JTAG_EVENT_WAIT: break;
        case JS320_JTAG_EVENT_SHIFT: {
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

    uint8_t payload[512];
    uint32_t offset = 0;  // in bytes
    op_.hdr.cmd = JS320_JTAG_EVENT_SHIFT;
    op_.hdr.arg1 = 0;

    while (data_bits) {
        op_.hdr.id++;
        op_.done = false;
        uint32_t bits = data_bits;
        if (data_bits > BITS_SHIFT_MAX) {
            bits = BITS_SHIFT_MAX;
        } else {
            op_.hdr.arg1 = must_end ? 1 : 0;
        }
        op_.hdr.arg0 = bits;
        uint32_t payload_size = (bits + 7) / 8;
        op_.data = output_data + offset;
        op_.size = payload_size;
        memcpy(payload, &op_.hdr, sizeof(op_.hdr));
        memcpy(payload + sizeof(op_.hdr), input_data + offset, payload_size);

        uint32_t sz = sizeof(op_.hdr) + payload_size;
        jsdrv_publish(context, topic_cmd_.topic, &jsdrv_union_bin(payload, sz), 0);
        while (!op_.done) {
            Sleep(1);  // todo
        }
        offset += payload_size;
        data_bits -= bits;
    }
}

void jtag_wait_time(uint32_t microseconds) {
    op_.hdr.id++;
    uint8_t data[16];
    if (microseconds > (16 * 8)) {
        Sleep(10);
        microseconds = 16 * 8;
    }
    jtag_tap_shift(data, data, microseconds, false);

    /*
    op_.hdr.cmd = JS320_JTAG_EVENT_WAIT;
    op_.hdr.arg0 = microseconds;
    op_.hdr.arg1 = 0;
    */
}

void jtag_go_to_state(unsigned state) {
    op_.hdr.id++;
    op_.hdr.cmd = JS320_JTAG_EVENT_GOTO_STATE;
    op_.hdr.arg0 = state;
    op_.hdr.arg1 = 0;
    jsdrv_publish(context, topic_cmd_.topic, &jsdrv_union_bin((uint8_t *) &op_.hdr, sizeof(op_.hdr)), 0);
}
