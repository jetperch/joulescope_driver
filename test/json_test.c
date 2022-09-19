/*
 * Copyright 2022 Jetperch LLC
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
#include <math.h>
#include "jsdrv_prv/json.h"
#include "jsdrv/error_code.h"

// include null terminator in size!
#define cstr_sz(_value, _size) ((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=0, .flags=JSDRV_UNION_FLAG_CONST, .app=0, .value={.str=(_value)}, .size=(uint32_t) (_size)})
#define cstr(_value) cstr_sz((_value), strlen(_value) + 1)

#define check_expected_ptr_value(parameter) \
    _check_expected(__func__, "value", __FILE__, __LINE__, \
                    cast_ptr_to_largest_integral_type(parameter))

#define check_expected_value(parameter) \
    _check_expected(__func__, "value", __FILE__, __LINE__, \
                    cast_to_largest_integral_type(parameter))

static int32_t on_token(void * user_data, const struct jsdrv_union_s * token) {
    (void) user_data;
    uint8_t type = token->type;
    uint8_t op = token->op;
    check_expected(type);
    check_expected(op);
    switch (type) {
        case JSDRV_UNION_NULL: break;
        case JSDRV_UNION_STR: {
            size_t len = token->size;
            check_expected(len);
            check_expected_ptr_value(token->value.str);
            break;
        }
        case JSDRV_UNION_I32: check_expected_value(token->value.i32); break;
        case JSDRV_UNION_F64: check_expected_value(token->value.u64); break;  // check binary representation
        default:
            assert_true(false);
    }
    return 0;
}

#define expect_tk(token__) {                                                                        \
    const struct jsdrv_union_s * token_ = (token__);                                                \
    expect_value(on_token, type, token_->type);                                                     \
    expect_value(on_token, op, token_->op);                                                         \
    switch (token_->type) {                                                                         \
        case JSDRV_UNION_NULL:  break;                                                              \
        case JSDRV_UNION_STR: {                                                                     \
            size_t len = strlen(token_->value.str) + 1; /* include null terminator */               \
            expect_value(on_token, len, len);                                                       \
            expect_memory(on_token, value, token_->value.str, len - 1);                             \
            break;                                                                                  \
        }                                                                                           \
        case JSDRV_UNION_I32:  expect_value(on_token, value, token_->value.i32); break;             \
        case JSDRV_UNION_F64:  expect_value(on_token, value, token_->value.u64); break;             \
        default: assert_true(false); break;                                                         \
    }                                                                                               \
}

#define expect_delim(op__) expect_tk(&((struct jsdrv_union_s){.type=JSDRV_UNION_NULL, .op=op__, .flags=0, .app=0, .value={.u64=0}, .size=0}))
#define expect_object_start() expect_delim(JSDRV_JSON_OBJ_START)
#define expect_object_end() expect_delim(JSDRV_JSON_OBJ_END)
#define expect_array_start() expect_delim(JSDRV_JSON_ARRAY_START)
#define expect_array_end() expect_delim(JSDRV_JSON_ARRAY_END)
#define expect_key(value__) expect_tk(&((struct jsdrv_union_s){.type=JSDRV_UNION_STR, .op=JSDRV_JSON_KEY, .flags=0, .app=0, .value={.str=value__}, .size=(uint32_t) strlen(value__)}))

static void test_empty(void **state) {
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID, jsdrv_json_parse(NULL, on_token, *state));
    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse("", on_token, *state));
    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse("    ", on_token, *state));
    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse("  \r\n\t  ", on_token, *state));
}

static void test_value_string(void **state) {
    expect_tk(&jsdrv_union_cstr("hello"));
    assert_int_equal(0, jsdrv_json_parse("   \"hello\"   ", on_token, *state));
    expect_tk(&jsdrv_union_cstr("hello\\n"));
    assert_int_equal(0, jsdrv_json_parse("   \"hello\\n\"   ", on_token, *state));
}

static void test_value_i32(void **state) {
    expect_tk(&jsdrv_union_i32(0));
    assert_int_equal(0, jsdrv_json_parse("   0   ", on_token, *state));
    expect_tk(&jsdrv_union_i32(42));
    assert_int_equal(0, jsdrv_json_parse("  \n42\t   ", on_token, *state));
    expect_tk(&jsdrv_union_i32(-42));
    assert_int_equal(0, jsdrv_json_parse("  \n-42\t   ", on_token, *state));
}

static void test_value_literals(void **state) {
    expect_tk(&jsdrv_union_null());
    assert_int_equal(0, jsdrv_json_parse("null", on_token, *state));
    expect_tk(&jsdrv_union_null());
    assert_int_equal(0, jsdrv_json_parse("   null   ", on_token, *state));
    expect_tk(&jsdrv_union_i32(0));
    assert_int_equal(0, jsdrv_json_parse("   false   ", on_token, *state));
    expect_tk(&jsdrv_union_i32(1));
    assert_int_equal(0, jsdrv_json_parse("   true   ", on_token, *state));

    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse("goober", on_token, *state));
}

