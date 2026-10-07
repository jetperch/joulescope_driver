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
 * Standalone unit test for device_match, which mirrors
 * pyjoulescope_driver/test/test_device_filter.py.  No jsdrv dependency.
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
    CHECK(device_match(p, "/u/js320/8W2A/"));
    CHECK(device_match(p, "js320/8W2A"));
    CHECK(device_match(p, "js320"));
    CHECK(device_match(p, "8W2A"));
    CHECK(device_match(p, "8w2a"));
    CHECK(device_match(p, "u/js320"));     // backend/model
    CHECK(device_match(p, "u/js320/"));
    CHECK(device_match(p, "U/JS320/"));

    CHECK(!device_match(NULL, NULL));
    CHECK(!device_match(p, "u"));
    CHECK(!device_match(p, "u/"));
    CHECK(!device_match(p, "x/js320"));
    CHECK(!device_match(p, "u/js32"));
    CHECK(!device_match(p, "u/js320/8"));
    CHECK(!device_match(p, "8"));
    CHECK(!device_match(p, "W2A"));
    CHECK(!device_match(p, "js32"));
    CHECK(!device_match(p, "js220"));
    CHECK(!device_match(p, "js320/8"));
    CHECK(!device_match(p, "u/js220/"));
}

static void test_match_model_dash_serial_number(void) {
    printf("test_match_model_dash_serial_number:\n");
    const char * p = "u/js320/Y9S4";
    CHECK(device_match(p, "js320-Y9S4"));
    CHECK(device_match(p, "jS320-Y9s4"));
    CHECK(device_match(p, "JS320-y9s4"));
    CHECK(!device_match(p, "js320-Y9"));
    CHECK(!device_match(p, "js320-9S4"));
    CHECK(!device_match(p, "js220-Y9S4"));
    CHECK(!device_match(p, "js320-"));
    CHECK(!device_match(p, "-Y9S4"));
    CHECK(!device_match(p, "u/js320-Y9S4"));
    CHECK(!device_match(p, "js320_Y9S4"));
}

static void test_match_comma_separated(void) {
    printf("test_match_comma_separated:\n");
    const char * p = "u/js320/8W2A";
    CHECK(device_match(p, "js220,8W2A"));
    CHECK(device_match(p, " js220 , 8W2A "));
    CHECK(device_match(p, "8W2A,"));
    CHECK(device_match(p, ",8W2A,,"));
    CHECK(device_match(p, " , "));
    CHECK(device_match(p, ","));
    CHECK(!device_match(p, "js220,8"));
    CHECK(!device_match(p, "js220,"));
    CHECK(!device_match(p, " / js320 / "));  // trim whitespace, then "/"
}

static void test_match_malformed(void) {
    printf("test_match_malformed:\n");
    CHECK(!device_match("u/js320", "u/js320"));
    CHECK(!device_match("u/js320", "js320"));
    CHECK(!device_match("u/js320/1/2", "1"));
    CHECK(device_match("u/js320", NULL));
}

static void test_brand(void) {
    printf("test_brand:\n");
    CHECK(0 == strcmp("Joulescope", device_brand_validate("Joulescope")));
    CHECK(0 == strcmp("Joulescope", device_brand_validate("joulescope")));
    CHECK(0 == strcmp("Joulescope", device_brand_validate("js")));
    CHECK(0 == strcmp("Joulescope", device_brand_validate("JS")));
    CHECK(NULL == device_brand_validate("acme"));
    CHECK(NULL == device_brand_validate(""));
    CHECK(NULL == device_brand_validate(NULL));

    CHECK(0 == strcmp("Joulescope", device_brand("u/js110/1")));
    CHECK(0 == strcmp("Joulescope", device_brand("u/JS220/1")));
    CHECK(0 == strcmp("Joulescope", device_brand("u/js320/1")));
    CHECK(NULL == device_brand("u/mb/1"));
    CHECK(NULL == device_brand("u/js999/1"));
    CHECK(NULL == device_brand("u/js320"));
    CHECK(NULL == device_brand(NULL));

    CHECK(device_is_brand("u/js320/1", NULL));
    CHECK(device_is_brand("u/mb/1", NULL));
    CHECK(device_is_brand("u/js320/1", "joulescope"));
    CHECK(device_is_brand("u/js320/1", "js"));
    CHECK(!device_is_brand("u/mb/1", "js"));
    CHECK(!device_is_brand("u/js320/1", "acme"));
}

static void test_is_joulescope(void) {
    printf("test_is_joulescope:\n");
    CHECK(device_is_joulescope("u/js110/000123"));
    CHECK(device_is_joulescope("u/js220/000415"));
    CHECK(device_is_joulescope("u/js320/8W2A"));
    CHECK(device_is_joulescope("u/JS320/8W2A"));
    CHECK(!device_is_joulescope("u/js999/1"));
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
    CHECK(4 == device_match_list(d, NULL, NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js220/000415"));
    CHECK(2 == device_match_list(d, "js320", NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js320/8W2A"));
    CHECK(1 == device_match_list(d, "31NB", NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/js320/31NB"));
    CHECK(1 == device_match_list(d, "u/mb/1", NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/mb/1"));
    CHECK(1 == device_match_list(d, "mb", NULL, m, sizeof(m)));
    CHECK(2 == device_match_list(d, "js220,31NB", NULL, NULL, 0));
    CHECK(2 == device_match_list(d, "js320-8w2a, 1", NULL, NULL, 0));
    CHECK(0 == device_match_list(d, "js110", NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, ""));
    CHECK(0 == device_match_list("", NULL, NULL, m, sizeof(m)));
    CHECK(0 == device_match_list(NULL, NULL, NULL, m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A", "8W2A", NULL, m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A,", "8W2A", NULL, m, sizeof(m)));
    CHECK(1 == device_match_list("u/js320/8W2A", "8W2A", NULL, m, 6));
    CHECK(0 == strcmp(m, "u/js3"));
}

static void test_match_list_brand(void) {
    printf("test_match_list_brand:\n");
    const char * d = "u/js220/000415,u/js320/8W2A,u/js320/31NB,u/mb/1";
    char m[64];
    CHECK(3 == device_match_list(d, NULL, "joulescope", m, sizeof(m)));
    CHECK(3 == device_match_list(d, "", "js", NULL, 0));
    CHECK(1 == device_match_list(d, "1", NULL, m, sizeof(m)));
    CHECK(0 == strcmp(m, "u/mb/1"));
    CHECK(0 == device_match_list(d, "1", "joulescope", m, sizeof(m)));
    CHECK(0 == device_match_list(d, "mb", "js", NULL, 0));
    CHECK(0 == device_match_list(d, NULL, "acme", NULL, 0));
}

int main(void) {
    test_match();
    test_match_model_dash_serial_number();
    test_match_comma_separated();
    test_match_malformed();
    test_brand();
    test_is_joulescope();
    test_is_model();
    test_match_list();
    test_match_list_brand();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
