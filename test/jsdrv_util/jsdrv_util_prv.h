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

#include "jsdrv.h"
#include "jsdrv/topic.h"

#if defined(_WIN64)
#define PLATFORM "win64"
#elif defined(_WIN32)
#define PLATFORM "win32"
#elif defined(__CYGWIN__)
#define PLATFORM "win cygwin"
#elif defined(__APPLE__) && defined(__MACH__) // Apple OSX and iOS (Darwin)
    #define PLATFORM "osx"
#elif defined(__linux__)
    #define PLATFORM "linux"
#else
    #define PLATFORM "unknown"
#endif

#define MAX_DEVICES_LENGTH (4096U)
#define ARG_CONSUME() --argc; ++argv
#define ARG_REQUIRE()  if (argc <= 0) {return usage();}

#define ROE(x) do {         \
    int rc__ = (x);         \
    if (rc__) {             \
        return rc__;        \
    }                       \
} while (0)

struct app_s {
    struct jsdrv_context_s * context;
    struct jsdrv_topic_s topic;
    int32_t verbose;
    char * filename;
    char devices[MAX_DEVICES_LENGTH];
    struct jsdrv_topic_s device;
};

extern volatile bool quit_;

int app_initialize(struct app_s * self);
int32_t app_scan(struct app_s * self);

bool js220_is_mem_region_valid(const char * region);

/**
 * @brief Match a specified device.
 *
 * @param self The application instance.
 * @param filter The device filter specification or NULL>
 * @return 0 or error code.
 *
 * This method will update self->devices and clear self->device.
 * On success, self->device will contain the matching device string.
 */
int32_t app_match(struct app_s * self, const char * filter);

typedef int (*command_fn)(struct app_s * self, int argc, char * argv[]);

int on_help(struct app_s * self, int argc, char * argv[]);
int on_demo(struct app_s * self, int argc, char * argv[]);
int on_dev(struct app_s * self, int argc, char * argv[]);
int on_hotplug(struct app_s * self, int argc, char * argv[]);
int on_info(struct app_s * self, int argc, char * argv[]);
int on_mem_erase(struct app_s * self, int argc, char * argv[]);
int on_mem_read(struct app_s * self, int argc, char * argv[]);
int on_mem_write(struct app_s * self, int argc, char * argv[]);
int on_reset(struct app_s * self, int argc, char * argv[]);
int on_scan(struct app_s * self, int argc, char * argv[]);
int on_statistics(struct app_s * self, int argc, char * argv[]);
int on_set(struct app_s * self, int argc, char * argv[]);
int on_threads(struct app_s * self, int argc, char * argv[]);
int on_version(struct app_s * self, int argc, char * argv[]);
