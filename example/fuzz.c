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

/**
 * @file
 *
 * @brief Joulescope driver fuzz tester.
 */

#include "jsdrv/error_code.h"
#include "jsdrv/log.h"
#include "jsdrv/cstr.h"
#include "jsdrv/time.h"
#include "jsdrv/topic.h"
#include "jsdrv/version.h"
#include "jsdrv_prv/thread.h"
#include <inttypes.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>

#if _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <unistd.h>
#include <pthread.h>
#endif

#define ARG_CONSUME() --argc; ++argv
#define ARG_REQUIRE()  if (argc <= 0) {return usage();}
#define ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )
#define ROE(x) do {         \
    int rc__ = (x);         \
    if (rc__) {             \
        return rc__;        \
    }                       \
} while (0)

#define ST_INITIALIZED  "initialized"
#define ST_FINALIZED    "finalized"
#define ST_OPEN         "open"
#define ST_STREAM       "stream"
#define ST_BUFFER       "buffer"

volatile int32_t quit_ = 0;
uint64_t random_seed = 1;
static const uint64_t random_multiplier = (2654435761ULL | (2654435761ULL << 32));

struct app_s; // forward declaration


typedef int32_t (*transition_fn)(struct app_s * self);


struct transition_s {
    uint32_t weight;
    const char * name;
    transition_fn fn;
};

#define TRANSITION_END {0, NULL, NULL}


struct state_s {
    const char * name;
    struct transition_s transitions[100];  // maximum
};

struct app_s {
    struct jsdrv_context_s * context;
    const char * state_name;
    struct state_s * states;
    int32_t nice_level;
    char device_prefix[256];
    char device_filter[256];  // if non-empty, restrict t_open to this prefix
    struct jsdrv_buffer_info_s buffer_info[16];
};

static uint32_t random_u32(void) {
    random_seed *= random_multiplier;
    random_seed >>= 1;
    return (uint32_t) random_seed;
}

static uint32_t random_range_u32(uint32_t min_inc, uint32_t max_exc) {
    return (random_u32() % (max_exc - min_inc)) + min_inc;
}

// cross-platform handler for CTRL-C to exit program
static void signal_handler(int signal){
    if ((signal == SIGABRT) || (signal == SIGINT)) {
        quit_ = 1;
    }
}

#if _WIN32
// Walk the crashing thread's stack using DbgHelp and print resolved
// frames to stderr.  Called from the top-level SEH filter so we have
// the CONTEXT at the fault, not at the handler entry.
static void print_stack(CONTEXT * ctx_in) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);

    CONTEXT ctx = *ctx_in;
    STACKFRAME64 frame = {0};
    DWORD machine;
#if defined(_M_AMD64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;  frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;  frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;  frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;   frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Fp;   frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Sp;   frame.AddrStack.Mode = AddrModeFlat;
#else
    fprintf(stderr, "  (stack walk: unsupported architecture)\n");
    return;
#endif

    uint8_t sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO * sym = (SYMBOL_INFO *) sym_buf;
    sym->MaxNameLen = 255;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, process, thread, &frame, &ctx, NULL,
                          SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        DWORD64 addr = frame.AddrPC.Offset;
        DWORD64 disp = 0;
        if (SymFromAddr(process, addr, &disp, sym)) {
            DWORD line_disp = 0;
            if (SymGetLineFromAddr64(process, addr, &line_disp, &line)) {
                fprintf(stderr, "  #%02d %s+0x%llx  %s:%lu\n",
                        i, sym->Name, (unsigned long long) disp,
                        line.FileName, (unsigned long) line.LineNumber);
            } else {
                fprintf(stderr, "  #%02d %s+0x%llx\n",
                        i, sym->Name, (unsigned long long) disp);
            }
        } else {
            fprintf(stderr, "  #%02d 0x%llx (no symbol)\n",
                    i, (unsigned long long) addr);
        }
    }
    fflush(stderr);
}

static const char * seh_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
        case 0xE06D7363:                         return "C++ exception";
        default:                                 return "unknown";
    }
}

