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

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv/time.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/js320_jtag.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/mb_drv.h"
#include "jsdrv_prv/platform.h"
#include <string.h>


// --- JTAG low-level command interface ---

enum jtag_cmd_e {
    JTAG_CMD_GOTO_STATE = 2,
    JTAG_CMD_WAIT       = 3,
    JTAG_CMD_SHIFT      = 4,
};

enum jtag_tap_state_e {
    TAP_TEST_LOGIC_RESET =  0,
    TAP_RUN_TEST_IDLE    =  1,
    TAP_SHIFT_DR         =  4,
    TAP_SHIFT_IR         = 11,
};

struct jtag_hdr_s {
    uint32_t id;
    uint8_t cmd;
    uint8_t arg1;
    uint16_t arg0;
};

// --- ECP5 constants ---

#define ECP5_LFE5U25_IDCODE            0x41111043

#define ECP5_READ_ID                    0xE0
#define ECP5_ISC_ENABLE                 0xC6
#define ECP5_ISC_DISABLE                0x26
#define ECP5_LSC_READ_STATUS            0x3C
#define ECP5_LSC_INIT_ADDRESS           0x46
#define ECP5_LSC_READ_CIPHER_KEY        0xF4
#define ECP5_LSC_PROG_CIPHER_KEY        0xF3
#define ECP5_LSC_PROG_FEABITS           0xF8
#define ECP5_LSC_READ_FEABITS           0xFB

static const uint8_t ECP5_BLANK_CIPHER_KEY[16] = {
    0xD5, 0x53, 0x51, 0x69, 0x0F, 0xF2, 0x17, 0xAE,
    0x6C, 0xEA, 0xA3, 0x45, 0xB4, 0xF1, 0x43, 0x00
};

// --- AES state machine ---

enum aes_state_e {
    AES_IDLE = 0,
    AES_READ_IDCODE,
    AES_ISC_ENABLE,
    AES_READ_STATUS,
    AES_INIT_ADDRESS,
    AES_READ_CIPHER,
    AES_PROG_CIPHER,
    AES_KEY_BURN_WAIT,
    AES_PROG_EO,
    AES_EO_BURN_WAIT,
    AES_VERIFY_EO,
    AES_PROG_KL,
    AES_KL_BURN_WAIT,
    AES_VERIFY_KL,
    AES_ISC_DISABLE,
    AES_DONE,
};

enum aes_event_e {
    AES_EV_NONE = 0,
    AES_EV_ENTER,
    AES_EV_EXIT,
    AES_EV_CMD,
    AES_EV_JTAG_RSP,
    AES_EV_TIMER,
};

// --- JTAG context ---

struct js320_jtag_s {
    struct jsdrvp_mb_dev_s * dev;

    // AES state machine
    uint8_t aes_state;
    uint32_t aes_transaction_id;
    uint32_t aes_jtag_cmd_id;
    uint32_t aes_flags;
    uint8_t aes_key[16];
    int32_t aes_error_code;
    uint8_t aes_expected_responses;
    uint8_t aes_current_response_idx;
    uint8_t aes_target_response_idx;
    uint8_t aes_response_buf[16];
    uint8_t aes_response_size;
};

// --- State machine types ---

typedef bool (*aes_sm_fn)(struct js320_jtag_s * self, uint8_t event);

struct aes_transition_s {
    uint8_t event;
    uint8_t state_next;
    aes_sm_fn guard;
};

struct aes_state_s {
    uint8_t state;
    const char * name;
    aes_sm_fn on_enter;
    aes_sm_fn on_exit;
    const struct aes_transition_s * transitions;
};

