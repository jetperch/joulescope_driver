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
#include "jsdrv_prv/js110_sample_processor.h"



static void test_basic(void ** state) {
    (void) state;
    struct js110_sp_s s;
    js110_sp_initialize(&s);
    for (int i = 0; i < 9; ++i) {
        s.cal[0][0][i] = (i + 1) * 100.0;
        s.cal[0][1][i] = pow(10, -3 - i);
    }
    for (int i = 0; i < 2; ++i) {
        s.cal[1][0][0] = (i + 1) * -100.0;
        s.cal[1][1][i] = pow(10, -4 - i);
    }
    struct js110_sample_s z = js110_sp_process(&s, 0, 0);

}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_basic),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
