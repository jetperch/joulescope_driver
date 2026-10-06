/*
 * SPDX-FileCopyrightText: Copyright 2025 Jetperch LLC
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

#include "adapter.h"
#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/log.h"
#include "jsdrv/os_thread.h"
#include "adapter_tracy.h"
#include "mbgen.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define COUNTER_FMT "%10" PRIu32 " "


const char * MB_OBJ_NAME[16] = {
    "isr",
    "context",
    "task",
    "timer",
    "msg",
    "rsv",
    "heap",
    "trace",
    "fsm",
    "unknown_9",
    "unknown_A",
    "unknown_B",
    "unknown_C",
    "unknown_D",
    "unknown_E",
    "unknown_F",
};

static int usage(void) {
    printf(
        "usage: minibitty adapter [options] [device_filter]\n"
        "options:\n"
        "  --print <filter>  Print to console.  The filter is a comma-separated\n"
        "                    list of terms.  Each term is a class with an\n"
        "                    optional \":\" selector:\n"
        "                      all (or *)   everything\n"
        "                      task         all tasks | task:usbd | task:2\n"
        "                      isr          all ISRs  | isr:TIM* | isr:54\n"
        "                      log          all logs  | log:W (warning and worse)\n"
        "                      timer, msg, heap, ...  other object classes\n"
        "                    Prefix a term with \"-\" to exclude, applied after\n"
        "                    includes, like \"all,-isr:TIM*\".  Name selectors\n"
        "                    require --mbgen; numeric ids always work.\n"
        "  --tracy           Send to Tracy\n"
        "  --mbgen <path>    Load the target's mbgen.bin build metadata to\n"
        "                    resolve task, ISR, file, and log string names\n"
        "  --verbose-name    Show both name and id, like \"usbd (task.2)\"\n"
        "examples:\n"
        "  minibitty adapter --print all u/mb\n"
        "  minibitty adapter --mbgen mbgen.bin --print task:usbd,log:W u/mb\n"
    );
    return 1;
}

#define FILTER_CLASS_ANY  (-1)
#define FILTER_CLASS_LOG  (-2)
#define FILTER_TERMS_MAX  (32)
#define FILTER_LEVEL_ALL  (127)

struct filter_term_s {
    bool exclude;
    int8_t obj_class;       // FILTER_CLASS_ANY, FILTER_CLASS_LOG, or MB_OBJ_*
    bool has_id;
    uint32_t id;
    char name_glob[32];     // empty = whole class
    int8_t log_level;       // severity threshold, FILTER_LEVEL_ALL = all logs
};

struct filter_s {
    struct filter_term_s terms[FILTER_TERMS_MAX];
    uint32_t count;
    bool has_include;
};

struct adapter_ctx_s {
    struct mbgen_s mbgen;
    bool mbgen_loaded;
    bool verbose_name;
    struct filter_s filter;
};

static bool strieq(const char * a, const char * b) {
    while (*a && *b) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == 0) && (*b == 0);
}

static bool glob_match(const char * pattern, const char * s) {
    while (*pattern) {
        if (*pattern == '*') {
            ++pattern;
            if (0 == *pattern) {
                return true;
            }
            for (; *s; ++s) {
                if (glob_match(pattern, s)) {
                    return true;
                }
            }
            return glob_match(pattern, s);
        }
        if ((0 == *s) || (tolower((unsigned char) *pattern) != tolower((unsigned char) *s))) {
            return false;
        }
        ++pattern;
        ++s;
    }
    return 0 == *s;
}

static int log_level_parse(const char * s) {
    static const struct { const char * name; int8_t level; } LEVELS[] = {
        {"!", 0}, {"emergency", 0},
        {"a", 1}, {"alert", 1},
        {"c", 2}, {"critical", 2},
        {"e", 3}, {"error", 3},
        {"w", 4}, {"warning", 4}, {"warn", 4},
        {"n", 5}, {"notice", 5},
        {"i", 6}, {"info", 6},
        {"d", 7}, {"debug", 7}, {"debug1", 7},
        {"debug2", 8},
        {"debug3", 9},
    };
    for (size_t i = 0; i < (sizeof(LEVELS) / sizeof(LEVELS[0])); ++i) {
        if (strieq(s, LEVELS[i].name)) {
            return LEVELS[i].level;
        }
    }
    if (isdigit((unsigned char) s[0]) && (0 == s[1])) {
        return s[0] - '0';
    }
    return -1;
}

static int filter_class_parse(const char * s) {
    if (strieq(s, "all") || (0 == strcmp(s, "*"))) {
        return FILTER_CLASS_ANY;
    }
    if (strieq(s, "log")) {
        return FILTER_CLASS_LOG;
    }
    for (int i = 0; i < 16; ++i) {
        if (strieq(s, MB_OBJ_NAME[i])) {
            return i;
        }
    }
    return -128;
}

static int filter_parse(struct adapter_ctx_s * ctx, const char * filter_str) {
    struct filter_s * self = &ctx->filter;
    char term_buf[64];
    const char * p = filter_str;
    while (*p) {
        const char * end = strchr(p, ',');
        size_t term_len = (NULL == end) ? strlen(p) : (size_t) (end - p);
        if (term_len >= sizeof(term_buf)) {
            printf("ERROR: filter term too long\n");
            return 1;
        }
        if (0 == term_len) {
            p += 1;
            continue;
        }
        memcpy(term_buf, p, term_len);
        term_buf[term_len] = 0;
        p += term_len + ((NULL == end) ? 0 : 1);

        if (self->count >= FILTER_TERMS_MAX) {
            printf("ERROR: too many filter terms\n");
            return 1;
        }
        struct filter_term_s * term = &self->terms[self->count];
        memset(term, 0, sizeof(*term));
        term->log_level = FILTER_LEVEL_ALL;
        char * t = term_buf;
        if (*t == '-') {
            term->exclude = true;
            ++t;
        }
        char * selector = strchr(t, ':');
        if (NULL != selector) {
            *selector++ = 0;
        }
        int obj_class = filter_class_parse(t);
        if (-128 == obj_class) {
            printf("ERROR: unknown filter class \"%s\"\n", t);
            return 1;
        }
        term->obj_class = (int8_t) obj_class;
        if (NULL != selector) {
            if (FILTER_CLASS_ANY == obj_class) {
                printf("ERROR: \"all\" does not take a selector\n");
                return 1;
            } else if (FILTER_CLASS_LOG == obj_class) {
                int level = log_level_parse(selector);
                if (level < 0) {
                    printf("ERROR: invalid log level \"%s\"\n", selector);
                    return 1;
                }
                term->log_level = (int8_t) level;
            } else if (isdigit((unsigned char) selector[0])) {
                term->has_id = true;
                term->id = (uint32_t) strtoul(selector, NULL, 0);
            } else {
                if (!ctx->mbgen_loaded) {
                    printf("ERROR: filter name \"%s:%s\" requires --mbgen\n", t, selector);
                    return 1;
                }
                if (strlen(selector) >= sizeof(term->name_glob)) {
                    printf("ERROR: filter name too long \"%s\"\n", selector);
                    return 1;
                }
                strcpy(term->name_glob, selector);
            }
        }
        if (!term->exclude) {
            self->has_include = true;
        }
        ++self->count;
    }
    if (0 == self->count) {
        printf("ERROR: empty filter\n");
        return 1;
    }
    return 0;
}

static bool filter_term_match_obj(struct adapter_ctx_s * ctx, const struct filter_term_s * term,
                                  uint32_t obj_type, uint32_t obj_id) {
    if (FILTER_CLASS_ANY == term->obj_class) {
        return true;
    }
    if (term->obj_class != (int8_t) obj_type) {
        return false;
    }
    if (term->has_id) {
        return term->id == obj_id;
    }
    if (term->name_glob[0]) {
        const char * name = NULL;
        if (obj_type == MB_OBJ_TASK) {
            name = mbgen_task_name(&ctx->mbgen, obj_id);
        } else if (obj_type == MB_OBJ_ISR) {
            name = mbgen_isr_name(&ctx->mbgen, obj_id);
        }
        return (NULL != name) && glob_match(term->name_glob, name);
    }
    return true;
}

static bool filter_term_match_log(const struct filter_term_s * term, uint32_t level) {
    if (FILTER_CLASS_ANY == term->obj_class) {
        return true;
    }
    if (FILTER_CLASS_LOG != term->obj_class) {
        return false;
    }
    return level <= (uint32_t) term->log_level;
}

/**
 * @brief Evaluate the filter for a trace record.
 *
 * @param ctx The adapter context.
 * @param is_log True for log records; obj_id is then the log level.
 * @param obj_type The object class for non-log records.
 * @param obj_id The object id, or the log level for log records.
 * @return True to display the record.
 */
