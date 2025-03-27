/*
 * Copyright 2023-2025 Jetperch LLC
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
#include <stdlib.h>
#include "jsdrv/error_code.h"
#include "jsdrv/tmap.h"

#define SECOND  JSDRV_TIME_SECOND
#define MINUTE  JSDRV_TIME_MINUTE
#define HOUR    JSDRV_TIME_HOUR
#define YEAR    JSDRV_TIME_YEAR


static void test_empty(void **state) {
    (void) state;
    int64_t v;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    assert_int_equal(0, jsdrv_tmap_size(s));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &v));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &v));
    jsdrv_tmap_ref_decr(s);
}

static void test_single(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };

    int64_t v;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry);
    assert_int_equal(1, jsdrv_tmap_size(s));
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &v)); assert_int_equal(YEAR, v);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2000, &v)); assert_int_equal(YEAR + SECOND, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &v)); assert_int_equal(1000, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &v)); assert_int_equal(2000, v);
    jsdrv_tmap_ref_decr(s);
}

static void test_add_duplicate(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };

    int64_t v;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry);
    jsdrv_tmap_add(s, &entry);
    assert_int_equal(1, jsdrv_tmap_size(s));
    jsdrv_tmap_ref_decr(s);
}

static void test_multiple(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry1 = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };
    struct jsdrv_time_map_s entry2 = {
        .offset_time = YEAR + SECOND,
        .offset_counter = 2000,
        .counter_rate = 1010.0,
    };
    struct jsdrv_time_map_s entry3 = {
        .offset_time = YEAR + 2 * SECOND,
        .offset_counter = 3010,
        .counter_rate = 1020.0,
    };

    int64_t v;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry1);
    jsdrv_tmap_add(s, &entry2);
    jsdrv_tmap_add(s, &entry3);

    // before and at entry1
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 0, &v)); assert_int_equal(YEAR - SECOND, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR - SECOND, &v)); assert_int_equal(0, v);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 500, &v)); assert_int_equal(YEAR - SECOND / 2, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR - SECOND / 2, &v)); assert_int_equal(500, v);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &v)); assert_int_equal(YEAR, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &v)); assert_int_equal(1000, v);

    // at and after entry3
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 3010, &v)); assert_int_equal(YEAR + 2 * SECOND, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 2 * SECOND, &v)); assert_int_equal(3010, v);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 3520, &v)); assert_int_equal(YEAR + 5 * SECOND / 2, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 5 * SECOND / 2, &v)); assert_int_equal(3520, v);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 4030, &v)); assert_int_equal(YEAR + 3 *  SECOND, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 3 * SECOND, &v)); assert_int_equal(4030, v);

    // between entry1 and entry2
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1500, &v)); assert_int_equal(YEAR + SECOND / 2, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND / 2, &v)); assert_int_equal(1500, v);

    // at entry2
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2000, &v)); assert_int_equal(YEAR + SECOND, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &v)); assert_int_equal(2000, v);

    // between entry2 and entry3
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2505, &v)); assert_int_equal(YEAR + 3 * SECOND / 2, v);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 3 * SECOND / 2, &v)); assert_int_equal(2505, v);

    jsdrv_tmap_ref_decr(s);
}

static void test_expire(void **state) {
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };
    for (size_t idx = 0; idx < 5; ++idx) {
        jsdrv_tmap_add(s, &entry);
        entry.offset_time += SECOND;
        entry.offset_counter += entry.counter_rate;
        entry.counter_rate += 2.0;
    }
    assert_int_equal(5, jsdrv_tmap_size(s));
    jsdrv_tmap_expire_by_sample_id(s, 0);
    assert_int_equal(5, jsdrv_tmap_size(s));

    jsdrv_tmap_expire_by_sample_id(s, 1999);
    assert_int_equal(5, jsdrv_tmap_size(s));

    jsdrv_tmap_expire_by_sample_id(s, 2001);
    assert_int_equal(4, jsdrv_tmap_size(s));

    jsdrv_tmap_expire_by_sample_id(s, 4100);
    assert_int_equal(2, jsdrv_tmap_size(s));

    jsdrv_tmap_ref_decr(s);
}

static struct jsdrv_time_map_s * construct(size_t count) {
    struct jsdrv_time_map_s * e = calloc(count, sizeof(struct jsdrv_time_map_s));
    assert_non_null(e);
    e[0].offset_time = YEAR;
    e[0].offset_counter = 1000;
    e[0].counter_rate = 1000.0;
    for (size_t idx = 1; idx < 20; ++idx) {
        e[idx].offset_time = e[idx - 1].offset_time + SECOND;
        e[idx].offset_counter = e[idx - 1].offset_counter + (uint64_t) e[idx - 1].counter_rate;
        e[idx].counter_rate = e[idx - 1].counter_rate + 2.0;
    }
    return e;
}

static void test_wrap(void **state) {
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 1; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
        if (entry[idx].offset_counter > 3500) {
            jsdrv_tmap_expire_by_sample_id(s, entry[idx].offset_counter - 2500);
        }
    }
    assert_int_equal(4, jsdrv_tmap_size(s));

    int64_t v;
    for (size_t idx = 16; idx < 20; ++idx) {
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[idx].offset_counter, &v)); assert_int_equal(entry[idx].offset_time, v);
        assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, entry[idx].offset_time, &v)); assert_int_equal(entry[idx].offset_counter, v);
    }

    jsdrv_tmap_ref_decr(s);
    free(entry);
}

static void test_grow(void **state) {
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(4);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }
    assert_int_equal(20, jsdrv_tmap_size(s));

    int64_t v;
    for (size_t idx = 0; idx < 20; ++idx) {
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[idx].offset_counter, &v)); assert_int_equal(entry[idx].offset_time, v);
        assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, entry[idx].offset_time, &v)); assert_int_equal(entry[idx].offset_counter, v);
    }

    // expiring "breaks" past history
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[0].offset_counter, &v)); assert_int_equal(entry[0].offset_time, v);
    jsdrv_tmap_expire_by_sample_id(s, 10000);
    assert_int_equal(12, jsdrv_tmap_size(s));
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[0].offset_counter, &v)); assert_int_not_equal(entry[0].offset_time, v);

    jsdrv_tmap_ref_decr(s);
    free(entry);
}

static void test_clear(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    jsdrv_tmap_add(s, &entry);
    assert_int_equal(1, jsdrv_tmap_size(s));
    jsdrv_tmap_clear(s);
    assert_int_equal(0, jsdrv_tmap_size(s));
    jsdrv_tmap_ref_decr(s);
}

static void test_get(void **state) {
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }

    jsdrv_tmap_expire_by_sample_id(s, 10000);
    assert_int_equal(12, jsdrv_tmap_size(s));
    for (size_t idx = 0; idx < 12; ++idx) {
        struct jsdrv_time_map_s * e = jsdrv_tmap_get(s, idx);
        assert_int_equal(entry[idx + 8].offset_time, e->offset_time);
        assert_int_equal(entry[idx + 8].offset_counter, e->offset_counter);
        assert_int_equal((int) entry[idx + 8].counter_rate, (int) e->counter_rate);
    }
    jsdrv_tmap_ref_decr(s);
}

static void test_concurrency(void **state) {
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);

    for (size_t idx = 0; idx < 20; ++idx) {
        assert_int_equal(idx, jsdrv_tmap_size(s));
        jsdrv_tmap_reader_enter(s);
        jsdrv_tmap_add(s, &entry[idx]);
        assert_int_equal(idx, jsdrv_tmap_size(s));
        jsdrv_tmap_reader_exit(s);
        assert_int_equal(idx + 1, jsdrv_tmap_size(s));
    }

    jsdrv_tmap_reader_enter(s);
    jsdrv_tmap_reader_enter(s);
    jsdrv_tmap_expire_by_sample_id(s, 4100);
    assert_int_equal(20, jsdrv_tmap_size(s));
    jsdrv_tmap_reader_exit(s);
    assert_int_equal(20, jsdrv_tmap_size(s));
    jsdrv_tmap_reader_exit(s);
    assert_int_equal(17, jsdrv_tmap_size(s));
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_single),
            cmocka_unit_test(test_add_duplicate),
            cmocka_unit_test(test_multiple),
            cmocka_unit_test(test_expire),
            cmocka_unit_test(test_wrap),
            cmocka_unit_test(test_grow),
            cmocka_unit_test(test_clear),
            cmocka_unit_test(test_get),
            cmocka_unit_test(test_concurrency),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
