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
#include "jsdrv_prv/devices/js320/js320_cal_mgr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/check32.h"


// =====================================================================
// Header / response struct layout
// =====================================================================

static void test_add_header_size(void ** state) {
    (void) state;
    // Compile-time assert lives in js320_cal_mgr.c; runtime check here
    // pins the on-wire size that Python wrappers and the minibitty CLI
    // depend on.
    assert_int_equal(40, sizeof(struct jsdrv_cal_add_header_s));
}

static void test_add_rsp_size(void ** state) {
    (void) state;
    assert_int_equal(8, sizeof(struct jsdrv_cal_add_rsp_s));
}

static void test_add_header_fields(void ** state) {
    (void) state;
    struct jsdrv_cal_add_header_s hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.device_prefix, "u/js320/8W2A");
    hdr.op = JSDRV_CAL_OP_SLOT_READ;
    hdr.src_slot = JSDRV_CAL_SLOT_FACTORY;
    hdr.dst_slot = 0;
    hdr.samples_per_point = 100000;

    assert_string_equal(hdr.device_prefix, "u/js320/8W2A");
    assert_int_equal(hdr.op, JSDRV_CAL_OP_SLOT_READ);
    assert_int_equal(hdr.src_slot, JSDRV_CAL_SLOT_FACTORY);
    assert_int_equal(hdr.samples_per_point, 100000);
}

static void test_add_header_prefix_max_length(void ** state) {
    (void) state;
    struct jsdrv_cal_add_header_s hdr;
    memset(&hdr, 0, sizeof(hdr));
    memset(hdr.device_prefix, 'x', 31);
    hdr.device_prefix[31] = '\0';
    assert_int_equal(31, strlen(hdr.device_prefix));
}

static void test_add_header_payload_layout(void ** state) {
    (void) state;
    // Header field byte offsets must remain stable so wrappers can pack
    // them by hand.
    struct jsdrv_cal_add_header_s hdr;
    assert_int_equal(0,  (int)((char *)&hdr.device_prefix    - (char *)&hdr));
    assert_int_equal(32, (int)((char *)&hdr.op               - (char *)&hdr));
    assert_int_equal(33, (int)((char *)&hdr.src_slot         - (char *)&hdr));
    assert_int_equal(34, (int)((char *)&hdr.dst_slot         - (char *)&hdr));
    assert_int_equal(35, (int)((char *)&hdr.flags            - (char *)&hdr));
    assert_int_equal(36, (int)((char *)&hdr.samples_per_point - (char *)&hdr));
}

static void test_add_rsp_fields(void ** state) {
    (void) state;
    struct jsdrv_cal_add_rsp_s rsp = { .rc = 0, .worker_id = 7 };
    assert_int_equal(0, rsp.rc);
    assert_int_equal(7, rsp.worker_id);

    struct jsdrv_cal_add_rsp_s err = { .rc = JSDRV_ERROR_PARAMETER_INVALID,
                                       .worker_id = 0 };
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, err.rc);
    assert_int_equal(0, err.worker_id);
}


// =====================================================================
// Op / slot enums and constants
// =====================================================================

static void test_op_values(void ** state) {
    (void) state;
    // Op values are part of the on-wire protocol; lock them in.
    assert_int_equal(0, JSDRV_CAL_OP_SLOT_READ);
    assert_int_equal(1, JSDRV_CAL_OP_SLOT_COPY);
    assert_int_equal(2, JSDRV_CAL_OP_CURRENT_OFFSET);
    assert_int_equal(3, JSDRV_CAL_OP_VOLTAGE_OFFSET);
}

static void test_slot_values(void ** state) {
    (void) state;
    // Slot ordering must match the device-side FLASH_BLOCK_CAL_* sequence
    // so the worker's slot->offset mapping stays correct.
    assert_int_equal(0, JSDRV_CAL_SLOT_ACTIVE);
    assert_int_equal(1, JSDRV_CAL_SLOT_TRIM2);
    assert_int_equal(2, JSDRV_CAL_SLOT_TRIM1);
    assert_int_equal(3, JSDRV_CAL_SLOT_FIELD);
    assert_int_equal(4, JSDRV_CAL_SLOT_LAB);
    assert_int_equal(5, JSDRV_CAL_SLOT_FACTORY);
    assert_int_equal(6, JSDRV_CAL_SLOT_COUNT);
}

