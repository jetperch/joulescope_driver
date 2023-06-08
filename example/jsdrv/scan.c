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

#include "jsdrv_prv.h"
#include <stdio.h>
#include <string.h>

int on_scan(struct app_s * self, int argc, char * argv[]) {
    while (argc) {
        if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            printf("usage: jsdrv_util scan [--verbose]\n");
            return 1;
        }
    }

    ROE(app_scan(self));
    if (self->devices[0]) {
        char * d = &self->devices[0];
        size_t sz = strlen(d);
        for (size_t i = 0; i <= sz; ++i) {
            if (self->devices[i] == ',') {
                self->devices[i] = 0;
            }
            if (self->devices[i] == 0) {
                printf("%s\n", d);
                d = &self->devices[i + 1];
            }
        }
    } else {
        if (self->verbose) {
            printf("no devices found\n");
        }
    }
    return 0;
}