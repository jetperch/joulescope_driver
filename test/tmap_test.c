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
    int64_t v = 0;
    uint64_t k = 0;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    assert_int_equal(0, jsdrv_tmap_length(s));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &v));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &k));
    jsdrv_tmap_free(s);
}

static void test_single(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };

    int64_t t = 0;
    uint64_t k = 0;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry);
    assert_int_equal(1, jsdrv_tmap_length(s));
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &t)); assert_int_equal(YEAR, t);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2000, &t)); assert_int_equal(YEAR + SECOND, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &k)); assert_int_equal(1000, k);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &k)); assert_int_equal(2000, k);
    jsdrv_tmap_free(s);
}

static void test_add_duplicate(void **state) {
    (void) state;
    struct jsdrv_time_map_s entry = {
        .offset_time = YEAR,
        .offset_counter = 1000,
        .counter_rate = 1000.0,
    };

    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry);
    jsdrv_tmap_add(s, &entry);
    assert_int_equal(1, jsdrv_tmap_length(s));
    jsdrv_tmap_free(s);
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

    int64_t t = 0;
    uint64_t k = 0;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, &entry1);
    jsdrv_tmap_add(s, &entry2);
    jsdrv_tmap_add(s, &entry3);

    // before and at entry1
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 0, &t)); assert_int_equal(YEAR - SECOND, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR - SECOND, &k)); assert_int_equal(0, k);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 500, &t)); assert_int_equal(YEAR - SECOND / 2, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR - SECOND / 2, &k)); assert_int_equal(500, k);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1000, &t)); assert_int_equal(YEAR, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR, &k)); assert_int_equal(1000, k);

    // at and after entry3
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 3010, &t)); assert_int_equal(YEAR + 2 * SECOND, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 2 * SECOND, &k)); assert_int_equal(3010, k);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 3520, &t)); assert_int_equal(YEAR + 5 * SECOND / 2, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 5 * SECOND / 2, &k)); assert_int_equal(3520, k);
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 4030, &t)); assert_int_equal(YEAR + 3 *  SECOND, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 3 * SECOND, &k)); assert_int_equal(4030, k);

    // between entry1 and entry2
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 1500, &t)); assert_int_equal(YEAR + SECOND / 2, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND / 2, &k)); assert_int_equal(1500, k);

    // at entry2
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2000, &t)); assert_int_equal(YEAR + SECOND, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &k)); assert_int_equal(2000, k);

    // between entry2 and entry3
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, 2505, &t)); assert_int_equal(YEAR + 3 * SECOND / 2, t);
    assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, YEAR + 3 * SECOND / 2, &k)); assert_int_equal(2505, k);

    jsdrv_tmap_free(s);
}

static void test_expire(void **state) {
    (void) state;
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
    assert_int_equal(5, jsdrv_tmap_length(s));
    jsdrv_tmap_expire_by_sample_id(s, 0);
    assert_int_equal(5, jsdrv_tmap_length(s));

    jsdrv_tmap_expire_by_sample_id(s, 1999);
    assert_int_equal(5, jsdrv_tmap_length(s));

    jsdrv_tmap_expire_by_sample_id(s, 2001);
    assert_int_equal(4, jsdrv_tmap_length(s));

    jsdrv_tmap_expire_by_sample_id(s, 4100);
    assert_int_equal(2, jsdrv_tmap_length(s));

    jsdrv_tmap_free(s);
}

static struct jsdrv_time_map_s * construct(size_t count) {
    struct jsdrv_time_map_s * e = calloc(count, sizeof(struct jsdrv_time_map_s));
    assert_non_null(e);
    e[0].offset_time = YEAR;
    e[0].offset_counter = 1000;
    e[0].counter_rate = 1000.0;
    for (size_t idx = 1; idx < count; ++idx) {
        e[idx].offset_time = e[idx - 1].offset_time + SECOND;
        e[idx].offset_counter = e[idx - 1].offset_counter + (uint64_t) e[idx - 1].counter_rate;
        e[idx].counter_rate = e[idx - 1].counter_rate + 2.0;
    }
    return e;
}

static void test_wrap(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 1; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
        if (entry[idx].offset_counter > 3500) {
            jsdrv_tmap_expire_by_sample_id(s, entry[idx].offset_counter - 2500);
        }
    }
    assert_int_equal(4, jsdrv_tmap_length(s));

    int64_t t = 0;
    uint64_t k = 0;
    for (size_t idx = 16; idx < 20; ++idx) {
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[idx].offset_counter, &t)); assert_int_equal(entry[idx].offset_time, t);
        assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, entry[idx].offset_time, &k)); assert_int_equal(entry[idx].offset_counter, k);
    }

    jsdrv_tmap_free(s);
    free(entry);
}

static void test_grow(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(4);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }
    assert_int_equal(20, jsdrv_tmap_length(s));

    int64_t t = 0;
    uint64_t k = 0;
    for (size_t idx = 0; idx < 20; ++idx) {
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[idx].offset_counter, &t)); assert_int_equal(entry[idx].offset_time, t);
        assert_int_equal(0, jsdrv_tmap_timestamp_to_sample_id(s, entry[idx].offset_time, &k)); assert_int_equal(entry[idx].offset_counter, k);
    }

    // expiring "breaks" past history
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[0].offset_counter, &t)); assert_int_equal(entry[0].offset_time, t);
    jsdrv_tmap_expire_by_sample_id(s, 10000);
    assert_int_equal(12, jsdrv_tmap_length(s));
    assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[0].offset_counter, &t)); assert_int_not_equal(entry[0].offset_time, t);

    jsdrv_tmap_free(s);
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
    assert_int_equal(1, jsdrv_tmap_length(s));
    jsdrv_tmap_clear(s);
    assert_int_equal(0, jsdrv_tmap_length(s));
    jsdrv_tmap_free(s);
}

