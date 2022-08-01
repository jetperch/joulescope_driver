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
#include <stdio.h>


static void on_pub(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) self;
    char buf[32];
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("on_pub(%s, %s)\n", topic, buf);
}

int on_dev(struct app_s * self, int argc, char * argv[]) {
    printf("CAUTION: developer tools - not intended for normal operation!\n");
    while (argc) {
        printf("usage: jsdrv_util dev\n");
        return 1;
    }

    jsdrv_subscribe(self->context, "@", JSDRV_SFLAG_PUB, on_pub, self, 1000);

    return 0;
}