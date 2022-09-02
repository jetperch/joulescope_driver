/*
 * Copyright 2014-2022 Jetperch LLC
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
 * @brief Joulescope driver test utility
 */

#include "jsdrv_util/jsdrv_util_prv.h"
#include "jsdrv/log.h"
#include "jsdrv/cstr.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv/version.h"
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>


struct command_s {
    const char * command;
    command_fn fn;
    const char * description;
};

struct app_s app_;

void on_log_recv(void * user_data, struct jsdrv_log_header_s const * header,
                 const char * filename, const char * message) {
    (void) user_data;
    char time_str[64];
    struct tm tm_utc;
    time_t time_s = (time_t) (header->timestamp / JSDRV_TIME_SECOND);
    int time_us = (int) ((header->timestamp - time_s * JSDRV_TIME_SECOND) / JSDRV_TIME_MICROSECOND);
    time_s += JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS;
#if _WIN32
    _gmtime64_s(&tm_utc, &time_s);  // windows only
#else
    gmtime_r(&time_s, &tm_utc);  // posix https://en.cppreference.com/w/c/chrono/gmtime
#endif
    strftime(time_str, sizeof(time_str), "%FT%T", &tm_utc);
    printf("%s.%06dZ %c %s:%d %s\n", time_str, time_us,
           jsdrv_log_level_to_char(header->level), filename, header->line, message);
}

int app_initialize(struct app_s * self) {
    memset(self, 0, sizeof(*self));
    jsdrv_topic_clear(&self->topic);
    ROE(jsdrv_initialize(&self->context, NULL, 1000));
    return 0;
}

static void app_finalize(struct app_s * self) {
    if (self->context) {
        jsdrv_finalize(self->context, 1000);
        self->context = NULL;
    }
}

int32_t app_scan(struct app_s * self) {
    struct jsdrv_union_s devices_value = jsdrv_union_str(self->devices);
    devices_value.size = sizeof(self->devices);
    return jsdrv_query(self->context, JSDRV_MSG_DEVICE_LIST, &devices_value, 0);
}

int32_t app_match(struct app_s * self, const char * filter) {
    jsdrv_topic_clear(&self->device);
    ROE(app_scan(self));
    if (0 == self->devices[0]) {
        printf("No devices found\n");
        return 1;
    }

    char * d = &self->devices[0];
    size_t sz = strlen(d);
    for (size_t i = 0; i <= sz; ++i) {
        if (self->devices[i] == ',') {
            self->devices[i] = 0;
        }
        if (self->devices[i] == 0) {
            if ((NULL == filter) || (jsdrv_cstr_starts_with(d, filter))) {
                jsdrv_topic_set(&self->device, d);
                return 0;
            }
        }
    }

    printf("No matching device found\n");
    return 1;
}

const struct command_s COMMANDS[] = {
        {"demo", on_demo, "Demonstrate streaming"},
        {"dev",  on_dev,  "Developer tools"},
        {"info", on_info, "List connected devices and display device details"},
        {"mem_erase", on_mem_erase, "Erase memory region"},
        {"mem_read", on_mem_read, "Read memory region"},
        {"mem_write", on_mem_write, "Write memory region"},
        {"reset", on_reset, "Reset to target"},
        {"scan", on_scan, "List connected devices"},
        {"set",  on_set,  "Set parameters"},
        {"statistics",  on_statistics,  "Display statistics from all connected devices"},
        {"version", on_version, "Display version and platform information"},
        {"help", on_help, "Display help"},
        {NULL, NULL, NULL}
};

static int usage() {
    const struct command_s * cmd = COMMANDS;
    printf("usage: jsdrv_util [--log-level <LEVEL>] <COMMAND> [...args]\n");
    printf("\n--log_level: Configure the log level to stdout\n"
           "    off, emergency, alert, critical, [error], warning,\n"
           "    notice, info, debug1, debug2, debug3, all\n");
    printf("\nAvailable commands:\n");
    while (cmd->command) {
        printf("  %-12s %s\n", cmd->command, cmd->description);
        ++cmd;
    }
    return 1;
}