static void test_get(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }

    jsdrv_tmap_expire_by_sample_id(s, 10000);
    assert_int_equal(12, jsdrv_tmap_length(s));
    for (size_t idx = 0; idx < 12; ++idx) {
        struct jsdrv_time_map_s e;
        assert_int_equal(0, jsdrv_tmap_get(s, idx, &e));
        assert_int_equal(entry[idx + 8].offset_time, e.offset_time);
        assert_int_equal(entry[idx + 8].offset_counter, e.offset_counter);
        assert_int_equal((int) entry[idx + 8].counter_rate, (int) e.counter_rate);
    }
    jsdrv_tmap_free(s);
}

static void test_free_null(void **state) {
    (void) state;
    jsdrv_tmap_free(NULL);  // must not crash
}

static void test_null_safety(void **state) {
    (void) state;
    // Every mutation and query API accepts NULL self without crashing.
    struct jsdrv_time_map_s entry = {.offset_time = YEAR, .offset_counter = 1000, .counter_rate = 1000.0};
    int64_t t = 0;
    uint64_t k = 0;
    struct jsdrv_time_map_s out;

    jsdrv_tmap_clear(NULL);
    jsdrv_tmap_add(NULL, &entry);
    jsdrv_tmap_expire_by_sample_id(NULL, 0);
    assert_int_equal(0, jsdrv_tmap_length(NULL));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_sample_id_to_timestamp(NULL, 1000, &t));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_timestamp_to_sample_id(NULL, YEAR, &k));
    assert_int_equal(JSDRV_ERROR_UNAVAILABLE, jsdrv_tmap_get(NULL, 0, &out));

    // jsdrv_tmap_add also tolerates NULL time_map.
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    jsdrv_tmap_add(s, NULL);
    assert_int_equal(0, jsdrv_tmap_length(s));
    jsdrv_tmap_free(s);
}

static void test_copy_empty(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(0);
    struct jsdrv_tmap_s * c = jsdrv_tmap_copy(s);
    assert_non_null(c);
    assert_int_equal(0, jsdrv_tmap_length(c));
    jsdrv_tmap_free(c);
    jsdrv_tmap_free(s);
    assert_null(jsdrv_tmap_copy(NULL));
}

static void test_copy_independent(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(8);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 5; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }

    struct jsdrv_tmap_s * c = jsdrv_tmap_copy(s);
    assert_int_equal(5, jsdrv_tmap_length(c));

    int64_t t_src = 0;
    int64_t t_copy = 0;
    for (size_t idx = 0; idx < 5; ++idx) {
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(s, entry[idx].offset_counter, &t_src));
        assert_int_equal(0, jsdrv_tmap_sample_id_to_timestamp(c, entry[idx].offset_counter, &t_copy));
        assert_int_equal(t_src, t_copy);
    }

    // mutating src does not change the copy
    for (size_t idx = 5; idx < 10; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
    }
    assert_int_equal(10, jsdrv_tmap_length(s));
    assert_int_equal(5, jsdrv_tmap_length(c));

    // mutating the copy does not change src
    jsdrv_tmap_clear(c);
    assert_int_equal(0, jsdrv_tmap_length(c));
    assert_int_equal(10, jsdrv_tmap_length(s));

    // freeing src does not invalidate the copy's prior data
    jsdrv_tmap_free(s);
    jsdrv_tmap_add(c, &entry[0]);
    assert_int_equal(1, jsdrv_tmap_length(c));

    jsdrv_tmap_free(c);
    free(entry);
}

static void test_copy_wrapped(void **state) {
    (void) state;
    struct jsdrv_tmap_s * s = jsdrv_tmap_alloc(4);
    struct jsdrv_time_map_s * entry = construct(20);
    for (size_t idx = 0; idx < 20; ++idx) {
        jsdrv_tmap_add(s, &entry[idx]);
        if (idx >= 4) {
            jsdrv_tmap_expire_by_sample_id(s, entry[idx].offset_counter - 2500);
        }
    }
    size_t sz = jsdrv_tmap_length(s);
    struct jsdrv_tmap_s * c = jsdrv_tmap_copy(s);
    assert_int_equal(sz, jsdrv_tmap_length(c));

    struct jsdrv_time_map_s a = {0}, b = {0};
    for (size_t idx = 0; idx < sz; ++idx) {
        assert_int_equal(0, jsdrv_tmap_get(s, idx, &a));
        assert_int_equal(0, jsdrv_tmap_get(c, idx, &b));
        assert_int_equal(a.offset_counter, b.offset_counter);
        assert_int_equal(a.offset_time, b.offset_time);
    }
    jsdrv_tmap_free(c);
    jsdrv_tmap_free(s);
    free(entry);
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
            cmocka_unit_test(test_free_null),
            cmocka_unit_test(test_null_safety),
            cmocka_unit_test(test_copy_empty),
            cmocka_unit_test(test_copy_independent),
            cmocka_unit_test(test_copy_wrapped),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
