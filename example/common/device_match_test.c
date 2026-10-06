/*
 * Copyright 2026 Jetperch LLC
 *
 * Standalone unit test for device_match.  No jsdrv dependency.
 * Build (mingw):  gcc -std=c11 device_match.c device_match_test.c -o device_match_test
 */

#include "device_match.h"
#include <stdio.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (cond) { \
        printf("  PASS: %s\n", #cond); \
    } else { \
        printf("  FAIL: %s  (line %d)\n", #cond, __LINE__); \
        ++failures; \
    } \
} while (0)

static void test_match(void) {
    printf("test_match:\n");
    const char * p = "u/js320/8W2A";
    CHECK(device_match(p, NULL));
    CHECK(device_match(p, ""));
    CHECK(device_match(p, "u/js320/8W2A"));
    CHECK(device_match(p, "U/JS320/8w2a"));
    CHECK(device_match(p, "u/js320"));
    CHECK(device_match(p, "u/js320/"));
    CHECK(device_match(p, "u/"));
    CHECK(device_match(p, "js320/8W2A"));
    CHECK(device_match(p, "js320"));
    CHECK(device_match(p, "8W2A"));
    CHECK(device_match(p, "8w2a"));

    CHECK(!device_match(NULL, NULL));
    CHECK(!device_match(p, "u/js320/8"));
    CHECK(!device_match(p, "u/js320/8W2A/"));
    CHECK(!device_match(p, "8"));
    CHECK(!device_match(p, "W2A"));
    CHECK(!device_match(p, "js32"));
    CHECK(!device_match(p, "js220"));
    CHECK(!device_match(p, "js320/8"));
    CHECK(!device_match(p, "u"));
    CHECK(!device_match(p, "u/js220/"));
}

static void test_match_malformed(void) {
    printf("test_match_malformed:\n");
    CHECK(device_match("u/js320", "u/js320"));
    CHECK(!device_match("u/js320", "js320"));
    CHECK(!device_match("u/js320/1/2", "1"));
}

static void test_is_joulescope(void) {
    printf("test_is_joulescope:\n");
    CHECK(device_is_joulescope("u/js110/000123"));
    CHECK(device_is_joulescope("u/js220/000415"));
    CHECK(device_is_joulescope("u/js320/8W2A"));
    CHECK(device_is_joulescope("u/JS320/8W2A"));
    CHECK(!device_is_joulescope("u/mb/1"));
    CHECK(!device_is_joulescope("u/js320"));
    CHECK(!device_is_joulescope(NULL));
}

int main(void) {
    test_match();
    test_match_malformed();
    test_is_joulescope();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