static bool filter_match(struct adapter_ctx_s * ctx, bool is_log,
                         uint32_t obj_type, uint32_t obj_id) {
    struct filter_s * self = &ctx->filter;
    bool include = !self->has_include;  // only excludes: include everything
    for (uint32_t i = 0; i < self->count; ++i) {
        const struct filter_term_s * term = &self->terms[i];
        bool match = is_log
                ? filter_term_match_log(term, obj_id)
                : filter_term_match_obj(ctx, term, obj_type, obj_id);
        if (match) {
            if (term->exclude) {
                return false;
            }
            include = true;
        }
    }
    return include;
}

/**
 * @brief Format the display name for a trace object.
 *
 * Without metadata: "task.2".  With metadata: "task  usbd", or with
 * verbose_name "task  usbd (task.2)".  Falls back to the raw form with the
 * class prefix when the id is not in the metadata.
 */
static void obj_display(struct adapter_ctx_s * ctx, char * buf, size_t buf_size,
                        uint32_t obj_type, uint32_t obj_id) {
    const char * cls = MB_OBJ_NAME[obj_type & 0xf];
    if ((NULL == ctx) || (!ctx->mbgen_loaded)) {
        snprintf(buf, buf_size, "%s.%u", cls, obj_id);
        return;
    }
    const char * name = NULL;
    if (obj_type == MB_OBJ_TASK) {
        name = mbgen_task_name(&ctx->mbgen, obj_id);
    } else if (obj_type == MB_OBJ_ISR) {
        name = mbgen_isr_name(&ctx->mbgen, obj_id);
    }
    if (NULL == name) {
        snprintf(buf, buf_size, "%-5s %s.%u", cls, cls, obj_id);
    } else if (ctx->verbose_name) {
        snprintf(buf, buf_size, "%-5s %s (%s.%u)", cls, name, cls, obj_id);
    } else {
        snprintf(buf, buf_size, "%-5s %s", cls, name);
    }
}

