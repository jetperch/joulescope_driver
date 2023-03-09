/*
 * Copyright 2023 Jetperch LLC
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
#include "jsdrv/time.h"


#define OFFSET1 2200000LLU
#define FS1 1000

static void test_constants(void **state) {
    (void) state;
    assert_int_equal(1 << 30, JSDRV_TIME_SECOND);
    assert_int_equal((JSDRV_TIME_SECOND + 500) / 1000, JSDRV_TIME_MILLISECOND);
    assert_int_equal((JSDRV_TIME_SECOND + 500000) / 1000000, JSDRV_TIME_MICROSECOND);
    assert_int_equal(1, JSDRV_TIME_NANOSECOND);
    assert_int_equal(JSDRV_TIME_SECOND * 60, JSDRV_TIME_MINUTE);
    assert_int_equal(JSDRV_TIME_SECOND * 60 * 60, JSDRV_TIME_HOUR);
    assert_int_equal(JSDRV_TIME_SECOND * 60 * 60 * 24, JSDRV_TIME_DAY);
    assert_int_equal(JSDRV_TIME_SECOND * 60 * 60 * 24 * 7, JSDRV_TIME_WEEK);
    assert_int_equal((JSDRV_TIME_SECOND * 60 * 60 * 24 * 365) / 12, JSDRV_TIME_MONTH);
    assert_int_equal(JSDRV_TIME_SECOND * 60 * 60 * 24 * 365, JSDRV_TIME_YEAR);
}

static void test_f32(void **state) {
    (void) state;
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_F32_TO_TIME(1.0f));
    assert_float_equal(1.0f, JSDRV_TIME_TO_F32(JSDRV_TIME_SECOND), 1e-12);
}

static void test_f64(void **state) {
    (void) state;
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_F64_TO_TIME(1.0));
    assert_float_equal(1.0, JSDRV_TIME_TO_F64(JSDRV_TIME_SECOND), 1e-12);
}

static void test_convert_time_to(void **state) {
    (void) state;
    assert_int_equal(1, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND));
    assert_int_equal(1, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND + 1));
    assert_int_equal(1, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND - 1));
    assert_int_equal(2, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND + JSDRV_TIME_SECOND / 2));
    assert_int_equal(1, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND - JSDRV_TIME_SECOND / 2));
    assert_int_equal(0, JSDRV_TIME_TO_SECONDS(JSDRV_TIME_SECOND - JSDRV_TIME_SECOND / 2 - 1));
    assert_int_equal(1000, JSDRV_TIME_TO_MILLISECONDS(JSDRV_TIME_SECOND));
    assert_int_equal(1000000, JSDRV_TIME_TO_MICROSECONDS(JSDRV_TIME_SECOND));
    assert_int_equal(1000000000, JSDRV_TIME_TO_NANOSECONDS(JSDRV_TIME_SECOND));
}

static void test_convert_to_time(void **state) {
    (void) state;
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_SECONDS_TO_TIME(1));
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_MILLISECONDS_TO_TIME(1000));
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_MICROSECONDS_TO_TIME(1000000));
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_NANOSECONDS_TO_TIME(1000000000));
}

static void test_abs(void **state) {
    (void) state;
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_TIME_ABS(JSDRV_TIME_SECOND));
    assert_int_equal(JSDRV_TIME_SECOND, JSDRV_TIME_ABS(-JSDRV_TIME_SECOND));
}

static void test_round_nearest(void **state) {
    (void) state;
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER(JSDRV_TIME_SECOND, 1));
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER(JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER(JSDRV_TIME_SECOND - 1, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER(-JSDRV_TIME_SECOND, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER(-JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER(-JSDRV_TIME_SECOND - 1, 1));
}

static void test_round_zero(void **state) {
    (void) state;
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER_RZERO(JSDRV_TIME_SECOND, 1));
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER_RZERO(JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(0, JSDRV_TIME_TO_COUNTER_RZERO(JSDRV_TIME_SECOND - 1, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER_RZERO(-JSDRV_TIME_SECOND, 1));
    assert_int_equal(0, JSDRV_TIME_TO_COUNTER_RZERO(-JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER_RZERO(-JSDRV_TIME_SECOND - 1, 1));
}

static void test_round_inf(void **state) {
    (void) state;
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER_RINF(JSDRV_TIME_SECOND, 1));
    assert_int_equal(2, JSDRV_TIME_TO_COUNTER_RINF(JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(1, JSDRV_TIME_TO_COUNTER_RINF(JSDRV_TIME_SECOND - 1, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER_RINF(-JSDRV_TIME_SECOND, 1));
    assert_int_equal(-1, JSDRV_TIME_TO_COUNTER_RINF(-JSDRV_TIME_SECOND + 1, 1));
    assert_int_equal(-2, JSDRV_TIME_TO_COUNTER_RINF(-JSDRV_TIME_SECOND - 1, 1));
}

static void test_min(void **state) {
    (void) state;
    assert_int_equal(1, jsdrv_time_min(1, 2));
    assert_int_equal(1, jsdrv_time_min(2, 1));
    assert_int_equal(-2, jsdrv_time_min(-2, 3));
    assert_int_equal(-2, jsdrv_time_min(3, -2));
    assert_int_equal(-2, jsdrv_time_min(-1, -2));
    assert_int_equal(-2, jsdrv_time_min(-2, -1));
}

static void test_max(void **state) {
    (void) state;
    assert_int_equal(2, jsdrv_time_max(1, 2));
    assert_int_equal(2, jsdrv_time_max(2, 1));
    assert_int_equal(3, jsdrv_time_max(-2, 3));
    assert_int_equal(3, jsdrv_time_max(3, -2));
    assert_int_equal(-1, jsdrv_time_max(-1, -2));
    assert_int_equal(-1, jsdrv_time_max(-2, -1));
}

static void test_str(void **state) {
    (void) state;
    char s[30];
    assert_int_equal(26, jsdrv_time_to_str(0, s, sizeof(s)));
    assert_string_equal("2018-01-01T00:00:00.000000", s);
    assert_int_equal(19, jsdrv_time_to_str(0, s, 20));
    assert_string_equal("2018-01-01T00:00:00", s);

    jsdrv_time_to_str(JSDRV_TIME_SECOND, s, sizeof(s));
    assert_string_equal("2018-01-01T00:00:01.000000", s);
    jsdrv_time_to_str(JSDRV_TIME_SECOND * 60 * 60 * 24, s, sizeof(s));
    assert_string_equal("2018-01-02T00:00:00.000000", s);

    jsdrv_time_to_str(117133546395387584LL, s, sizeof(s));
    assert_string_equal("2021-06-16T14:31:56.002794", s);
}


static void test_counter_trivial(void **state) {
    (void) state;
    struct jsdrv_time_map_s tmap = {
        .offset_time = 0,
        .offset_counter = 0,
        .counter_rate = 1
    };

    // at offset
    assert_int_equal(0, jsdrv_time_from_counter(&tmap, 0));
    assert_int_equal(0, jsdrv_time_to_counter(&tmap, 0));

    // after offset
    assert_int_equal(JSDRV_TIME_SECOND, jsdrv_time_from_counter(&tmap, 1));
    assert_int_equal(1, jsdrv_time_to_counter(&tmap, JSDRV_TIME_SECOND));
}

static void test_counter(void **state) {
    (void) state;
    struct jsdrv_time_map_s tmap = {
            .offset_time = JSDRV_TIME_HOUR,
            .offset_counter = OFFSET1,
            .counter_rate = FS1,
    };

    // at offset
    assert_int_equal(JSDRV_TIME_HOUR, jsdrv_time_from_counter(&tmap, OFFSET1));
    assert_int_equal(OFFSET1, jsdrv_time_to_counter(&tmap, JSDRV_TIME_HOUR));

    // after offset
    assert_int_equal(JSDRV_TIME_HOUR + JSDRV_TIME_SECOND, jsdrv_time_from_counter(&tmap, OFFSET1 + FS1));
    assert_int_equal(OFFSET1 + FS1, jsdrv_time_to_counter(&tmap, JSDRV_TIME_HOUR + JSDRV_TIME_SECOND));

    // before offset
    assert_int_equal(JSDRV_TIME_HOUR - JSDRV_TIME_SECOND, jsdrv_time_from_counter(&tmap, OFFSET1 - FS1));
    assert_int_equal(OFFSET1 - FS1, jsdrv_time_to_counter(&tmap, JSDRV_TIME_HOUR - JSDRV_TIME_SECOND));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_constants),
            cmocka_unit_test(test_f32),
            cmocka_unit_test(test_f64),
            cmocka_unit_test(test_convert_time_to),
            cmocka_unit_test(test_convert_to_time),
            cmocka_unit_test(test_abs),
            cmocka_unit_test(test_round_nearest),
            cmocka_unit_test(test_round_zero),
            cmocka_unit_test(test_round_inf),
            cmocka_unit_test(test_min),
            cmocka_unit_test(test_max),
            cmocka_unit_test(test_str),
            cmocka_unit_test(test_counter_trivial),
            cmocka_unit_test(test_counter),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