int on_help(struct app_s * self, int argc, char * argv[]) {
    usage();
    return 0;
}

struct log_level_convert_s {
    const char * str;
    int8_t level;
};

const struct log_level_convert_s LOG_LEVEL_CONVERT[] = {
    {"off", JSDRV_LOG_LEVEL_OFF},
    {"emergency", JSDRV_LOG_LEVEL_EMERGENCY},
    {"e", JSDRV_LOG_LEVEL_EMERGENCY},
    {"alert", JSDRV_LOG_LEVEL_ALERT},
    {"a", JSDRV_LOG_LEVEL_ALERT},
    {"critical", JSDRV_LOG_LEVEL_CRITICAL},
    {"c", JSDRV_LOG_LEVEL_CRITICAL},
    {"error", JSDRV_LOG_LEVEL_ERROR},
    {"e", JSDRV_LOG_LEVEL_ERROR},
    {"warn", JSDRV_LOG_LEVEL_WARNING},
    {"warning", JSDRV_LOG_LEVEL_WARNING},
    {"w", JSDRV_LOG_LEVEL_WARNING},
    {"notice", JSDRV_LOG_LEVEL_NOTICE},
    {"n", JSDRV_LOG_LEVEL_NOTICE},
    {"info", JSDRV_LOG_LEVEL_INFO},
    {"i", JSDRV_LOG_LEVEL_INFO},
    {"debug1", JSDRV_LOG_LEVEL_DEBUG1},
    {"d", JSDRV_LOG_LEVEL_DEBUG1},
    {"debug", JSDRV_LOG_LEVEL_DEBUG1},
    {"debug2", JSDRV_LOG_LEVEL_DEBUG2},
    {"debug3", JSDRV_LOG_LEVEL_DEBUG3},
    {"all", JSDRV_LOG_LEVEL_ALL},
    {NULL, 0},
};

static int log_level_cvt(const char * level_str, int8_t * level_i8) {
    *level_i8 = JSDRV_LOG_LEVEL_ERROR;
    for (const struct log_level_convert_s * cvt = LOG_LEVEL_CONVERT; cvt->str; ++cvt) {
        if (jsdrv_cstr_casecmp(cvt->str, level_str) == 0) {
            *level_i8 = cvt->level;
            return 0;
        }
    }
    return 1;
}

int main(int argc, char * argv[]) {
    struct app_s * self = &app_;
    memset(self, 0, sizeof(*self));
    int32_t rc;

    if (argc < 2) {
        return usage();
    }
    ARG_CONSUME();

    jsdrv_log_initialize();
    jsdrv_log_register(on_log_recv, NULL);
    if ((jsdrv_cstr_casecmp("--log-level", argv[0]) == 0) || (jsdrv_cstr_casecmp("--log_level", argv[0]) == 0)) {
        ARG_CONSUME();
        int8_t level = JSDRV_LOG_LEVEL_ERROR;
        if (log_level_cvt(argv[0], &level)) {
            printf("Invalid log level: %s\n", argv[0]);
            return usage();
        }
        ARG_CONSUME();
        jsdrv_log_level_set(level);
    }

    ROE(app_initialize(self));

    char * command_str = argv[0];
    ARG_CONSUME();

    rc = 9999;
    const struct command_s * cmd = COMMANDS;
    while (cmd->command) {
        if (strcmp(cmd->command, command_str) == 0) {
            rc = cmd->fn(self, argc, argv);
            break;
        }
        ++cmd;
    }
    if (rc == 9999) {
        rc = usage();
    }

    app_finalize(self);
    jsdrv_log_finalize();
    if (rc) {
        printf("### ERROR return code %d ###\n", rc);
    }
    return rc;
}