/**
 * @brief Format the display for a source location.
 *
 * With metadata: ":/src/tasks/led.c:143".  Without: "44.137".
 */
static void src_display(struct adapter_ctx_s * ctx, char * buf, size_t buf_size,
                        uint32_t file_id, uint32_t line) {
    const char * file = NULL;
    if ((NULL != ctx) && ctx->mbgen_loaded) {
        file = mbgen_file_name(&ctx->mbgen, file_id);
    }
    if (NULL == file) {
        snprintf(buf, buf_size, "%u.%u", file_id, line);
    } else {
        snprintf(buf, buf_size, "%s:%u", file, line);
    }
}

static void on_trace_print(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct adapter_ctx_s * ctx = (struct adapter_ctx_s *) user_data;
    char obj_buf[64];
    char src_buf[192];
    char log_buf[256];
    (void) topic;
    if (value->type != JSDRV_UNION_BIN) {
        JSDRV_LOGW("trace: invalid type %d", value->type);
        return;
    }
    const uint8_t * p8 = value->value.bin;
    const uint32_t * p32 = (const uint32_t *) p8;
    const uint32_t * p32_end = p32 + ((value->size + 3) >> 2);
    while (p32 < p32_end) {
        if (MB_TRACE_SOF != (p32[0] & 0xff)) {
            JSDRV_LOGW("trace: invalid SOF");
            while (MB_TRACE_SOF != (p32[0] & 0xff)) {
                ++p32;
                if (p32 >= p32_end) {
                    return;
                }
            }
        }
        uint8_t length = (p32[0] >> 8) & 0x0f;
        uint8_t type = (p32[0] >> 12) & 0x0f;
        uint16_t metadata = p32[0] >> 16;
        uint32_t obj_type = (metadata >> 12) & 0x000f;
        uint32_t obj_id = metadata & 0x0fff;
        uint32_t counter = p32[1];
        p32 += 2;
        uint32_t file_id = 0;
        uint32_t line = 0;
        if (length) {
            file_id = (p32[0] >> 16) & 0x0000ffff;
            line = p32[0] & 0x0000ffff;
        }

        bool show = true;
        if (NULL != ctx) {
            switch (type) {
                case MB_TRACE_TYPE_READY:
                case MB_TRACE_TYPE_ENTER:
                case MB_TRACE_TYPE_EXIT:
                case MB_TRACE_TYPE_ALLOC:
                case MB_TRACE_TYPE_FREE:
                    show = filter_match(ctx, false, obj_type, obj_id);
                    break;
                case MB_TRACE_TYPE_LOG:
                    show = filter_match(ctx, true, 0, metadata);
                    break;
                default:
                    break;
            }
        }
        if (!show) {
            p32 += length;
            continue;
        }

        switch (type) {
            case MB_TRACE_TYPE_INVALID:
                JSDRV_LOGW("trace type invalid");
                break;
            case MB_TRACE_TYPE_READY:
                obj_display(ctx, obj_buf, sizeof(obj_buf), obj_type, obj_id);
                printf(COUNTER_FMT "%s ready\n", counter, obj_buf);
                break;
            case MB_TRACE_TYPE_ENTER:
                obj_display(ctx, obj_buf, sizeof(obj_buf), obj_type, obj_id);
                printf(COUNTER_FMT "%s enter\n", counter, obj_buf);
                break;
            case MB_TRACE_TYPE_EXIT:
                obj_display(ctx, obj_buf, sizeof(obj_buf), obj_type, obj_id);
                if (length == 0) {
                    printf(COUNTER_FMT "%s exit\n", counter, obj_buf);
                } else if (length == 1) {
                    printf(COUNTER_FMT "%s exit %" PRIu32 "\n", counter, obj_buf, p32[0]);
                } else {
                    JSDRV_LOGW("exit length invalid");
                }
                break;
            case MB_TRACE_TYPE_ALLOC:
                obj_display(ctx, obj_buf, sizeof(obj_buf), obj_type, obj_id);
                src_display(ctx, src_buf, sizeof(src_buf), file_id, line);
                printf(COUNTER_FMT "%s alloc @ %s\n", counter, obj_buf, src_buf);
                break;
            case MB_TRACE_TYPE_FREE:
                obj_display(ctx, obj_buf, sizeof(obj_buf), obj_type, obj_id);
                src_display(ctx, src_buf, sizeof(src_buf), file_id, line);
                printf(COUNTER_FMT "%s free @ %s\n", counter, obj_buf, src_buf);
                break;
            case MB_TRACE_TYPE_RSV6: break;
            case MB_TRACE_TYPE_RSV7: break;
            case MB_TRACE_TYPE_TIMESYNC: break;
            case MB_TRACE_TYPE_TIMEMAP: break;
            case MB_TRACE_TYPE_FAULT: break;
            case MB_TRACE_TYPE_VALUE: break;
            case MB_TRACE_TYPE_LOG:
                if ((NULL != ctx) && ctx->mbgen_loaded && (length >= 1)) {
                    mbgen_log_line(&ctx->mbgen, log_buf, sizeof(log_buf),
                                   metadata, file_id, line, &p32[1], length - 1);
                    printf(COUNTER_FMT "%s\n", counter, log_buf);
                } else {
                    switch (length) {
                        case 2: printf(COUNTER_FMT "LOG @ %d.%d 0x%08x\n", counter, file_id, line, p32[1]); break;
                        case 3: printf(COUNTER_FMT "LOG @ %d.%d 0x%08x 0x%08x\n", counter, file_id, line, p32[1], p32[2]); break;
                        case 4: printf(COUNTER_FMT "LOG @ %d.%d 0x%08x 0x%08x 0x%08x\n", counter, file_id, line, p32[1], p32[2], p32[3]); break;
                        case 5: printf(COUNTER_FMT "LOG @ %d.%d 0x%08x 0x%08x 0x%08x 0x%08x\n", counter, file_id, line, p32[1], p32[2], p32[3], p32[4]); break;
                        default: printf(COUNTER_FMT "LOG @ %d.%d\n", counter, file_id, line); break;
                    }
                }
                break;
            case MB_TRACE_TYPE_RSV13: break;
            case MB_TRACE_TYPE_RSV14: break;
            case MB_TRACE_TYPE_OVERFLOW:
                printf(COUNTER_FMT "OVERFLOW %d\n", counter, metadata);
                break;
        }
        p32 += length;
    }
}