static LONG WINAPI fuzz_unhandled_exception(EXCEPTION_POINTERS * ep) {
    EXCEPTION_RECORD * r = ep->ExceptionRecord;
    fprintf(stderr, "\n### UNHANDLED EXCEPTION 0x%08lx (%s) at %p, thread=%lu\n",
            (unsigned long) r->ExceptionCode, seh_name(r->ExceptionCode),
            r->ExceptionAddress, GetCurrentThreadId());
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) && (r->NumberParameters >= 2)) {
        fprintf(stderr, "  %s 0x%llx\n",
                r->ExceptionInformation[0] ? "write to" : "read from",
                (unsigned long long) r->ExceptionInformation[1]);
    }
    print_stack(ep->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate
}

// Last-resort CRT handler for abort() / pure-virtual / etc.  These
// bypass the SEH filter on MSVC's CRT, so we hook them separately.
static void crt_invalid_parameter(const wchar_t * expr, const wchar_t * func,
                                  const wchar_t * file, unsigned int line, uintptr_t rsvd) {
    (void) rsvd;
    fprintf(stderr, "\n### CRT invalid parameter: %ls at %ls %ls:%u\n",
            expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    print_stack(&ctx);
}

static void crt_purecall(void) {
    fprintf(stderr, "\n### CRT pure virtual call\n");
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    print_stack(&ctx);
}

static void crash_handler_install(void) {
    SetUnhandledExceptionFilter(fuzz_unhandled_exception);
    _set_invalid_parameter_handler(crt_invalid_parameter);
    _set_purecall_handler(crt_purecall);
    // Route CRT asserts to stderr rather than a modal dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#else
// POSIX (Linux + macOS) crash handler.  Uses sigaction + execinfo's
// backtrace() to dump a stack of the crashing thread, then re-raises
// the signal with SIG_DFL so a core dump is still produced.  Frames
// include module+offset and, on Linux builds with -rdynamic (or dynsym
// exports) / macOS normally, resolved symbol names.  Run the resulting
// output through `addr2line -e ./cmake-build/example/fuzz -f` for
// source:line when symbols are not inlined.
#include <string.h>

static const char * posix_signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default:      return "SIG?";
    }
}

