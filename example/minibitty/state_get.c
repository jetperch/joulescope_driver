/*
 * Copyright 2026 Jetperch LLC
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
 * @brief Full !state GET stress test command with pipelining.
 */

#include "minibitty_exe_prv.h"
#include "mb/stdmsg.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <windows.h>

#ifndef MB_STDMSG_STATE
#define MB_STDMSG_STATE 0x08
#endif
#ifndef MB_TOPIC_LENGTH_MAX
#define MB_TOPIC_LENGTH_MAX 32
#endif

#define PIPELINE_MAX 16

enum {
    STATE_TYPE_GET_INIT = 1,
    STATE_TYPE_GET_RSP  = 2,
    STATE_TYPE_GET_NEXT = 3,
};

enum {
    STATE_FLAG_START = 0x01,
    STATE_FLAG_END   = 0x02,
};

struct state_header_s {
    uint32_t transaction_id;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  status;
    uint8_t  rsv;
};

struct state_entry_s {
    char     topic[32];
    uint8_t  value_type;
    uint8_t  rsv1;
    uint16_t size;
    uint32_t rsv2;
    uint8_t  value[];
};

struct state_get_s {
    HANDLE semaphore;
    volatile LONG outstanding;
    volatile LONG error_count;
    volatile LONG entry_count;
    volatile LONG end_seen;
    uint32_t pipeline_depth;
    int verbose;
};

static struct state_get_s sg_;

static void on_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_STDMSG || value->size < 8 + 8) {
        return;
    }
    const uint8_t * data = value->value.bin;
    const struct mb_stdmsg_header_s * hdr = (const struct mb_stdmsg_header_s *) data;
    if (hdr->type != MB_STDMSG_STATE) {
        return;
    }

    const struct state_header_s * rsp = (const struct state_header_s *)(data + 8);
    uint32_t rsp_size = value->size - 8;

    if (rsp->status != 0) {
        if (!sg_.end_seen) {
            InterlockedIncrement(&sg_.error_count);
        }
    } else if (rsp->type == STATE_TYPE_GET_RSP) {
        // Count entries
        uint32_t offset = sizeof(struct state_header_s);
        while (offset + 48 <= rsp_size) {
            const struct state_entry_s * entry =
                (const struct state_entry_s *)((const uint8_t *)rsp + offset);
            if (entry->topic[0] == '\0') break;
            if (sg_.verbose) {
                printf("  %s type=%u size=%u\n", entry->topic, entry->value_type, entry->size);
            }
            InterlockedIncrement(&sg_.entry_count);
            uint16_t padded = (entry->size + 7) & ~7;
            if (padded < 8) padded = 8;
            offset += 40 + padded;
        }
        if (rsp->flags & STATE_FLAG_END) {
            InterlockedExchange(&sg_.end_seen, 1);
        }
    }

    InterlockedDecrement(&sg_.outstanding);
    ReleaseSemaphore(sg_.semaphore, 1, NULL);
}

static int state_publish(struct app_s * self, const char * prefix,
                         uint8_t type, uint32_t transaction_id) {
    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct state_header_s)];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_STATE;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct state_header_s * sh = (struct state_header_s *)(hdr + 1);
    memset(sh, 0, sizeof(*sh));
    sh->transaction_id = transaction_id;
    sh->type = type;

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    char state_path[16];
    state_path[0] = prefix[0];
    state_path[1] = '/'; state_path[2] = '.'; state_path[3] = '/';
    state_path[4] = '!'; state_path[5] = 's'; state_path[6] = 't';
    state_path[7] = 'a'; state_path[8] = 't'; state_path[9] = 'e';
    state_path[10] = '\0';
    jsdrv_topic_append(&topic, state_path);

    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = sizeof(buf);
    v.value.bin = buf;
    v.flags = 0;
    v.app = 0;
    return jsdrv_publish(self->context, topic.topic, &v, 0);
}

static int pipeline_wait_slot(uint32_t timeout_ms) {
    while (sg_.outstanding >= (LONG) sg_.pipeline_depth) {
        DWORD result = WaitForSingleObject(sg_.semaphore, timeout_ms);
        if (result != WAIT_OBJECT_0) return 1;
    }
    return (sg_.error_count > 0) ? 1 : 0;
}

static int pipeline_drain(uint32_t timeout_ms) {
    while (sg_.outstanding > 0) {
        DWORD result = WaitForSingleObject(sg_.semaphore, timeout_ms);
        if (result != WAIT_OBJECT_0) return 1;
    }
    return (sg_.error_count > 0) ? 1 : 0;
}