int on_adapter(struct app_s * self, int argc, char * argv[]) {
    bool tracy = false;
    struct jsdrv_topic_s topic;
    char *device_filter = NULL;
    char *mbgen_path = NULL;
    char *print_filter = NULL;
    struct adapter_ctx_s ctx;
    memset(&ctx, 0, sizeof(ctx));

    while (argc) {
        if (argv[0][0] != '-') {
            if (NULL != device_filter) {
                printf("Duplicate device_filter\n");
                return usage();
            }
            device_filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--print")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            print_filter = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--tracy")) {
            tracy = true;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--mbgen")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            mbgen_path = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--verbose-name")) {
            ctx.verbose_name = true;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (NULL == device_filter) {
        printf("device_filter required\n");
        return usage();
    }

    if (NULL != mbgen_path) {
        if (mbgen_load(&ctx.mbgen, mbgen_path)) {
            return 1;
        }
        ctx.mbgen_loaded = true;
    }

    if (NULL != print_filter) {
        if (filter_parse(&ctx, print_filter)) {
            mbgen_finalize(&ctx.mbgen);
            return usage();
        }
    }

    ROE(app_match(self, device_filter));

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/!trace");
    if (NULL != print_filter) {
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_trace_print, &ctx, 0);
    }

    struct adapter_tracy_s * tracy_ = NULL;
    if (tracy) {
        tracy_ = adapter_tracy_initialize(self->context,
                                          ctx.mbgen_loaded ? &ctx.mbgen : NULL,
                                          ctx.verbose_name);
        jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, adapter_tracy_on_trace, tracy_, 0);
    }

    while (!quit_) {
        jsdrv_thread_sleep_ms(10);
    }

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);

    if (tracy) {
        adapter_tracy_finalize(tracy_);
        tracy_ = NULL;
    }

    if (ctx.mbgen_loaded) {
        mbgen_finalize(&ctx.mbgen);
    }

    return 0;
}
