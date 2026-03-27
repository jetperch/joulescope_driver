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
 * @brief Display PubSub instance info (././info topic).
 *
 * Uses the !state GET protocol to actively fetch the retained
 * ././info value from the device, since retained values published
 * before the comm link are not automatically forwarded to the host.
 */

#include "minibitty_exe_prv.h"
#include "mb/stdmsg.h"
#include <stdio.h>
#include <string.h>

#include "jsdrv/os_event.h"
#include "jsdrv/os_thread.h"

#ifndef MB_STDMSG_PUBSUB_INFO
#define MB_STDMSG_PUBSUB_INFO 0x09
#endif
#ifndef MB_STDMSG_STATE
#define MB_STDMSG_STATE 0x08
#endif
#ifndef MB_TOPIC_LENGTH_MAX
#define MB_TOPIC_LENGTH_MAX 32
#endif

// Mirror of mb/pubsub.h types for host-side parsing.
enum mb_pubsub_info_blob_type_e {
    MB_PUBSUB_INFO_BLOB_INVALID = 0,
    MB_PUBSUB_INFO_BLOB_PUBSUB_META = 1,
    MB_PUBSUB_INFO_BLOB_TRACE = 2,
};

struct mb_pubsub_info_blob_s {
    uint8_t  blob_type;
    uint8_t  target;
    uint16_t page_size;
    uint32_t offset;
    char     topic[32];
};

struct mb_pubsub_info_s {
    uint8_t  prefix;
    uint8_t  blob_count;
    uint16_t rsv_u16;
};

// Mirror of mb/stdmsg.h state types.
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
    uint8_t  value[];   // 8-byte aligned, padded
};


struct pubsub_info_state_s {
    jsdrv_os_event_t event;
    int received;
    uint8_t rsp_buf[512];
    uint32_t rsp_size;
};

static struct pubsub_info_state_s state_;

static const char * blob_type_name(uint8_t blob_type) {
    switch (blob_type) {
        case MB_PUBSUB_INFO_BLOB_PUBSUB_META: return "pubsub_meta";
        case MB_PUBSUB_INFO_BLOB_TRACE:       return "trace";
        default:                               return "unknown";
    }
}

static void on_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;

    if (value->type != JSDRV_UNION_STDMSG || value->size < sizeof(struct mb_stdmsg_header_s)) {
        return;  // not a stdmsg, ignore
    }
    const struct mb_stdmsg_header_s * hdr =
        (const struct mb_stdmsg_header_s *) value->value.bin;
    if (hdr->type != MB_STDMSG_STATE) {
        return;  // not a state response, ignore
    }

    uint32_t payload_size = value->size - sizeof(struct mb_stdmsg_header_s);
    if (payload_size > sizeof(state_.rsp_buf)) {
        payload_size = sizeof(state_.rsp_buf);
    }
    memcpy(state_.rsp_buf, value->value.bin + sizeof(struct mb_stdmsg_header_s), payload_size);
    state_.rsp_size = payload_size;
    state_.received = 1;
    jsdrv_os_event_signal(state_.event);
}

static int state_publish(struct app_s * self, const char * prefix,
                         uint8_t type, uint32_t transaction_id,
                         const char * target_topic) {
    uint16_t payload_size = (uint16_t) sizeof(struct state_header_s);
    if (target_topic) {
        payload_size += MB_TOPIC_LENGTH_MAX;
    }

    uint8_t buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct state_header_s) + MB_TOPIC_LENGTH_MAX];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_STATE;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;

    struct state_header_s * sh = (struct state_header_s *)(hdr + 1);
    memset(sh, 0, sizeof(*sh));
    sh->transaction_id = transaction_id;
    sh->type = type;

    if (target_topic) {
        char * target = (char *)(sh + 1);
        memset(target, 0, MB_TOPIC_LENGTH_MAX);
        strncpy(target, target_topic, MB_TOPIC_LENGTH_MAX - 1);
    }

    // Publish to {device}/{prefix}/./!state
    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    char state_path[16];
    state_path[0] = prefix[0];
    state_path[1] = '/';
    state_path[2] = '.';
    state_path[3] = '/';
    state_path[4] = '!';
    state_path[5] = 's';
    state_path[6] = 't';
    state_path[7] = 'a';
    state_path[8] = 't';
    state_path[9] = 'e';
    state_path[10] = '\0';
    jsdrv_topic_append(&topic, state_path);

    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = (uint32_t)(sizeof(struct mb_stdmsg_header_s) + payload_size);
    v.value.bin = buf;
    return jsdrv_publish(self->context, topic.topic, &v, 0);
}

static int wait_rsp(uint32_t timeout_ms) {
    state_.received = 0;
    int32_t result = jsdrv_os_event_wait(state_.event, timeout_ms);
    if (result) {
        return 1;
    }
    return (state_.received > 0) ? 0 : 1;
}

