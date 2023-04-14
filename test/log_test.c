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
#include <string.h>
#include <stdio.h>
#include "jsdrv/log.h"
#include "jsdrv_prv/log.h"


#define ENTRY_MAX  (1024)


struct state_s {
    uint32_t entries_head;
    uint32_t entries_tail;
    uint8_t level[ENTRY_MAX];
    uint32_t line[ENTRY_MAX];
};


static void log_cbk(void * user_data, struct jsdrv_log_header_s const * header,
               const char * filename, const char * message) {
    struct state_s * state = (struct state_s *) user_data;
    assert_int_equal(1, header->version);
    if (state->entries_head >= ENTRY_MAX) {
        printf("entry_max exceeded\n");
        return;
    }
    state->level[state->entries_head] = header->level;
    state->line[state->entries_head] = header->line;
    ++state->entries_head;
    (void) filename;
    (void) message;
}

#define CHECK(state, level_, line_) \
    assert_true((state)->entries_tail < (state)->entries_head); \
    assert_int_equal(level_, (state)->level[(state)->entries_tail]); \
    assert_int_equal(line_, (state)->line[(state)->entries_tail]);   \
    ++(state)->entries_tail


static void test_register_before_init(void **state) {
    (void) state;
    struct state_s s = {
            .entries_head=0,
            .entries_tail=0,
            .level={0},
            .line={0},
    };
    jsdrv_log_register(log_cbk, &s);
    assert_int_equal(JSDRV_LOG_LEVEL_OFF, jsdrv_log_level_get());
    jsdrv_log_initialize();
    assert_int_equal(JSDRV_LOG_LEVEL_OFF, jsdrv_log_level_get());
    jsdrv_log_level_set(JSDRV_LOG_LEVEL_INFO);
    assert_int_equal(JSDRV_LOG_LEVEL_INFO, jsdrv_log_level_get());

    jsdrv_log_publish(JSDRV_LOG_LEVEL_WARNING, "filename", 42, "%s %s", "hello", "world");
    jsdrv_log_finalize();
    CHECK(&s, JSDRV_LOG_LEVEL_WARNING, 42);
}

static void test_basic(void **state) {
    (void) state;
    struct state_s s = {
            .entries_head=0,
            .entries_tail=0,
            .level={0},
            .line={0},
    };
    jsdrv_log_initialize();
    jsdrv_log_register(log_cbk, &s);
    jsdrv_log_publish(JSDRV_LOG_LEVEL_WARNING, "filename", 42, "%s %s", "hello", "world");
    jsdrv_log_finalize();
    CHECK(&s, JSDRV_LOG_LEVEL_WARNING, 42);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_register_before_init),
            cmocka_unit_test(test_basic),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