static void test_obj_empty(void **state) {
    expect_object_start();
    expect_object_end();
    assert_int_equal(0, jsdrv_json_parse("   {\r\n\t    \n}\n   ", on_token, *state));
}

static void test_obj_1(void **state) {
    expect_object_start();
    expect_key("hello");
    expect_tk(&jsdrv_union_cstr("world"));
    expect_object_end();
    assert_int_equal(0, jsdrv_json_parse("{ \"hello\": \"world\" }", on_token, *state));
}

static void test_obj_N(void **state) {
    expect_object_start();
    expect_key("hello");
    expect_tk(&jsdrv_union_cstr("world"));
    expect_key("json");
    expect_tk(&jsdrv_union_cstr("parse"));
    expect_object_end();
    assert_int_equal(0, jsdrv_json_parse("{ \"hello\":\"world\", \"json\" : \"parse\" }", on_token, *state));
}

static void test_obj_trailing_comma(void **state) {
    expect_object_start();
    expect_key("hello");
    expect_tk(&jsdrv_union_cstr("world"));
    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse("{ \"hello\":\"world\", }", on_token, *state));
}

static void test_array_1(void **state) {
    expect_array_start();
    expect_tk(&jsdrv_union_i32(1));
    expect_array_end();
    assert_int_equal(0, jsdrv_json_parse(" [ 1 ]", on_token, *state));
}

static void test_array_N(void **state) {
    expect_array_start();
    expect_tk(&jsdrv_union_i32(1));
    expect_tk(&jsdrv_union_i32(2));
    expect_tk(&jsdrv_union_i32(3));
    expect_tk(&jsdrv_union_cstr("apple"));
    expect_tk(&jsdrv_union_cstr("orange"));
    expect_array_end();
    assert_int_equal(0, jsdrv_json_parse(" [ 1, 2, 3, \"apple\", \"orange\" ]", on_token, *state));
}

static void test_array_trailing_comma(void **state) {
    expect_array_start();
    expect_tk(&jsdrv_union_i32(1));
    assert_int_equal(JSDRV_ERROR_SYNTAX_ERROR, jsdrv_json_parse(" [ 1, ]", on_token, *state));
}

static void test_strcmp(void **state) {
    (void) state;
    assert_int_equal(-2, jsdrv_json_strcmp(NULL, &cstr("b")));  // include null terminator in size!
    assert_int_equal(-1, jsdrv_json_strcmp("", &cstr("b")));
    assert_int_equal(-1, jsdrv_json_strcmp("a", &cstr("b")));
    assert_int_equal(0, jsdrv_json_strcmp("b", &cstr("b")));
    assert_int_equal(1, jsdrv_json_strcmp("c", &cstr("b")));

    assert_int_equal(0, jsdrv_json_strcmp("hello", &cstr("hello")));
    assert_int_equal(-1, jsdrv_json_strcmp("hell", &cstr("hello")));

    assert_int_equal(0, jsdrv_json_strcmp("hello", &cstr_sz("hello world", 6)));
    assert_int_equal(1, jsdrv_json_strcmp("hello", &cstr_sz("hello world", 5)));
    assert_int_equal(-1, jsdrv_json_strcmp("hello", &cstr_sz("hello world", 7)));
}

static void test_f64(void **state) {
    (void) state;
    expect_array_start();
    expect_tk(&jsdrv_union_f64(2.25));
    expect_tk(&jsdrv_union_f64(-2.25));
    expect_tk(&jsdrv_union_f64(0.25));
    expect_tk(&jsdrv_union_f64(22.5));
    expect_tk(&jsdrv_union_f64(-22.5));
    expect_tk(&jsdrv_union_f64(200.0));
    expect_tk(&jsdrv_union_f64(200.0));
    expect_tk(&jsdrv_union_f64(0.01));
    expect_array_end();
    assert_int_equal(0, jsdrv_json_parse(" [ 2.25, -2.25, 0.25, 2.25e1, -2.25e1, 2e2, +2.0e+2.0, 1.0e-2.0]", on_token, *state));
}

static int32_t on_token_expect_f64_nan(void * user_data, const struct jsdrv_union_s * token) {
    (void) user_data;
    assert_int_equal(JSDRV_UNION_F64, token->type);
    assert_true(isnan(token->value.f64));
    return 0;
}

static void test_f64_nan(void **state) {
    assert_int_equal(0, jsdrv_json_parse("NaN", on_token_expect_f64_nan, *state));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_value_string),
            cmocka_unit_test(test_value_i32),
            cmocka_unit_test(test_value_literals),
            cmocka_unit_test(test_obj_empty),
            cmocka_unit_test(test_obj_1),
            cmocka_unit_test(test_obj_N),
            cmocka_unit_test(test_obj_trailing_comma),
            cmocka_unit_test(test_array_1),
            cmocka_unit_test(test_array_N),
            cmocka_unit_test(test_array_trailing_comma),

            cmocka_unit_test(test_strcmp),
            cmocka_unit_test(test_f64),
            cmocka_unit_test(test_f64_nan),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
