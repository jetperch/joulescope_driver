/*
 * SPDX-FileCopyrightText: Copyright 2022-2025 Jetperch LLC
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

/**
 * @file
 *
 * @brief Adapter header.
 */

#ifndef MB_EXAMPLE_MINIBITTY_EXE_PRV_H__
#define MB_EXAMPLE_MINIBITTY_EXE_PRV_H__


#include "jsdrv.h"
#include "mb/cdef.h"
#include "jsdrv/topic.h"

MB_CPP_GUARD_START

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

    // Convenience variables for use by subcommands.
    uint32_t duration_ms;
    uint32_t sleep_ms;
};

extern volatile bool quit_;

int app_initialize(struct app_s * self);
int32_t app_scan(struct app_s * self);

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

/// app_match_ex() flag: refuse the JS110 and JS220, which are not MiniBitty devices.
#define APP_MATCH_MB        (1U << 0)
/// app_match_ex() flag: require a filter that matches exactly one device.
#define APP_MATCH_EXPLICIT  (1U << 1)

/**
 * @brief Match a specified device, with restrictions.
 *
 * @param self The application instance.
 * @param filter The device filter specification or NULL.
 * @param flags The APP_MATCH_* bitmask.  Use APP_MATCH_EXPLICIT for
 *      destructive operations, so they never act on the first device found.
 * @return 0 or error code.
 *
 * Same as app_match() when flags is 0.
 */
int32_t app_match_ex(struct app_s * self, const char * filter, uint32_t flags);

/**
 * @brief Refuse a target filter that matches the power device.
 *
 * @param power_device The matched power device path.
 * @param target_filter The target device filter.
 * @return 0 when the target filter does not match the power device, 1 otherwise.
 */
int32_t app_power_target_check(const char * power_device, const char * target_filter);

typedef int (*command_fn)(struct app_s * self, int argc, char * argv[]);

int on_adapter(struct app_s * self, int argc, char * argv[]);
int on_cal(struct app_s * self, int argc, char * argv[]);
int on_firmware(struct app_s * self, int argc, char * argv[]);
int on_fwup(struct app_s * self, int argc, char * argv[]);
int on_force_remove(struct app_s * self, int argc, char * argv[]);
int on_fpga_mem(struct app_s * self, int argc, char * argv[]);
int on_fuzz_fwup(struct app_s * self, int argc, char * argv[]);
int on_help(struct app_s * self, int argc, char * argv[]);
int on_hotplug(struct app_s * self, int argc, char * argv[]);
int on_info(struct app_s * self, int argc, char * argv[]);
int on_loopback(struct app_s * self, int argc, char * argv[]);
int on_mem(struct app_s * self, int argc, char * argv[]);
int on_stream(struct app_s * self, int argc, char * argv[]);
int on_stream_test(struct app_s * self, int argc, char * argv[]);
int on_suspend_test(struct app_s * self, int argc, char * argv[]);
int on_throughput(struct app_s * self, int argc, char * argv[]);
int on_timesync(struct app_s * self, int argc, char * argv[]);
int on_publish_cmd(struct app_s * self, int argc, char * argv[]);
int on_pubsub_info(struct app_s * self, int argc, char * argv[]);
int on_state_get(struct app_s * self, int argc, char * argv[]);
int on_pubsub_sniffer(struct app_s * self, int argc, char * argv[]);
int on_pubsub_test(struct app_s * self, int argc, char * argv[]);
int on_power_cycle(struct app_s * self, int argc, char * argv[]);
int on_version(struct app_s * self, int argc, char * argv[]);

MB_CPP_GUARD_END

#endif  /* MB_EXAMPLE_MINIBITTY_EXE_PRV_H__ */