// Forward declarations for on_enter and guard functions
static bool on_read_idcode(struct js320_jtag_s * self, uint8_t event);
static bool on_isc_enable(struct js320_jtag_s * self, uint8_t event);
static bool on_read_status(struct js320_jtag_s * self, uint8_t event);
static bool on_init_address(struct js320_jtag_s * self, uint8_t event);
static bool on_read_cipher(struct js320_jtag_s * self, uint8_t event);
static bool on_prog_cipher(struct js320_jtag_s * self, uint8_t event);
static bool on_key_burn_wait(struct js320_jtag_s * self, uint8_t event);
static bool on_prog_eo(struct js320_jtag_s * self, uint8_t event);
static bool on_eo_burn_wait(struct js320_jtag_s * self, uint8_t event);
static bool on_read_feabits(struct js320_jtag_s * self, uint8_t event);
static bool on_prog_kl(struct js320_jtag_s * self, uint8_t event);
static bool on_kl_burn_wait(struct js320_jtag_s * self, uint8_t event);
static bool on_isc_disable(struct js320_jtag_s * self, uint8_t event);
static bool on_done(struct js320_jtag_s * self, uint8_t event);
static bool guard_idcode_ok(struct js320_jtag_s * self, uint8_t event);
static bool guard_status_ok(struct js320_jtag_s * self, uint8_t event);
static bool guard_cipher_blank(struct js320_jtag_s * self, uint8_t event);
static bool guard_encrypt_only(struct js320_jtag_s * self, uint8_t event);
static bool guard_key_lock(struct js320_jtag_s * self, uint8_t event);
static bool guard_eo_ok_and_kl(struct js320_jtag_s * self, uint8_t event);
static bool guard_eo_ok(struct js320_jtag_s * self, uint8_t event);
static bool guard_kl_ok(struct js320_jtag_s * self, uint8_t event);

// --- AES transition tables ---

