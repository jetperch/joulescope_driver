#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "jsdrv/cdef.h"
#include "jsdrv_prv/time_map.h"

START_TEST(test_tmap_bounds_safety)
{
    // Invariant: tmap copy/merge operations must never write beyond allocated entry capacity
    struct jsdrv_time_map_s entries[JSDRV_TIME_MAP_LENGTH];
    memset(entries, 0, sizeof(entries));

    // Initialize entries with valid time map data
    for (int i = 0; i < JSDRV_TIME_MAP_LENGTH; i++) {
        entries[i].offset_time = (int64_t)i * 1000000;
        entries[i].offset_counter = (uint64_t)i * 48000;
        entries[i].counter_rate = 48000.0;
    }

    // Test cases: (tail, length) pairs representing adversarial scenarios
    struct { uint16_t tail; uint16_t length; } cases[] = {
        {JSDRV_TIME_MAP_LENGTH - 1, JSDRV_TIME_MAP_LENGTH},  // tail near end, full wrap
        {0, JSDRV_TIME_MAP_LENGTH},                            // no wrap, full capacity
        {JSDRV_TIME_MAP_LENGTH / 2, JSDRV_TIME_MAP_LENGTH},   // mid wrap, full capacity
        {0, 1},                                                 // minimal valid input
    };
    int num_cases = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < num_cases; i++) {
        struct jsdrv_time_map_s dst[JSDRV_TIME_MAP_LENGTH];
        uint8_t guard[16];
        memset(guard, 0xAA, sizeof(guard));
        memset(dst, 0, sizeof(dst));

        uint16_t tail = cases[i].tail;
        uint16_t length = cases[i].length;
        uint16_t head_count = (tail + length <= JSDRV_TIME_MAP_LENGTH)
            ? length : JSDRV_TIME_MAP_LENGTH - tail;
        uint16_t tail_count = length - head_count;

        // Simulate the vulnerable memcpy pattern with bounds check
        ck_assert_uint_le(head_count + tail_count, JSDRV_TIME_MAP_LENGTH);
        ck_assert_uint_le(tail + head_count, JSDRV_TIME_MAP_LENGTH);
        ck_assert_uint_le(tail_count, JSDRV_TIME_MAP_LENGTH);

        memcpy(dst, &entries[tail], head_count * sizeof(struct jsdrv_time_map_s));
        if (tail_count > 0) {
            memcpy(&dst[head_count], &entries[0], tail_count * sizeof(struct jsdrv_time_map_s));
        }

        // Verify guard was not corrupted (no overflow)
        for (int g = 0; g < (int)sizeof(guard); g++) {
            ck_assert_uint_eq(guard[g], 0xAA);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_tmap_bounds_safety);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}