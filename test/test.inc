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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>


void fbp_fatal(char const * file, int line, char const * msg) {
    printf("FATAL: %s:%d: %s\n", file, line, msg);
    fflush(stdout);
    exit(1);
}

void * fbp_alloc_(fbp_size_t size_bytes) {
    void * ptr = test_malloc(size_bytes);
    FBP_ASSERT_ALLOC(ptr);
    //FBP_LOGD3("fbp_alloc_(%d) %p - %p", (int) size_bytes, ptr, ((uint8_t *) ptr) + size_bytes);
    return ptr;
}

void fbp_free_(void * ptr) {
    //FBP_LOGD3("fbp_free_ %p", ptr);
    test_free(ptr);
}

void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    fflush(stderr);
    printf("%c %s:%d %s\n", fbp_log_level_char[level], filename, line, buf);
    fflush(stdout);
}