static const struct aes_transition_s aes_idle_transitions[] = {
    {AES_EV_CMD, AES_READ_IDCODE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_read_idcode_transitions[] = {
    {AES_EV_JTAG_RSP, AES_ISC_ENABLE, guard_idcode_ok},
    {AES_EV_JTAG_RSP, AES_DONE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_isc_enable_transitions[] = {
    {AES_EV_JTAG_RSP, AES_READ_STATUS, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_read_status_transitions[] = {
    {AES_EV_JTAG_RSP, AES_INIT_ADDRESS, guard_status_ok},
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_init_address_transitions[] = {
    {AES_EV_JTAG_RSP, AES_READ_CIPHER, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_read_cipher_transitions[] = {
    {AES_EV_JTAG_RSP, AES_PROG_CIPHER, guard_cipher_blank},
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_prog_cipher_transitions[] = {
    {AES_EV_JTAG_RSP, AES_KEY_BURN_WAIT, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_key_burn_wait_transitions[] = {
    {AES_EV_TIMER, AES_PROG_EO, guard_encrypt_only},
    {AES_EV_TIMER, AES_PROG_KL, guard_key_lock},
    {AES_EV_TIMER, AES_ISC_DISABLE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_prog_eo_transitions[] = {
    {AES_EV_JTAG_RSP, AES_EO_BURN_WAIT, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_eo_burn_wait_transitions[] = {
    {AES_EV_TIMER, AES_VERIFY_EO, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_verify_eo_transitions[] = {
    {AES_EV_JTAG_RSP, AES_PROG_KL, guard_eo_ok_and_kl},
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, guard_eo_ok},
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_prog_kl_transitions[] = {
    {AES_EV_JTAG_RSP, AES_KL_BURN_WAIT, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_kl_burn_wait_transitions[] = {
    {AES_EV_TIMER, AES_VERIFY_KL, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_verify_kl_transitions[] = {
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, guard_kl_ok},
    {AES_EV_JTAG_RSP, AES_ISC_DISABLE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_isc_disable_transitions[] = {
    {AES_EV_JTAG_RSP, AES_DONE, NULL},
    {0, 0, NULL}
};

static const struct aes_transition_s aes_done_transitions[] = {
    {0, 0, NULL}
};

// --- AES state table ---

static const struct aes_state_s aes_states[] = {
    {AES_IDLE,          "idle",          NULL,              NULL, aes_idle_transitions},
    {AES_READ_IDCODE,   "read_idcode",   on_read_idcode,    NULL, aes_read_idcode_transitions},
    {AES_ISC_ENABLE,    "isc_enable",    on_isc_enable,     NULL, aes_isc_enable_transitions},
    {AES_READ_STATUS,   "read_status",   on_read_status,    NULL, aes_read_status_transitions},
    {AES_INIT_ADDRESS,  "init_address",  on_init_address,   NULL, aes_init_address_transitions},
    {AES_READ_CIPHER,   "read_cipher",   on_read_cipher,    NULL, aes_read_cipher_transitions},
    {AES_PROG_CIPHER,   "prog_cipher",   on_prog_cipher,    NULL, aes_prog_cipher_transitions},
    {AES_KEY_BURN_WAIT, "key_burn_wait", on_key_burn_wait,  NULL, aes_key_burn_wait_transitions},
    {AES_PROG_EO,       "prog_eo",       on_prog_eo,        NULL, aes_prog_eo_transitions},
    {AES_EO_BURN_WAIT,  "eo_burn_wait",  on_eo_burn_wait,   NULL, aes_eo_burn_wait_transitions},
    {AES_VERIFY_EO,     "verify_eo",     on_read_feabits,   NULL, aes_verify_eo_transitions},
    {AES_PROG_KL,       "prog_kl",       on_prog_kl,        NULL, aes_prog_kl_transitions},
    {AES_KL_BURN_WAIT,  "kl_burn_wait",  on_kl_burn_wait,   NULL, aes_kl_burn_wait_transitions},
    {AES_VERIFY_KL,     "verify_kl",     on_read_feabits,   NULL, aes_verify_kl_transitions},
    {AES_ISC_DISABLE,   "isc_disable",   on_isc_disable,    NULL, aes_isc_disable_transitions},
    {AES_DONE,          "done",          on_done,           NULL, aes_done_transitions},
    {0, NULL, NULL, NULL, NULL},
};

// --- State machine engine ---

static void aes_state_transition(struct js320_jtag_s * self, uint8_t state_next) {
    const struct aes_state_s * s_prev = &aes_states[self->aes_state];
    const struct aes_state_s * s_next = &aes_states[state_next];
    JSDRV_LOGD1("AES: %s -> %s", s_prev->name, s_next->name);
    if (s_prev->on_exit) {
        s_prev->on_exit(self, AES_EV_EXIT);
    }
    self->aes_state = state_next;
    if (s_next->on_enter) {
        s_next->on_enter(self, AES_EV_ENTER);
    }
}

static bool aes_transitions_evaluate(struct js320_jtag_s * self, uint8_t state, uint8_t event) {
    const struct aes_transition_s * t = aes_states[state].transitions;
    while (t->event) {
        if (t->event == event) {
            if ((NULL == t->guard) || t->guard(self, event)) {
                aes_state_transition(self, t->state_next);
                return true;
            }
        }
        ++t;
    }
    return false;
}

static void aes_process(struct js320_jtag_s * self, uint8_t event) {
    aes_transitions_evaluate(self, self->aes_state, event);
}

// --- JTAG command helpers ---

static void jtag_goto(struct js320_jtag_s * self, uint8_t state) {
    struct jtag_hdr_s hdr;
    hdr.id = ++self->aes_jtag_cmd_id;
    hdr.cmd = JTAG_CMD_GOTO_STATE;
    hdr.arg1 = 0;
    hdr.arg0 = state;
    jsdrvp_mb_dev_publish_to_device(self->dev, "c/jtag/!cmd",
        &jsdrv_union_bin((uint8_t *) &hdr, sizeof(hdr)));
}

static void jtag_wait_us(struct js320_jtag_s * self, uint16_t microseconds) {
    struct jtag_hdr_s hdr;
    hdr.id = ++self->aes_jtag_cmd_id;
    hdr.cmd = JTAG_CMD_WAIT;
    hdr.arg1 = 0;
    hdr.arg0 = microseconds;
    jsdrvp_mb_dev_publish_to_device(self->dev, "c/jtag/!cmd",
        &jsdrv_union_bin((uint8_t *) &hdr, sizeof(hdr)));
}

static void jtag_shift(struct js320_jtag_s * self, const uint8_t * data,
                        uint16_t bits, bool must_end) {
    uint8_t buf[sizeof(struct jtag_hdr_s) + 32];
    uint32_t data_bytes = (bits + 7) / 8;
    struct jtag_hdr_s * hdr = (struct jtag_hdr_s *) buf;
    hdr->id = ++self->aes_jtag_cmd_id;
    hdr->cmd = JTAG_CMD_SHIFT;
    hdr->arg1 = must_end ? 1 : 0;
    hdr->arg0 = bits;
    memcpy(buf + sizeof(struct jtag_hdr_s), data, data_bytes);
    uint32_t sz = sizeof(struct jtag_hdr_s) + data_bytes;
    jsdrvp_mb_dev_publish_to_device(self->dev, "c/jtag/!cmd",
        &jsdrv_union_bin(buf, sz));
    self->aes_expected_responses++;
}

static void aes_begin_batch(struct js320_jtag_s * self, uint8_t target_idx, uint8_t target_size) {
    self->aes_expected_responses = 0;
    self->aes_current_response_idx = 0;
    self->aes_target_response_idx = target_idx;
    self->aes_response_size = target_size;
    memset(self->aes_response_buf, 0, sizeof(self->aes_response_buf));
}

// --- Convenience: ECP5 JTAG command sequences ---

// ecp_jtag_cmd: IR instruction + 32 idle cycles in RTI
static void ecp_jtag_cmd(struct js320_jtag_s * self, uint8_t cmd) {
    uint8_t zeros4[4] = {0};
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &cmd, 8, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros4, 32, false);
}

// ecp_jtag_cmd8: IR instruction + 8-bit DR + 32 idle cycles in RTI
static void ecp_jtag_cmd8(struct js320_jtag_s * self, uint8_t cmd, uint8_t operand) {
    uint8_t zeros4[4] = {0};
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &cmd, 8, true);
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, &operand, 8, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros4, 32, false);
}

// --- AES state on_enter functions ---

static bool on_read_idcode(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    aes_begin_batch(self, 1, 4);
    jtag_goto(self, TAP_TEST_LOGIC_RESET);
    uint8_t read_id = ECP5_READ_ID;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &read_id, 8, true);
    uint8_t zeros4[4] = {0};
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, zeros4, 32, true);
    return true;
}

static bool on_isc_enable(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    aes_begin_batch(self, 0xFF, 0);
    ecp_jtag_cmd8(self, ECP5_ISC_ENABLE, 0x02);
    jtag_wait_us(self, 10000);
    return true;
}

static bool on_read_status(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t zeros1[1] = {0};
    uint8_t zeros4[4] = {0};
    aes_begin_batch(self, 2, 4);
    uint8_t ir = ECP5_LSC_READ_STATUS;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &ir, 8, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros1, 2, false);
    jtag_wait_us(self, 1000);
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, zeros4, 32, true);
    return true;
}

static bool on_init_address(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    aes_begin_batch(self, 0xFF, 0);
    ecp_jtag_cmd8(self, ECP5_LSC_INIT_ADDRESS, 0x00);
    jtag_wait_us(self, 10000);
    return true;
}

static bool on_read_cipher(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t zeros1[1] = {0};
    uint8_t zeros16[16] = {0};
    aes_begin_batch(self, 2, 16);
    uint8_t ir = ECP5_LSC_READ_CIPHER_KEY;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &ir, 8, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros1, 5, false);
    jtag_wait_us(self, 1000);
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, zeros16, 128, true);
    return true;
}

static bool on_prog_cipher(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: programming cipher key");
    aes_begin_batch(self, 0xFF, 0);
    uint8_t ir = ECP5_LSC_PROG_CIPHER_KEY;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &ir, 8, true);
    uint8_t key_tdi[16];
    for (int i = 0; i < 16; i++) {
        key_tdi[i] = self->aes_key[15 - i];
    }
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, key_tdi, 128, true);
    uint8_t idle[13] = {0};  // 100 idle cycles
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, idle, 100, false);
    return true;
}

static bool on_key_burn_wait(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: key burn wait (5s)");
    jsdrvp_mb_dev_set_timeout(self->dev, jsdrv_time_utc() + 5 * JSDRV_TIME_SECOND);
    return true;
}

static bool on_prog_feabits(struct js320_jtag_s * self, uint8_t feabits_value) {
    uint8_t zeros1[1] = {0};
    aes_begin_batch(self, 0xFF, 0);
    uint8_t ir = ECP5_LSC_PROG_FEABITS;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &ir, 8, true);
    uint8_t feabits[2] = {feabits_value, 0x00};
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, feabits, 16, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros1, 2, false);
    return true;
}

static bool on_prog_eo(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: programming encrypt-only");
    return on_prog_feabits(self, 0x02);
}

static bool on_eo_burn_wait(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: encrypt-only burn wait (2s)");
    jsdrvp_mb_dev_set_timeout(self->dev, jsdrv_time_utc() + 2 * JSDRV_TIME_SECOND);
    return true;
}

static bool on_read_feabits(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t zeros1[1] = {0};
    uint8_t zeros2[2] = {0};
    aes_begin_batch(self, 2, 2);
    uint8_t ir = ECP5_LSC_READ_FEABITS;
    jtag_goto(self, TAP_SHIFT_IR);
    jtag_shift(self, &ir, 8, true);
    jtag_goto(self, TAP_RUN_TEST_IDLE);
    jtag_shift(self, zeros1, 2, false);
    jtag_wait_us(self, 1000);
    jtag_goto(self, TAP_SHIFT_DR);
    jtag_shift(self, zeros2, 16, true);
    return true;
}

static bool on_prog_kl(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: programming key-lock");
    return on_prog_feabits(self, 0x01);
}

static bool on_kl_burn_wait(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    JSDRV_LOGI("AES: key-lock burn wait (2s)");
    jsdrvp_mb_dev_set_timeout(self->dev, jsdrv_time_utc() + 2 * JSDRV_TIME_SECOND);
    return true;
}

static bool on_isc_disable(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    aes_begin_batch(self, 0xFF, 0);
    ecp_jtag_cmd(self, ECP5_ISC_DISABLE);
    jtag_wait_us(self, 50000);
    jtag_wait_us(self, 50000);
    jtag_wait_us(self, 50000);
    jtag_wait_us(self, 50000);
    ecp_jtag_cmd(self, 0xFF);  // BYPASS
    jtag_wait_us(self, 1000);
    return true;
}

static void aes_send_rsp(struct js320_jtag_s * self, int32_t status) {
    struct jtag_aes_rsp_s rsp;
    rsp.transaction_id = self->aes_transaction_id;
    rsp.status = status;
    jsdrvp_mb_dev_send_to_frontend(self->dev, "h/jtag/aes/!rsp",
        &jsdrv_union_bin((uint8_t *) &rsp, sizeof(rsp)));
}

static bool on_done(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    if (self->aes_error_code == 0) {
        JSDRV_LOGI("AES: programming complete");
    } else {
        JSDRV_LOGW("AES: programming failed with error %d", self->aes_error_code);
    }
    aes_send_rsp(self, self->aes_error_code);
    self->aes_state = AES_IDLE;
    return true;
}

// --- AES guard functions ---

static bool guard_idcode_ok(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint32_t idcode = ((uint32_t) self->aes_response_buf[0])
                    | ((uint32_t) self->aes_response_buf[1] << 8)
                    | ((uint32_t) self->aes_response_buf[2] << 16)
                    | ((uint32_t) self->aes_response_buf[3] << 24);
    if (idcode == ECP5_LFE5U25_IDCODE) {
        return true;
    }
    JSDRV_LOGW("AES: IDCODE mismatch 0x%08x", (unsigned) idcode);
    self->aes_error_code = JSDRV_ERROR_IO;
    return false;
}

static bool guard_status_ok(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint32_t status = ((uint32_t) self->aes_response_buf[0])
                    | ((uint32_t) self->aes_response_buf[1] << 8)
                    | ((uint32_t) self->aes_response_buf[2] << 16)
                    | ((uint32_t) self->aes_response_buf[3] << 24);
    if (status & 0x00024040) {
        JSDRV_LOGW("AES: OTP bits already set, status=0x%08x", (unsigned) status);
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    if ((status & 0x00003C00) != 0x00000C00) {
        JSDRV_LOGW("AES: unexpected status=0x%08x", (unsigned) status);
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    return true;
}

static bool guard_cipher_blank(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    if (0 != memcmp(self->aes_response_buf, ECP5_BLANK_CIPHER_KEY, 16)) {
        JSDRV_LOGW("AES: cipher key fuses already programmed");
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    return true;
}

static bool guard_encrypt_only(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    return (self->aes_flags & ECP5_AES_FLAG_ENCRYPT_ONLY) != 0;
}

static bool guard_key_lock(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    return (self->aes_flags & ECP5_AES_FLAG_KEY_LOCK) != 0;
}

static bool guard_eo_ok_and_kl(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t feabits = self->aes_response_buf[0];
    if ((feabits & 0x02) != 0x02) {
        JSDRV_LOGW("AES: encrypt-only verify failed, feabits=0x%02x", feabits);
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    return (self->aes_flags & ECP5_AES_FLAG_KEY_LOCK) != 0;
}

static bool guard_eo_ok(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t feabits = self->aes_response_buf[0];
    if ((feabits & 0x02) != 0x02) {
        JSDRV_LOGW("AES: encrypt-only verify failed, feabits=0x%02x", feabits);
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    return true;
}

static bool guard_kl_ok(struct js320_jtag_s * self, uint8_t event) {
    (void) event;
    uint8_t feabits = self->aes_response_buf[0];
    if ((feabits & 0x01) != 0x01) {
        JSDRV_LOGW("AES: key-lock verify failed, feabits=0x%02x", feabits);
        self->aes_error_code = JSDRV_ERROR_IO;
        return false;
    }
    return true;
}

// --- Public API ---

struct js320_jtag_s * js320_jtag_alloc(void) {
    return jsdrv_alloc_clr(sizeof(struct js320_jtag_s));
}

void js320_jtag_free(struct js320_jtag_s * jtag) {
    jsdrv_free(jtag);
}

void js320_jtag_on_open(struct js320_jtag_s * jtag, struct jsdrvp_mb_dev_s * dev) {
    jtag->dev = dev;
}

void js320_jtag_on_close(struct js320_jtag_s * jtag) {
    if (jtag->aes_state != AES_IDLE) {
        JSDRV_LOGW("AES: device closed during operation, aborting");
        jsdrvp_mb_dev_set_timeout(jtag->dev, 0);
        jtag->aes_state = AES_IDLE;
    }
    jtag->dev = NULL;
}

bool js320_jtag_handle_cmd(struct js320_jtag_s * jtag,
                            const char * subtopic,
                            const struct jsdrv_union_s * value) {
    if (0 != strcmp(subtopic, "h/jtag/aes/!cmd")) {
        return false;
    }

    if (jtag->aes_state != AES_IDLE) {
        JSDRV_LOGW("AES: command received while busy");
        if (value->type == JSDRV_UNION_BIN && value->size >= sizeof(uint32_t)) {
            jtag->aes_transaction_id = *((const uint32_t *) value->value.bin);
        }
        aes_send_rsp(jtag, JSDRV_ERROR_BUSY);
        return true;
    }

    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct jtag_aes_cmd_s)) {
        JSDRV_LOGW("AES: invalid command format");
        aes_send_rsp(jtag, JSDRV_ERROR_PARAMETER_INVALID);
        return true;
    }

    const struct jtag_aes_cmd_s * cmd = (const struct jtag_aes_cmd_s *) value->value.bin;
    jtag->aes_transaction_id = cmd->transaction_id;
    jtag->aes_flags = cmd->flags;
    memcpy(jtag->aes_key, cmd->key, 16);
    jtag->aes_error_code = 0;
    jtag->aes_jtag_cmd_id = 0;

    JSDRV_LOGI("AES: command received, txn=%u flags=0x%x",
               (unsigned) cmd->transaction_id, (unsigned) cmd->flags);
    aes_process(jtag, AES_EV_CMD);
    return true;
}

bool js320_jtag_handle_publish(struct js320_jtag_s * jtag,
                                const char * subtopic,
                                const struct jsdrv_union_s * value) {
    if (jtag->aes_state == AES_IDLE) {
        return false;
    }
    if (0 != strcmp(subtopic, "c/jtag/!done")) {
        return false;
    }
    if (value->type != JSDRV_UNION_BIN || value->size < sizeof(struct jtag_hdr_s)) {
        return true;  // malformed, consume
    }

    if (jtag->aes_current_response_idx == jtag->aes_target_response_idx) {
        const uint8_t * data = value->value.bin + sizeof(struct jtag_hdr_s);
        uint32_t data_size = value->size - sizeof(struct jtag_hdr_s);
        uint32_t copy_size = (data_size < jtag->aes_response_size)
                           ? data_size : jtag->aes_response_size;
        memcpy(jtag->aes_response_buf, data, copy_size);
    }

    jtag->aes_current_response_idx++;
    jtag->aes_expected_responses--;

    if (jtag->aes_expected_responses == 0) {
        aes_process(jtag, AES_EV_JTAG_RSP);
    }

    return true;  // consume all JTAG responses during AES operation
}

void js320_jtag_on_timeout(struct js320_jtag_s * jtag) {
    aes_process(jtag, AES_EV_TIMER);
}
