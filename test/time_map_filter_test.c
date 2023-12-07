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
#include "jsdrv_prv/time_map_filter.h"


#define FREQ (2000000LLU)


static void test_new_free(void **state) {
    (void) state;
    struct jsdrv_time_map_s tm;
    struct jsdrv_tmf_s * t = jsdrv_tmf_new(FREQ, 60, JSDRV_TIME_SECOND);
    assert_non_null(t);
    jsdrv_tmf_get(t, &tm);
    assert_int_equal(0, tm.offset_counter);
    assert_int_equal(0, tm.offset_time);
    assert_int_equal(FREQ, tm.counter_rate);
    jsdrv_tmf_free(t);
}

static void test_add_one(void **state) {
    (void) state;
    struct jsdrv_time_map_s tm;
    struct jsdrv_tmf_s * t = jsdrv_tmf_new(FREQ, 60, JSDRV_TIME_SECOND);
    assert_non_null(t);
    jsdrv_tmf_add(t, 60 * FREQ, JSDRV_TIME_MINUTE);
    jsdrv_tmf_get(t, &tm);
    assert_int_equal(60 * FREQ, tm.offset_counter);
    assert_int_equal(JSDRV_TIME_MINUTE, tm.offset_time);
    assert_int_equal(FREQ, tm.counter_rate);
    jsdrv_tmf_free(t);
}

static void test_add_multiple(void **state) {
    (void) state;
    struct jsdrv_time_map_s tm;
    struct jsdrv_tmf_s * t = jsdrv_tmf_new(FREQ, 60, JSDRV_TIME_SECOND);
    assert_non_null(t);
    jsdrv_tmf_add(t, 60 * FREQ, 60 * JSDRV_TIME_SECOND);
    jsdrv_tmf_add(t, 62 * FREQ, 62 * JSDRV_TIME_SECOND - JSDRV_TIME_MILLISECOND);
    jsdrv_tmf_add(t, 64 * FREQ, 64 * JSDRV_TIME_SECOND + JSDRV_TIME_MILLISECOND);
    jsdrv_tmf_get(t, &tm);
    assert_int_equal(60 * FREQ, tm.offset_counter);
    assert_int_equal(JSDRV_TIME_MINUTE - JSDRV_TIME_MILLISECOND, tm.offset_time);
    assert_int_equal(FREQ, tm.counter_rate);
    jsdrv_tmf_free(t);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_new_free),
            cmocka_unit_test(test_add_one),
            cmocka_unit_test(test_add_multiple),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
