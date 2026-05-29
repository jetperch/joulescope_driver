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

#include "jsdrv_prv/devices/js320/js320_cal.h"


static void test_cmd_struct_layout(void ** state) {
    (void) state;
    assert_int_equal(12, sizeof(struct jsdrv_cal_cmd_s));
    struct jsdrv_cal_cmd_s cmd;
    assert_int_equal(0,  (int)((char *)&cmd.transaction_id   - (char *)&cmd));
    assert_int_equal(4,  (int)((char *)&cmd.op                - (char *)&cmd));
    assert_int_equal(5,  (int)((char *)&cmd.src_slot          - (char *)&cmd));
    assert_int_equal(6,  (int)((char *)&cmd.dst_slot          - (char *)&cmd));
    assert_int_equal(7,  (int)((char *)&cmd.flags             - (char *)&cmd));
    assert_int_equal(8,  (int)((char *)&cmd.samples_per_point - (char *)&cmd));
}

static void test_rsp_struct_layout(void ** state) {
    (void) state;
    assert_int_equal(8, sizeof(struct jsdrv_cal_rsp_s));
    struct jsdrv_cal_rsp_s rsp;
    assert_int_equal(0, (int)((char *)&rsp.transaction_id - (char *)&rsp));
    assert_int_equal(4, (int)((char *)&rsp.status         - (char *)&rsp));
}

static void test_op_enum(void ** state) {
    (void) state;
    assert_int_equal(0, JSDRV_CAL_OP_SLOT_READ);
    assert_int_equal(1, JSDRV_CAL_OP_SLOT_COPY);
    assert_int_equal(2, JSDRV_CAL_OP_CURRENT_OFFSET);
    assert_int_equal(3, JSDRV_CAL_OP_VOLTAGE_OFFSET);
}

static void test_slot_enum(void ** state) {
    (void) state;
    // Slot ordering mirrors fpga_mcu FLASH_BLOCK_CAL_*.
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
    // Matches struct js320_calibration_s.
    assert_int_equal(1024, JSDRV_CAL_RECORD_SIZE);
}

static void test_signature_magic(void ** state) {
    (void) state;
    assert_string_equal("JSDRV_OFFSET_CAL", JSDRV_CAL_SIGNATURE_MAGIC);
    assert_int_equal(16, strlen(JSDRV_CAL_SIGNATURE_MAGIC));
}

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
    int count = 0;
    for (int s = 0; s < 6; ++s) {
        for (int m = s; m < 6; ++m) {
            assert_int_equal(count, expected[s][m]);
            ++count;
        }
    }
    assert_int_equal(21, count);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cmd_struct_layout),
        cmocka_unit_test(test_rsp_struct_layout),
        cmocka_unit_test(test_op_enum),
        cmocka_unit_test(test_slot_enum),
        cmocka_unit_test(test_record_size),
        cmocka_unit_test(test_signature_magic),
        cmocka_unit_test(test_index_map_layout),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
