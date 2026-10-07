/*
 * SPDX-FileCopyrightText: Copyright 2022 Jetperch LLC
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
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include "jsdrv/meta.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/meta_settable.h"

#define cstr(_value) ((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=0, .flags=JSDRV_UNION_FLAG_CONST, .app=0, .value={.str=_value}, .size=(uint32_t) (strlen(_value) + 1)})

const char * META1 = "{"
    "\"dtype\": \"u8\","
    "\"brief\": \"Number selection.\","
    "\"default\": 2,"
    "\"options\": ["
        "[0, \"zero\"],"
        "[1, \"one\"],"
        "[2, \"two\"],"
        "[3, \"three\", \"_3_\"],"
        "[4, \"four\"],"
        "[5, \"five\"],"
        "[6, \"six\"],"
        "[7, \"seven\"],"
        "[8, \"eight\"],"
        "[9, \"nine\"],"
        "[10, \"ten\"]"
    "]"
"}";

const char * META_NO_DEFAULT = "{"
    "\"dtype\": \"u8\","
    "\"brief\": \"Number selection.\""
"}";

const char * META_F32 = "{"
    "\"dtype\": \"f32\","
    "\"brief\": \"A scale factor.\","
    "\"default\": 1.0"
"}";

const char * META_F64 = "{"
    "\"dtype\": \"f64\","
    "\"brief\": \"A scale factor.\","
    "\"default\": 1.0"
"}";

static void test_float_dtype(void **state) {
    (void) state;
    uint8_t dtype = 0;
    struct jsdrv_union_s value;

    assert_int_equal(0, jsdrv_meta_dtype(META_F32, &dtype));
    assert_int_equal(JSDRV_UNION_F32, dtype);
    assert_int_equal(0, jsdrv_meta_dtype(META_F64, &dtype));
    assert_int_equal(JSDRV_UNION_F64, dtype);

    // A value for a float-typed topic must validate (no options/range):
    // dtype_lookup must know "f32"/"f64", else jsdrv_meta_value rejects it.
    value = jsdrv_union_f32(2.0f);
    assert_int_equal(0, jsdrv_meta_value(META_F32, &value));
    value = jsdrv_union_f64(2.0);
    assert_int_equal(0, jsdrv_meta_value(META_F32, &value));  // f64 host value, f32 meta
    value = jsdrv_union_f64(2.0);
    assert_int_equal(0, jsdrv_meta_value(META_F64, &value));
}

// As emitted by meta_binary.c for a MiniBitty bool topic.
static const char META_BOOL[] = "{\"dtype\": \"bool\", \"default\": 0}";

static void test_bool_dtype(void **state) {
    (void) state;
    uint8_t dtype = 0;
    struct jsdrv_union_s value;
    assert_int_equal(0, jsdrv_meta_dtype(META_BOOL, &dtype));
    assert_int_equal(JSDRV_UNION_U8, dtype);
    assert_int_equal(0, jsdrv_meta_default(META_BOOL, &value));
    assert_true(jsdrv_union_equiv(&jsdrv_union_u8(0), &value));

    const char * true_str[] = {"on", "ON", "True", "yes", "Enable", "1", NULL};
    for (const char ** s = true_str; *s; ++s) {
        value = cstr(*s);
        assert_int_equal(0, jsdrv_meta_value(META_BOOL, &value));
        assert_true(jsdrv_union_eq(&jsdrv_union_u8(1), &value));
    }
    const char * false_str[] = {"off", "OFF", "False", "no", "disabled", "0", NULL};
    for (const char ** s = false_str; *s; ++s) {
        value = cstr(*s);
        assert_int_equal(0, jsdrv_meta_value(META_BOOL, &value));
        assert_true(jsdrv_union_eq(&jsdrv_union_u8(0), &value));
    }
    value = jsdrv_union_u32(1);
    assert_int_equal(0, jsdrv_meta_value(META_BOOL, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(1), &value));
    value = jsdrv_union_u32(0);
    assert_int_equal(0, jsdrv_meta_value(META_BOOL, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(0), &value));

    value = cstr("maybe");
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_value(META_BOOL, &value));
}

static void test_basic(void **state) {
    (void) state;
    uint8_t dtype = 0;
    struct jsdrv_union_s value = jsdrv_union_null();
    assert_int_equal(0, jsdrv_meta_syntax_check(META1));

    assert_int_equal(0, jsdrv_meta_dtype(META1, &dtype));
    assert_int_equal(JSDRV_UNION_U8, dtype);

    assert_int_equal(0, jsdrv_meta_default(META1, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(2), &value));
    assert_true(value.flags & JSDRV_UNION_FLAG_RETAIN);
}

static void test_value(void **state) {
    (void) state;
    struct jsdrv_union_s value;
    value = jsdrv_union_u8(3);
    assert_int_equal(0, jsdrv_meta_value(META1, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(3), &value));

    value = cstr("three");
    assert_int_equal(0, jsdrv_meta_value(META1, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(3), &value));

    value = cstr("_3_");
    assert_int_equal(0, jsdrv_meta_value(META1, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(3), &value));

    value = cstr("2");
    assert_int_equal(0, jsdrv_meta_value(META1, &value));
    assert_true(jsdrv_union_eq(&jsdrv_union_u8(2), &value));

    value = cstr("__invalid__");
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_value(META1, &value));
}

static void test_range_too_long_rejected(void **state) {
    // range must be [v_min, v_max] or [v_min, v_max, v_step]; a longer
    // array previously overflowed the internal 3-entry parse buffer.
    (void) state;
    static const char * META_RANGE_BAD =
        "{"
        "\"dtype\": \"u32\","
        "\"range\": [0, 100, 1, 2, 3, 4, 5, 6, 7, 8]"
        "}";
    struct jsdrv_union_s value = jsdrv_union_u32(10);
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_value(META_RANGE_BAD, &value));

    static const char * META_RANGE_OK =
        "{"
        "\"dtype\": \"u32\","
        "\"range\": [0, 100, 2]"
        "}";
    value = jsdrv_union_u32(10);
    assert_int_equal(0, jsdrv_meta_value(META_RANGE_OK, &value));
}

static void test_no_default(void **state) {
    (void) state;
    struct jsdrv_union_s value = jsdrv_union_null();
    assert_int_equal(0, jsdrv_meta_syntax_check(META_NO_DEFAULT));
    assert_int_equal(0, jsdrv_meta_default(META_NO_DEFAULT, &value));
    assert_int_equal(JSDRV_UNION_NULL, value.type);
    assert_false(value.flags & JSDRV_UNION_FLAG_RETAIN);
}

static void test_is_settable(void **state) {
    (void) state;
    assert_true(jsdrv_meta_is_settable("s/x/sel", META1));
    assert_true(jsdrv_meta_is_settable("h/scale", META_F32));
    assert_true(jsdrv_meta_is_settable("s/x/none", META_NO_DEFAULT));  // caller checks default
}

static void test_is_settable_excluded(void **state) {
    (void) state;
    const char * meta_ro = "{\"dtype\": \"u8\", \"default\": 1, \"flags\": [\"ro\"]}";
    const char * meta_str = "{\"dtype\": \"str\", \"default\": \"hi\"}";
    const char * meta_clear = "{\"dtype\": \"bool\", \"default\": 0}";
    assert_false(jsdrv_meta_is_settable("s/x/ro", meta_ro));
    assert_false(jsdrv_meta_is_settable("s/stats/!clear", meta_clear));  // JS220 fw 1.3.0
    assert_false(jsdrv_meta_is_settable("s/gpo/+/!set", META1));
    assert_false(jsdrv_meta_is_settable("s/x/str", meta_str));
    assert_false(jsdrv_meta_is_settable(NULL, META1));
    assert_false(jsdrv_meta_is_settable("s/x/sel", NULL));
}

static void test_flags_none(void **state) {
    (void) state;
    uint32_t flags = 0xFFFFFFFF;
    assert_int_equal(0, jsdrv_meta_flags(META1, &flags));
    assert_int_equal(0, flags);
}

static void test_flags_no_flags_key(void **state) {
    (void) state;
    uint32_t flags = 0xFFFFFFFF;
    assert_int_equal(0, jsdrv_meta_flags(META_NO_DEFAULT, &flags));
    assert_int_equal(0, flags);
}

static void test_flags_ro_string(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": \"ro\"}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_RO, flags);
}

static void test_flags_hide_string(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": \"hide\"}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_HIDE, flags);
}

static void test_flags_dev_string(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": \"dev\"}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_DEV, flags);
}

static void test_flags_array_single(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": [\"ro\"]}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_RO, flags);
}

static void test_flags_array_multiple(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": [\"ro\", \"hide\"]}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_RO | JSDRV_META_FLAG_HIDE, flags);
}

static void test_flags_array_all(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": [\"ro\", \"hide\", \"dev\"]}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_RO | JSDRV_META_FLAG_HIDE | JSDRV_META_FLAG_DEV, flags);
}

static void test_flags_unknown_ignored(void **state) {
    (void) state;
    const char * meta = "{\"dtype\": \"u8\", \"flags\": [\"ro\", \"no_traverse\"]}";
    uint32_t flags = 0;
    assert_int_equal(0, jsdrv_meta_flags(meta, &flags));
    assert_int_equal(JSDRV_META_FLAG_RO, flags);
}

static void test_flags_null_params(void **state) {
    (void) state;
    uint32_t flags = 0;
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_flags(NULL, &flags));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_flags("{}", NULL));
}

static void test_syntax_check_null(void **state) {
    (void) state;
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_meta_syntax_check(NULL));
}

static void test_syntax_check_valid(void **state) {
    (void) state;
    assert_int_equal(0, jsdrv_meta_syntax_check("{\"dtype\": \"u8\"}"));
    assert_int_equal(0, jsdrv_meta_syntax_check("{}"));
    assert_int_equal(0, jsdrv_meta_syntax_check(META1));
    assert_int_equal(0, jsdrv_meta_syntax_check(META_NO_DEFAULT));
}

static void test_syntax_check_non_object_root(void **state) {
    (void) state;
    // Valid JSON but not a top-level object: must be rejected.
    assert_int_not_equal(0, jsdrv_meta_syntax_check("[\"a\", \"b\"]"));
    assert_int_not_equal(0, jsdrv_meta_syntax_check("\"hello\""));
    assert_int_not_equal(0, jsdrv_meta_syntax_check("42"));
}

static void test_syntax_check_malformed(void **state) {
    (void) state;
    assert_int_not_equal(0, jsdrv_meta_syntax_check("{\"dtype\":"));
    assert_int_not_equal(0, jsdrv_meta_syntax_check("{"));
    assert_int_not_equal(0, jsdrv_meta_syntax_check("not json"));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_basic),
            cmocka_unit_test(test_value),
            cmocka_unit_test(test_float_dtype),
            cmocka_unit_test(test_bool_dtype),
            cmocka_unit_test(test_range_too_long_rejected),
            cmocka_unit_test(test_no_default),
            cmocka_unit_test(test_is_settable),
            cmocka_unit_test(test_is_settable_excluded),
            cmocka_unit_test(test_flags_none),
            cmocka_unit_test(test_flags_no_flags_key),
            cmocka_unit_test(test_flags_ro_string),
            cmocka_unit_test(test_flags_hide_string),
            cmocka_unit_test(test_flags_dev_string),
            cmocka_unit_test(test_flags_array_single),
            cmocka_unit_test(test_flags_array_multiple),
            cmocka_unit_test(test_flags_array_all),
            cmocka_unit_test(test_flags_unknown_ignored),
            cmocka_unit_test(test_flags_null_params),
            cmocka_unit_test(test_syntax_check_null),
            cmocka_unit_test(test_syntax_check_valid),
            cmocka_unit_test(test_syntax_check_non_object_root),
            cmocka_unit_test(test_syntax_check_malformed),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