static int do_one_state_get(struct app_s * self, const char * prefix,
                            uint32_t txn_id) {
    sg_.outstanding = 0;
    sg_.error_count = 0;
    sg_.entry_count = 0;
    sg_.end_seen = 0;

    // GET_INIT (synchronous, pipeline depth 1)
    InterlockedIncrement(&sg_.outstanding);
    int32_t pub_rc = state_publish(self, prefix, STATE_TYPE_GET_INIT, txn_id);
    if (pub_rc) return -1;
    if (pipeline_drain(3000)) return -1;

    // GET_NEXT with pipelining
    while (!sg_.end_seen) {
        if (pipeline_wait_slot(3000)) return -1;
        if (sg_.end_seen) break;
        InterlockedIncrement(&sg_.outstanding);
        pub_rc = state_publish(self, prefix, STATE_TYPE_GET_NEXT, txn_id);
        if (pub_rc) {
            InterlockedDecrement(&sg_.outstanding);
            return -1;
        }
    }
    if (pipeline_drain(3000)) return -1;

    return (int) sg_.entry_count;
}

static int usage(void) {
    printf(
        "usage: minibitty state_get <prefix> [--repeat N] [--pipeline N]\n"
        "                           [--verbose] [device_filter]\n"
        "\n"
        "Perform full !state GET (untargeted) for a pubsub instance.\n"
        "\n"
        "  <prefix>        PubSub prefix (e.g. c, s)\n"
        "  --repeat N      Repeat N times (default 1)\n"
        "  --pipeline N    Pipeline depth for GET_NEXT (default 1, max %u)\n"
        "  --verbose       Print each topic entry (first iteration only)\n"
        "\n"
        "Examples:\n"
        "  minibitty state_get c\n"
        "  minibitty state_get c --repeat 100000\n"
        "  minibitty state_get c --pipeline 4 --repeat 1000\n"
        "  minibitty state_get s --verbose\n",
        PIPELINE_MAX
    );
    return 1;
}

int on_state_get(struct app_s * self, int argc, char * argv[]) {
    if (argc < 1) return usage();

    const char * prefix = argv[0];
    ARG_CONSUME();
    if (strlen(prefix) != 1) {
        printf("prefix must be a single character\n");
        return usage();
    }

    uint32_t repeat = 1;
    uint32_t pipeline = 1;
    int verbose = 0;
    char * device_filter = NULL;

    while (argc > 0) {
        if (0 == strcmp(argv[0], "--repeat")) {
            ARG_CONSUME();
            if (argc < 1) { printf("--repeat requires N\n"); return usage(); }
            repeat = strtoul(argv[0], NULL, 0);
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--pipeline")) {
            ARG_CONSUME();
            if (argc < 1) { printf("--pipeline requires N\n"); return usage(); }
            pipeline = strtoul(argv[0], NULL, 0);
            if (pipeline < 1) pipeline = 1;
            if (pipeline > PIPELINE_MAX) pipeline = PIPELINE_MAX;
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--verbose")) {
            verbose = 1;
            ARG_CONSUME();
        } else if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else {
            printf("unknown option: %s\n", argv[0]);
            return usage();
        }
    }

    ROE(app_match(self, device_filter));

    memset(&sg_, 0, sizeof(sg_));
    sg_.semaphore = CreateSemaphore(NULL, 0, PIPELINE_MAX, NULL);
    sg_.pipeline_depth = pipeline;

    struct jsdrv_topic_s rsp_topic;
    jsdrv_topic_set(&rsp_topic, self->device.topic);
    jsdrv_topic_append(&rsp_topic, "h/!rsp");
    jsdrv_subscribe(self->context, rsp_topic.topic, JSDRV_SFLAG_PUB,
                     on_rsp, NULL, 0);

    ROE(jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    Sleep(500);

    int rc = 0;
    uint32_t txn_id = 0x7700;
    for (uint32_t i = 0; i < repeat; ++i) {
        if (quit_) { rc = 1; break; }
        txn_id++;
        sg_.verbose = verbose && (i == 0);
        int entries = do_one_state_get(self, prefix, txn_id);
        if (entries < 0) {
            printf("FAILED at iteration %u\n", i);
            fflush(stdout);
            rc = 1;
            break;
        }
        if ((i % 1000) == 0 || i == repeat - 1) {
            printf("iteration %u/%u: %d entries\n", i + 1, repeat, entries);
            fflush(stdout);
        }
    }

    jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    CloseHandle(sg_.semaphore);
    return rc;
}
