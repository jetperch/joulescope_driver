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

#include "jsdrv_util_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv/version.h"
#include <stdio.h>

static int get_version(struct app_s * self) {
    char version_str[32] = "";
    struct jsdrv_union_s value = jsdrv_union_null();
    ROE(jsdrv_query(self->context, JSDRV_MSG_VERSION, &value, 0));
    jsdrv_version_u32_to_str(value.value.u32, version_str, sizeof(version_str));
    printf("version = %s\n", version_str);
    return 0;
}

int on_version(struct app_s * self, int argc, char * argv[]) {
    (void) argc;
    (void) argv;
    printf("jsdrv: Joulescope driver utility\n");
    printf("platform = %s (%d-bit)\n", PLATFORM, (int) (8 * sizeof(size_t)));
    ROE(get_version(self));
    return 0;
}
