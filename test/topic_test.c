/*
 * Copyright 2021 Jetperch LLC
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
#include "jsdrv/topic.h"
#include <string.h>

void jsdrv_fatal(const char * file, uint32_t line, const char * msg) {
    (void) file;
    (void) line;
    mock_assert(0, msg, file, line);
}

#define SETUP() \
    (void) state; \
    struct jsdrv_topic_s t = JSDRV_TOPIC_INIT


#define assert_topic(str, t) \
    assert_string_equal((str), (t)->topic); \
    assert_int_equal(strlen(str), (t)->length)


static void test_append(void **state) {
    SETUP();
    assert_int_equal(0, t.length);
    jsdrv_topic_append(&t, "a123456");
    jsdrv_topic_append(&t, "b123456");
    jsdrv_topic_append(&t, "c123456");
    jsdrv_topic_append(&t, "d123456");
    assert_topic("a123456/b123456/c123456/d123456", &t);
    jsdrv_topic_append(&t, "e123456");
    jsdrv_topic_append(&t, "f123456");
    jsdrv_topic_append(&t, "g123456");
    jsdrv_topic_append(&t, "h123456");
    assert_topic("a123456/b123456/c123456/d123456/e123456/f123456/g123456/h123456", &t);
    expect_assert_failure(jsdrv_topic_append(&t, "a"));  // too long
}

static void test_clear(void **state) {
    SETUP();
    jsdrv_topic_append(&t, "hello");
    jsdrv_topic_clear(&t);
    assert_topic("", &t);
}

static void test_truncate(void **state) {
    SETUP();
    jsdrv_topic_append(&t, "hello");
    uint8_t length = t.length;
    jsdrv_topic_append(&t, "world");
    jsdrv_topic_truncate(&t, length);
    assert_topic("hello", &t);
}

static void test_set(void **state) {
    SETUP();
    jsdrv_topic_set(&t, "hello");
    assert_topic("hello", &t);
    expect_assert_failure(jsdrv_topic_set(&t, "a123456/b123456/c123456/d123456/e123456/f123456/g123456/h123456/a"));
}

static void test_suffix_add(void **state) {
    SETUP();
    jsdrv_topic_suffix_add(&t, '#');
    assert_topic("#", &t);

    jsdrv_topic_set(&t, "hello");
    jsdrv_topic_suffix_add(&t, '#');
    assert_topic("hello#", &t);

    jsdrv_topic_set(&t, "hello/");
    jsdrv_topic_suffix_add(&t, '#');
    assert_topic("hello/#", &t);

    jsdrv_topic_set(&t, "01234567/01234567/01234567/012");
    jsdrv_topic_suffix_add(&t, '#');
    assert_topic("01234567/01234567/01234567/012#", &t);

    jsdrv_topic_set(&t, "a123456/b123456/c123456/d123456/e123456/f123456/g123456/h123456");
    expect_assert_failure(jsdrv_topic_suffix_add(&t, '#'));
}

static void test_suffix_remove(void **state) {
    SETUP();

    jsdrv_topic_set(&t, "hello/there/world");
    assert_int_equal(0, jsdrv_topic_suffix_remove(&t));
    assert_string_equal("hello/there/world", t.topic);

    char suffix[] = "%$&?#";
    for (char *ch = &suffix[0]; *ch; ++ch) {
        jsdrv_topic_set(&t, "hello/there/world");
        jsdrv_topic_suffix_add(&t, *ch);
        assert_int_equal(*ch, jsdrv_topic_suffix_remove(&t));
        assert_string_equal("hello/there/world", t.topic);
    }
}

static void test_remove(void **state) {
    SETUP();

    jsdrv_topic_set(&t, "hello/there/world");
    assert_int_equal(6, jsdrv_topic_remove(&t));
    assert_string_equal("hello/there", t.topic);

    jsdrv_topic_set(&t, "hello/there/world/");
    assert_int_equal(7, jsdrv_topic_remove(&t));
    assert_string_equal("hello/there", t.topic);

    jsdrv_topic_set(&t, "0");
    assert_int_equal(1, jsdrv_topic_remove(&t));
    assert_string_equal("", t.topic);

    jsdrv_topic_set(&t, "/");
    assert_int_equal(1, jsdrv_topic_remove(&t));
    assert_string_equal("", t.topic);
}

JSDRV_API int32_t jsdrv_topic_remove(struct jsdrv_topic_s * topic);

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_append),
            cmocka_unit_test(test_clear),
            cmocka_unit_test(test_truncate),
            cmocka_unit_test(test_set),
            cmocka_unit_test(test_suffix_add),
            cmocka_unit_test(test_suffix_remove),
            cmocka_unit_test(test_remove),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
