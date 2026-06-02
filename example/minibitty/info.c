/*
 * Copyright 2022-2026 Jetperch LLC
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

#include "minibitty_exe_prv.h"
#include "boot_info.h"
#include "mb/stdmsg.h"
#include "jsdrv/cstr.h"
#include "jsdrv/version.h"
#include "jsdrv/os_sem.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// c/sys mem-read targets (minibitty/src/tasks/sys.c)
#define SYS_TARGET_BOOT         1
#define SYS_TARGET_PERSONALITY  2
#define SYS_READ_MAX            256U   // one page; device clamps to struct size


struct sys_read_s {
    uint8_t * buf;
    uint32_t buf_len;
    uint32_t recv_len;
    int status;
    jsdrv_os_sem_t sem;
};

static struct sys_read_s sys_read_;


static void on_sys_rsp(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    uint32_t hdr_offset = 0;
    if (value->type == JSDRV_UNION_STDMSG) {
        hdr_offset = sizeof(struct mb_stdmsg_header_s);
    }
    if (value->size < hdr_offset + sizeof(struct mb_stdmsg_mem_s)) {
        sys_read_.status = 1;
        jsdrv_os_sem_release(sys_read_.sem);
        return;
    }
    const struct mb_stdmsg_mem_s * rsp = (const struct mb_stdmsg_mem_s *)
        (value->value.bin + hdr_offset);
    uint32_t data_size = value->size - hdr_offset - (uint32_t) sizeof(struct mb_stdmsg_mem_s);

    if (rsp->status != 0) {
        sys_read_.status = rsp->status;
    } else if (rsp->operation == MB_STDMSG_MEM_OP_READ) {
        uint32_t copy_size = rsp->length;
        if (copy_size > data_size) {
            copy_size = data_size;
        }
        if (copy_size > sys_read_.buf_len) {
            copy_size = sys_read_.buf_len;
        }
        memcpy(sys_read_.buf, rsp->data, copy_size);
        sys_read_.recv_len = copy_size;
        sys_read_.status = 0;
    }
    jsdrv_os_sem_release(sys_read_.sem);
}

static int sys_read(struct app_s * self, uint8_t target,
                    uint8_t * buf, uint32_t buf_len, uint32_t * recv_len) {
    sys_read_.buf = buf;
    sys_read_.buf_len = buf_len;
    sys_read_.recv_len = 0;
    sys_read_.status = 0;

    uint8_t cmd_buf[sizeof(struct mb_stdmsg_header_s) + sizeof(struct mb_stdmsg_mem_s)];
    struct mb_stdmsg_header_s * hdr = (struct mb_stdmsg_header_s *) cmd_buf;
    hdr->version = 0;
    hdr->type = MB_STDMSG_MEM;
    hdr->origin_prefix = 'h';
    hdr->metadata = 0;
    struct mb_stdmsg_mem_s * cmd = (struct mb_stdmsg_mem_s *) (hdr + 1);
    memset(cmd, 0, sizeof(*cmd));
    cmd->transaction_id = 1;
    cmd->target = target;
    cmd->operation = MB_STDMSG_MEM_OP_READ;
    cmd->timeout_ms = 1000;
    cmd->offset = 0;
    cmd->length = buf_len;

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "c/sys/!cmd");
    struct jsdrv_union_s v;
    v.type = JSDRV_UNION_STDMSG;
    v.size = (uint32_t) sizeof(cmd_buf);
    v.value.bin = cmd_buf;
    int32_t rc = jsdrv_publish(self->context, topic.topic, &v, 0);
    if (rc) {
        return rc;
    }
    if (jsdrv_os_sem_wait(sys_read_.sem, 5000)) {
        printf("ERROR: timeout reading c/sys target %u\n", target);
        return 1;
    }
    *recv_len = sys_read_.recv_len;
    return sys_read_.status;
}

static int device_info(struct app_s * self, const char * device) {
    struct jsdrv_topic_s topic;
    uint8_t buf[SYS_READ_MAX];
    uint32_t recv_len = 0;
    int rc;

    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, 1000));
    printf("%s\n", device);

    sys_read_.sem = jsdrv_os_sem_alloc(0, 1);
    jsdrv_topic_set(&topic, device);
    jsdrv_topic_append(&topic, "h/!rsp");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_sys_rsp, NULL, 0);

    rc = sys_read(self, SYS_TARGET_BOOT, buf, sizeof(buf), &recv_len);
    if (rc) {
        printf("Boot:\n    <read failed: status=%d>\n", rc);
    } else {
        struct boot_info_s boot;
        boot_info_decode(buf, recv_len, &boot);
        boot_info_print(stdout, &boot);
    }

    rc = sys_read(self, SYS_TARGET_PERSONALITY, buf, sizeof(buf), &recv_len);
    if (rc) {
        printf("Personality:\n    <read failed: status=%d>\n", rc);
    } else {
        struct personality_info_s pers;
        personality_info_decode(buf, recv_len, &pers);
        personality_info_print(stdout, &pers);
    }

    jsdrv_unsubscribe(self->context, topic.topic, on_sys_rsp, NULL, 0);
    jsdrv_os_sem_free(sys_read_.sem);
    sys_read_.sem = NULL;

    return jsdrv_close(self->context, device, 1000);
}

static int usage(void) {
    printf("usage: minibitty info [--verbose] [device_path]\n");
    return 1;
}

int on_info(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    printf("JSDRV version: %s\n", JSDRV_VERSION_STR);

    ROE(app_scan(self));
    if (0 == self->devices[0]) {
        printf("No devices found\n");
        return 1;
    }
    char * d = &self->devices[0];
    char * p = &self->devices[0];
    size_t sz = strlen(d);

    printf("Devices:\n");
    for (size_t i = 0; i <= sz; ++i) {
        if (*p == ',') {
            *p++ = 0;
            printf("    %s\n", d);
            d = p;
        } else if (*p == 0) {
            printf("    %s\n", d);
            break;
        }
        p++;
    }

    ROE(app_match(self, device_filter));
    return device_info(self, self->device.topic);
}