static void display_info(const struct state_entry_s * entry) {
    if (entry->value_type != JSDRV_UNION_STDMSG) {
        printf("Unexpected entry value_type: %u\n", entry->value_type);
        return;
    }
    if (entry->size < sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_pubsub_info_s)) {
        printf("Entry value too short: %u bytes\n", entry->size);
        return;
    }

    const struct mb_stdmsg_header_s * inner_hdr =
        (const struct mb_stdmsg_header_s *) entry->value;
    if (inner_hdr->type != MB_STDMSG_PUBSUB_INFO) {
        printf("Unexpected inner stdmsg type: %u\n", inner_hdr->type);
        return;
    }

    const struct mb_pubsub_info_s * info =
        (const struct mb_pubsub_info_s *)(inner_hdr + 1);
    uint32_t expected = sizeof(struct mb_stdmsg_header_s)
        + sizeof(struct mb_pubsub_info_s)
        + info->blob_count * sizeof(struct mb_pubsub_info_blob_s);
    if (entry->size < expected) {
        printf("Entry truncated: %u < %u\n", entry->size, expected);
        return;
    }

    printf("PubSub Info: prefix='%c', blobs=%u\n",
           info->prefix, info->blob_count);

    const struct mb_pubsub_info_blob_s * blobs =
        (const struct mb_pubsub_info_blob_s *)(info + 1);
    for (uint8_t i = 0; i < info->blob_count; ++i) {
        printf("  [%u] %-12s  target=%u  page=%u  offset=0x%06X\n",
               i, blob_type_name(blobs[i].blob_type),
               blobs[i].target, blobs[i].page_size, blobs[i].offset);
        printf("      topic=%s\n", blobs[i].topic);
    }
}

static int usage(void) {
    printf(
        "usage: minibitty pubsub_info <prefix> [device_filter]\n"
        "\n"
        "Display the ././info topic for a PubSub instance.\n"
        "Uses the !state GET protocol to fetch the retained value.\n"
        "\n"
        "  <prefix>        PubSub instance prefix (e.g. c, s)\n"
        "  [device_filter] Optional device filter string\n"
        "\n"
        "Examples:\n"
        "  minibitty pubsub_info c\n"
        "  minibitty pubsub_info s\n"
        "  minibitty pubsub_info c js320\n"
    );
    return 1;
}

int on_pubsub_info(struct app_s * self, int argc, char * argv[]) {
    if (argc < 1) {
        return usage();
    }

    const char * prefix = argv[0];
    ARG_CONSUME();
    if (strlen(prefix) != 1) {
        printf("prefix must be a single character, got: %s\n", prefix);
        return usage();
    }

    char * device_filter = NULL;
    if (argc > 0) { device_filter = argv[0]; ARG_CONSUME(); }

    ROE(app_match(self, device_filter));

    memset(&state_, 0, sizeof(state_));
    state_.event = jsdrv_os_event_alloc();

    // Subscribe to {device}/h/!rsp before open
    struct jsdrv_topic_s rsp_topic;
    jsdrv_topic_set(&rsp_topic, self->device.topic);
    jsdrv_topic_append(&rsp_topic, "h/!rsp");
    jsdrv_subscribe(self->context, rsp_topic.topic, JSDRV_SFLAG_PUB,
                     on_rsp, NULL, 0);

    ROE(jsdrv_open(self->context, self->device.topic,
                   JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));
    jsdrv_thread_sleep_ms(500);

    // Build target topic: {prefix}/./info
    char target_topic[MB_TOPIC_LENGTH_MAX];
    target_topic[0] = prefix[0];
    target_topic[1] = '/';
    target_topic[2] = '.';
    target_topic[3] = '/';
    target_topic[4] = 'i';
    target_topic[5] = 'n';
    target_topic[6] = 'f';
    target_topic[7] = 'o';
    target_topic[8] = '\0';

    // Send !state GET_INIT targeting just the info topic
    uint32_t txn_id = 1;
    printf("Querying %s/./info via !state GET ...\n", prefix);
    int32_t pub_rc = state_publish(self, prefix, STATE_TYPE_GET_INIT, txn_id, target_topic);
    if (pub_rc) {
        printf("ERROR: publish GET_INIT failed: %d\n", pub_rc);
        jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
        jsdrv_os_event_free(state_.event);
        return 1;
    }

    int rc = wait_rsp(3000);
    if (rc) {
        printf("ERROR: no GET_INIT response\n");
        jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
        jsdrv_os_event_free(state_.event);
        return 1;
    }

    // Check GET_INIT response status
    const struct state_header_s * rsp_hdr = (const struct state_header_s *) state_.rsp_buf;
    if (rsp_hdr->status != 0) {
        printf("ERROR: GET_INIT status=%u\n", rsp_hdr->status);
        jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
        jsdrv_os_event_free(state_.event);
        return 1;
    }

    // Send GET_NEXT to receive entries
    pub_rc = state_publish(self, prefix, STATE_TYPE_GET_NEXT, txn_id, NULL);
    if (pub_rc) {
        printf("ERROR: publish GET_NEXT failed: %d\n", pub_rc);
        jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
        jsdrv_os_event_free(state_.event);
        return 1;
    }

    rc = wait_rsp(3000);
    if (rc) {
        printf("ERROR: no GET_NEXT response\n");
        jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
        jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
        jsdrv_os_event_free(state_.event);
        return 1;
    }

    // Parse the GET_RSP
    rsp_hdr = (const struct state_header_s *) state_.rsp_buf;
    if (rsp_hdr->status != 0) {
        printf("ERROR: GET_NEXT status=%u\n", rsp_hdr->status);
        rc = 1;
    } else {
        uint32_t offset = sizeof(struct state_header_s);
        int found = 0;
        while (offset + sizeof(struct state_entry_s) <= state_.rsp_size) {
            const struct state_entry_s * entry =
                (const struct state_entry_s *)(state_.rsp_buf + offset);
            if (strstr(entry->topic, "/./info")) {
                display_info(entry);
                found = 1;
                break;
            }
            uint16_t padded = (entry->size + 7) & ~7;
            uint16_t entry_size = 40 + padded;
            if (padded < 8) { entry_size = 40 + 8; }
            offset += entry_size;
        }
        if (!found) {
            printf("No ././info entry found in state response\n");
            rc = 1;
        }
    }

    jsdrv_unsubscribe(self->context, rsp_topic.topic, on_rsp, NULL, 0);
    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);
    jsdrv_os_event_free(state_.event);
    return rc;
}