static void posix_crash_handler(int sig, siginfo_t * info, void * ctx) {
    (void) ctx;
    // async-signal-safe: use write() / dprintf on STDERR_FILENO.
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
            "\n### SIGNAL %d (%s) at addr %p, thread=%p, code=%d\n",
            sig, posix_signal_name(sig),
            info ? info->si_addr : NULL,
            (void *) (uintptr_t) pthread_self(),
            info ? info->si_code : 0);
    if (n > 0) {
        ssize_t wrote = write(STDERR_FILENO, buf, (size_t) n);
        (void) wrote;
    }
    void * frames[64];
    int nframes = backtrace(frames, (int) (sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
    // Restore default handler and re-raise so a core dump is produced
    // and the exit status reflects the signal.
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

static void crash_handler_install(void) {
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posix_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        sigaction(sigs[i], &sa, NULL);
    }
}
#endif

static void state_update(struct app_s * self, const char * name) {
    self->state_name = name;
}

static int32_t t_initialize(struct app_s * self) {
    if (NULL != self->context) {
        jsdrv_finalize(self->context, 0);
    }
    int32_t rc = jsdrv_initialize(&self->context, NULL, 0);
    if (0 == rc) {
        state_update(self, ST_INITIALIZED);
    }
    return rc;
}

static int32_t t_finalize(struct app_s * self) {
    jsdrv_finalize(self->context, 0);
    self->context = NULL;
    state_update(self, ST_FINALIZED);
    return 0;
}

static int32_t t_open(struct app_s * self) {
    uint32_t device_count = 0;
    char devices_str[4096];
    char * p = devices_str;
    char * devices[64];
    struct jsdrv_union_s devices_value = jsdrv_union_str(devices_str);
    devices_value.size = sizeof(devices_str);
    int32_t rc = jsdrv_query(self->context, JSDRV_MSG_DEVICE_LIST, &devices_value, 0);
    if (rc) {
        printf("ERROR: could not query device list\n");
        return rc;
    }

    devices[0] = p;
    while (1) {
        if (*p == ',') {
            *p++ = 0;
            devices[++device_count] = p;
        } else if (*p == 0) {
            ++device_count;
            break;
        } else {
            ++p;
        }
    }
    if (device_count == 0) {
        printf("ERROR : open failed, no devices: %s\n", devices_str);
        return 1;
    }
    if (self->device_filter[0]) {
        bool found = false;
        for (uint32_t i = 0; i < device_count; ++i) {
            if (0 == strcmp(devices[i], self->device_filter)) {
                found = true;
                break;
            }
        }
        if (!found) {
            printf("ERROR: filter device %s not in list: %s\n",
                   self->device_filter, devices_str);
            return 1;
        }
        snprintf(self->device_prefix, sizeof(self->device_prefix), "%s", self->device_filter);
    } else {
        uint32_t device_idx = random_range_u32(0, device_count);
        snprintf(self->device_prefix, sizeof(self->device_prefix), "%s", devices[device_idx]);
    }
    printf("open %s\n", self->device_prefix);
    rc = jsdrv_open(self->context, self->device_prefix, 0, 0);
    if (0 == rc) {
        state_update(self, ST_OPEN);
    }
    return rc;
}

static int32_t t_close(struct app_s * self) {
    int32_t rc = 0;
    if (self->device_prefix[0]) {
        printf("close %s\n", self->device_prefix);
        rc = jsdrv_close(self->context, self->device_prefix, 0);
    }
    self->device_prefix[0] = 0;
    state_update(self, ST_INITIALIZED);
    return rc;
}

static int32_t t_wait(struct app_s * self) {
    (void) self;
    uint32_t duration_ms = random_range_u32(10, 250);
    jsdrv_thread_sleep_ms(duration_ms);
    return 0;
}

static int32_t publish(struct app_s * self, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    char buf[128];
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("Publish %s => %s\n", topic, buf);
    int32_t rc = jsdrv_publish(self->context, topic, value, timeout_ms);
    if (rc) {
        printf("publish %s failed with %d\n", topic, (int) rc);
    }
    return rc;
}

static int32_t publish_device(struct app_s * self, const char * topic, const struct jsdrv_union_s * value, uint32_t timeout_ms) {
    struct jsdrv_topic_s t;
    if (0 == self->device_prefix[0]) {
        printf("ERROR: device_publish but no active device\n");
        return 1;
    }
    jsdrv_topic_set(&t, self->device_prefix);
    jsdrv_topic_append(&t, topic);
    return publish(self, t.topic, value, timeout_ms);
}

static int32_t t_sample_rate(struct app_s * self) {
    (void) self;
    const uint32_t sample_rates[] = {
            1000000,
            500000, 200000, 100000,
            50000, 20000, 10000,
            5000, 2000, 1000,
            500, 200, 100,
            50, 20, 10,
            5, 2, 1,
    };
    uint32_t sample_rate_idx = random_range_u32(0, ARRAY_SIZE(sample_rates));
    uint32_t sample_rate = sample_rates[sample_rate_idx];
    // Not every device supports every rate (e.g. JS320 rejects <1 kHz).
    // A parameter rejection is an expected fuzz outcome, not a failure.
    int32_t rc = publish_device(self, "h/fs", &jsdrv_union_u32_r(sample_rate),
                                JSDRV_TIMEOUT_MS_DEFAULT);
    if ((rc != 0) && (rc != JSDRV_ERROR_PARAMETER_INVALID)) {
        return rc;
    }
    return 0;
}

static int32_t t_stream_start(struct app_s * self) {
    ROE(publish_device(self, "s/i/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish_device(self, "s/v/ctrl", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
    if (self->nice_level) {
        publish_device(self, "h/fs", &jsdrv_union_u32_r(1000000),JSDRV_TIMEOUT_MS_DEFAULT);
    }
    state_update(self, ST_STREAM);
    return 0;
}

static int32_t t_stream_stop(struct app_s * self) {
    ROE(publish_device(self, "s/i/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish_device(self, "s/v/ctrl", &jsdrv_union_u32_r(0), JSDRV_TIMEOUT_MS_DEFAULT));
    state_update(self, ST_OPEN);
    return 0;
}

static void on_info(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrv_buffer_info_s * info = (struct jsdrv_buffer_info_s *) user_data;
    (void) topic;
    if (value->size > sizeof(*info)) {
        printf("info too big\n");
        return;
    }
    memcpy(info, value->value.bin, value->size);
}

static int32_t t_buffer_start(struct app_s * self) {
    state_update(self, ST_BUFFER);
    memset(self->buffer_info, 0, sizeof(self->buffer_info));

    // for now, use fixed buffer_id 1
    ROE(publish(self, "m/@/!add", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, "m/001/a/!add", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, "m/001/a/!add", &jsdrv_union_u32_r(2), JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, "m/001/g/size", &jsdrv_union_u32_r(256*1024*1024), JSDRV_TIMEOUT_MS_DEFAULT));

    struct jsdrv_topic_s t;
    if (0 == self->device_prefix[0]) {
        printf("ERROR: device_publish but no active device\n");
        return 1;
    }
    jsdrv_topic_set(&t, self->device_prefix);
    jsdrv_topic_append(&t, "s/i/!data");
    ROE(jsdrv_subscribe(self->context, "m/001/s/001/info", JSDRV_SFLAG_PUB, on_info, &self->buffer_info[1], JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, "m/001/s/001/topic", &jsdrv_union_str(t.topic), JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&t, self->device_prefix);
    jsdrv_topic_append(&t, "s/v/!data");
    ROE(jsdrv_subscribe(self->context, "m/001/s/002/info", JSDRV_SFLAG_PUB, on_info, &self->buffer_info[2], JSDRV_TIMEOUT_MS_DEFAULT));
    ROE(publish(self, "m/001/s/002/topic", &jsdrv_union_str(t.topic), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}

static int32_t t_buffer_stop(struct app_s * self) {
    ROE(publish(self, "m/@/!remove", &jsdrv_union_u32_r(1), JSDRV_TIMEOUT_MS_DEFAULT));
    state_update(self, ST_STREAM);
    return 0;
}

static int32_t t_buffer_req(struct app_s * self) {
    struct jsdrv_buffer_info_s * info = &self->buffer_info[1];
    if (0 == info->size_in_samples) {
        // no data to request
        return 0;
    }
    struct jsdrv_buffer_request_s req = {
            .version = 1,
            .time_type = JSDRV_TIME_SAMPLES,
            .rsv1_u8 = 0,
            .rsv2_u8 = 0,
            .rsv3_u32 = 0,
            .time = {.samples = info->time_range_samples},
            .rsp_topic = {'r', '/', 't', 0},
            .rsp_id = 0,
    };
    ROE(publish(self, "m/001/s/001/!req", &jsdrv_union_bin((uint8_t *) &req, sizeof(req)), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}


struct state_s states[] = {
        {
            ST_FINALIZED,
            {
                    {10, "initialize", t_initialize},
                    {10, "refinalize", t_finalize},
                    TRANSITION_END,
            }
        },
        {
            ST_INITIALIZED,
            {
                    {10, "reinitialize", t_initialize},
                    {10, "finalize", t_finalize},
                    {90, "open", t_open},
                    TRANSITION_END,
            }
        },
        {
            ST_OPEN,
            {
                    {10, "close", t_close},
                    // sample_rate
                    {40, "stream", t_stream_start},
                    {60, "wait", t_wait},
                    {20, "sample_rate", t_sample_rate},
                    TRANSITION_END,
            }
        },
        {
            ST_STREAM,
            {
                    {10, "buffer", t_buffer_start},
                    {10, "stop", t_stream_stop},
                    {60, "wait", t_wait},
                    TRANSITION_END,
            }
        },
        {
            ST_BUFFER,
            {
                    {10, "t_buffer_stop", t_buffer_stop},
                    {40, "t_buffer_req", t_buffer_req},
                    {80, "wait", t_wait},
                    TRANSITION_END,
            }
        },

        {NULL, {TRANSITION_END}},  // must be last
};

static void on_log_recv(void * user_data, struct jsdrv_log_header_s const * header,
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

static int usage(void) {
    printf(
        "usage: fuzz [--<flag>] [--<arg> <value>]\n"
        "Perform Joulescope driver fuzz testing.\n"
        "\n"
        "Flags:\n"
        "  nice       How nice the fuzz test should be.\n"
        "             Each instance increases the niceness level..\n"
        "             0 = no niceness\n"
        "             1 = restore h/fs after streaming\n"
        "\n"
        "Optional arguments:\n"
        "  log_level  Configure the log level to stdout.  One of:\n"
        "             off, emergency, alert, critical, [error], warning,\n"
        "             notice, info, debug1, debug2, debug3, all\n"
        "  random     The 64-bit random number seed\n"
        "  device     Restrict fuzzing to a specific device prefix\n"
        "             (e.g. u/js320/8W2A).  Without this option, open\n"
        "             selects randomly from all enumerated devices.\n"
    );
    return 1;
}

int on_help(struct app_s * self, int argc, char * argv[]) {
    (void) self;
    (void) argc;
    (void) argv;
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

static int32_t iteration(struct app_s * self) {
    for (size_t state_idx = 0; NULL != self->states[state_idx].name; ++state_idx) {
        struct state_s * state = &self->states[state_idx];
        if (0 == strcmp(self->state_name, state->name)) {
            // Compute total weight
            uint32_t weight_total = 0;
            for (size_t transition_idx = 0; NULL != state->transitions[transition_idx].name; ++transition_idx) {
                struct transition_s * transition = &state->transitions[transition_idx];
                weight_total += transition->weight;
            }

            // Select a transition
            uint32_t weight_select = random_range_u32(0, weight_total);
            weight_total = 0;
            for (size_t transition_idx = 0; NULL != state->transitions[transition_idx].name; ++transition_idx) {
                struct transition_s * transition = &state->transitions[transition_idx];
                weight_total += transition->weight;
                if (weight_select < weight_total) {
                    printf("state %s | transition %s\n", state->name, transition->name);
                    return transition->fn(self);
                }
            }
            return 0;
        }
    }
    printf("ERROR state not found: %s\n", self->state_name);
    return 1;
}

int main(int argc, char * argv[]) {
    int32_t rc = 0;
    struct app_s app = {
            .context = NULL,
            .state_name = ST_FINALIZED,
            .states = states,
            .nice_level = 0,
    };
    int8_t log_level = JSDRV_LOG_LEVEL_ERROR;

    ARG_CONSUME();  // argv[0] == executable path
    while (argc) {
        if ((jsdrv_cstr_casecmp("--log-level", argv[0]) == 0) || (jsdrv_cstr_casecmp("--log_level", argv[0]) == 0)) {
            ARG_CONSUME();
            if (log_level_cvt(argv[0], &log_level)) {
                printf("Invalid log level: %s\n", argv[0]);
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp("--random", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u64(argv[0], &random_seed)) {
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp("--device", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            snprintf(app.device_filter, sizeof(app.device_filter), "%s", argv[0]);
            ARG_CONSUME();
        } else if (0 == strcmp("--nice", argv[0])) {
            ARG_CONSUME();
            app.nice_level++;
        } else {
            return usage();
        }
    }

    crash_handler_install();
    signal(SIGABRT, signal_handler);
    signal(SIGINT, signal_handler);

    printf("Joulescope driver version = %s\n", JSDRV_VERSION_STR);
    printf("Random seed = %" PRIu64 "\n", random_seed);
    jsdrv_log_initialize();
    jsdrv_log_register(on_log_recv, NULL);
    jsdrv_log_level_set(log_level);
    while ((rc == 0) && !quit_) {
        rc = iteration(&app);
    }
    jsdrv_finalize(app.context, 0);
    if (rc) {
        printf("### ERROR return code %d %s %s###\n", rc,
               jsdrv_error_code_name(rc),
               jsdrv_error_code_description(rc));
    }
    return rc;
}
