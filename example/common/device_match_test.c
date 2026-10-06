/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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
 *
 * Standalone unit test for device_match.  No jsdrv dependency.
 * Build (mingw):  gcc -std=c11 device_match.c device_match_test.c -o device_match_test
 */

#include "device_match.h"
#include <stdio.h>
#include <string.h>

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

static void test_is_model(void) {
    printf("test_is_model:\n");
    CHECK(device_is_model("u/js320/8W2A", "js320"));
    CHECK(device_is_model("u/JS320/8W2A", "js320"));
    CHECK(device_is_model("u/js220/000415", "JS220"));
    CHECK(!device_is_model("u/js220/000415", "js22"));
    CHECK(!device_is_model("u/js220/000415", "js2200"));
    CHECK(!device_is_model("u/mb/1", "js320"));
    CHECK(!device_is_model("u/js320", "js320"));
    CHECK(!device_is_model(NULL, "js320"));
    CHECK(!device_is_model("u/js320/8W2A", NULL));
}

static void test_match_list(void) {
    printf("test_match_list:\n");
    const char * d = "u/js220/000415,u/js320/8W2A,u/js320/31NB,u/mb/1";
    char m[64];
    CHECK(4 == device_match_list(d, NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js220/000415"));
    CHECK(2 == device_match_list(d, "js320", m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js320/8W2A"));
    CHECK(1 == device_match_list(d, "31NB", m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js320/31NB"));
    CHECK(1 == device_match_list(d, "u/mb/1", m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/mb/1"));
    CHECK(0 == device_match_list(d, "js110", m, sizeof(m)));
    CHECK(0 == strcmp(m, ""));
    CHECK(2 == device_match_list(d, "u/js320/", NULL, 0));
    CHECK(0 == device_match_list("", NULL, m, sizeof(m)));
    CHECK(0 == device_match_list(NULL, NULL, m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A", "8W2A", m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A,", "8W2A", m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A", "8W2A", m, 6));
    CHECK(0 == strcmp(m, "u/js3"));
}

int main(void) {
    test_match();
    test_match_malformed();
    test_is_joulescope();
    test_is_model();
    test_match_list();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