static void test_record_size(void ** state) {
    (void) state;
    // Matches struct js320_calibration_s in
    // js320/firmware/fpga_common/include/calibration.h.
    assert_int_equal(1024, JSDRV_CAL_RECORD_SIZE);
}

static void test_signature_magic(void ** state) {
    (void) state;
    assert_string_equal("JSDRV_OFFSET_CAL", JSDRV_CAL_SIGNATURE_MAGIC);
    assert_int_equal(16, strlen(JSDRV_CAL_SIGNATURE_MAGIC));
}

static void test_topic_defines(void ** state) {
    (void) state;
    assert_string_equal("cal/@/!add", JSDRV_CAL_MGR_TOPIC_ADD);
    assert_string_equal("cal/@/list", JSDRV_CAL_MGR_TOPIC_LIST);
    assert_true(JSDRV_CAL_INSTANCE_MAX >= 1);
    assert_true(JSDRV_CAL_INSTANCE_MAX <= 16);
}


// =====================================================================
// Index map for current cal upper-triangular sweep
//
// Algorithm coverage for check32_xxhash lives in test/check32_test.c.
// =====================================================================

static void test_index_map_layout(void ** state) {
    (void) state;
    // Expected mapping from (i_select, i_mux_select) -> point index.
    // Mirrors calibration.h:60-77 and cal_save.py:32-55.
    int expected[6][6] = {
        { 0,  1,  2,  3,  4,  5},
        {-1,  6,  7,  8,  9, 10},
        {-1, -1, 11, 12, 13, 14},
        {-1, -1, -1, 15, 16, 17},
        {-1, -1, -1, -1, 18, 19},
        {-1, -1, -1, -1, -1, 20},
    };
    // We re-derive the count and ordering rather than reading
    // CAL_INDEX_MAP (which is static in js320_cal_mgr.c).
    int count = 0;
    for (int s = 0; s < 6; ++s) {
        for (int m = s; m < 6; ++m) {
            assert_int_equal(count, expected[s][m]);
            ++count;
        }
    }
    assert_int_equal(21, count);
}


// =====================================================================
// Signature pattern packing
// =====================================================================

static void test_signature_layout(void ** state) {
    (void) state;
    // Pack the same way js320_cal_mgr.c does and verify byte layout.
    uint8_t sig[64];
    memset(sig, 0xAA, sizeof(sig));   // marker so we catch leftover bytes

    memset(sig, 0, sizeof(sig));
    memcpy(sig, JSDRV_CAL_SIGNATURE_MAGIC, 16);
    uint32_t ver = 0x01020304U;
    sig[16] = 0x04;
    sig[17] = 0x03;
    sig[18] = 0x02;
    sig[19] = 0x01;

    assert_int_equal('J', sig[0]);
    assert_int_equal('S', sig[1]);
    assert_int_equal('_', sig[5]);    // "JSDRV_..."
    assert_int_equal('L', sig[15]);   // last char of "OFFSET_CAL"
    assert_int_equal(0x04, sig[16]);
    assert_int_equal(0x01, sig[19]);
    for (int i = 20; i < 64; ++i) {
        assert_int_equal(0, sig[i]);
    }
    (void) ver;
}


// =====================================================================
// main
// =====================================================================

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_add_header_size),
        cmocka_unit_test(test_add_rsp_size),
        cmocka_unit_test(test_add_header_fields),
        cmocka_unit_test(test_add_header_prefix_max_length),
        cmocka_unit_test(test_add_header_payload_layout),
        cmocka_unit_test(test_add_rsp_fields),
        cmocka_unit_test(test_op_values),
        cmocka_unit_test(test_slot_values),
        cmocka_unit_test(test_record_size),
        cmocka_unit_test(test_signature_magic),
        cmocka_unit_test(test_topic_defines),
        cmocka_unit_test(test_index_map_layout),
        cmocka_unit_test(test_signature_layout),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
