/*
 * SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
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

#include "mbgen.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format details: minibitty include/mb/dbg.h and pyminibitty/dbg.py */
#define MB_DBG_HEADER "MBty_dbg\x0D\x0A \x0A\x1A\xB2\x1C"
#define MB_DBG_HEADER_SIZE (64)
#define MB_DBG_CHECK_MULT (0xcba9U)

enum mb_dbg_tag_e {
    MB_DBG_TAG_INVALID = 0,
    MB_DBG_TAG_FILES = 1,
    MB_DBG_TAG_LOGS = 2,
    MB_DBG_TAG_IRQS = 3,
    MB_DBG_TAG_TASKS = 4,
    MB_DBG_TAG_STRINGS = 5,
    MB_DBG_TAG_COUNT
};

static uint32_t crc32_ieee(const uint8_t * data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

static uint32_t rd_u32(const uint8_t * p) {
    return ((uint32_t) p[0])
        | (((uint32_t) p[1]) << 8)
        | (((uint32_t) p[2]) << 16)
        | (((uint32_t) p[3]) << 24);
}

static uint8_t * file_read(const char * path, uint32_t * size_out) {
    FILE * f = fopen(path, "rb");
    if (NULL == f) {
        printf("ERROR: could not open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        printf("ERROR: empty file %s\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t * data = malloc(sz);
    if (NULL == data) {
        fclose(f);
        return NULL;
    }
    if (1 != fread(data, sz, 1, f)) {
        printf("ERROR: could not read %s\n", path);
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t) sz;
    return data;
}

int mbgen_load(struct mbgen_s * self, const char * path) {
    memset(self, 0, sizeof(*self));
    uint32_t size = 0;
    uint8_t * b = file_read(path, &size);
    if (NULL == b) {
        return 1;
    }
    self->data = b;
    self->size = size;

    if ((size < MB_DBG_HEADER_SIZE) || (0 != memcmp(b, MB_DBG_HEADER, 16))) {
        printf("ERROR: %s is not a MiniBitty debug file\n", path);
        goto error;
    }
    if (rd_u32(b + 20) != size) {
        printf("ERROR: mbgen file size mismatch: %u != %u\n", rd_u32(b + 20), size);
        goto error;
    }
    if (crc32_ieee(b, 60) != rd_u32(b + 60)) {
        printf("ERROR: mbgen header check failed\n");
        goto error;
    }

    const uint8_t * sections[MB_DBG_TAG_COUNT] = {NULL};
    uint32_t section_sizes[MB_DBG_TAG_COUNT] = {0};
    uint32_t offset = MB_DBG_HEADER_SIZE;
    while (offset < size) {
        if ((size - offset) < 16) {
            printf("ERROR: mbgen truncated TLV @ %u\n", offset);
            goto error;
        }
        uint32_t word0 = rd_u32(b + offset);
        uint32_t check = 0xffffffffU & (
                (word0 & 0xffff) * MB_DBG_CHECK_MULT
                + ((word0 >> 16) & 0xffff) * MB_DBG_CHECK_MULT);
        if (check != rd_u32(b + offset + 4)) {
            printf("ERROR: mbgen TLV check failed @ %u\n", offset);
            goto error;
        }
        uint32_t tag = word0 >> 24;
        uint32_t sz = word0 & 0x00ffffff;
        uint32_t stuff = 7 - ((sz + 3) & 7);
        uint32_t offset_next = offset + 8 + sz + stuff + 4;
        if ((offset_next < offset) || (offset_next > size)) {
            printf("ERROR: mbgen TLV size invalid @ %u\n", offset);
            goto error;
        }
        if (crc32_ieee(b + offset + 8, sz + stuff) != rd_u32(b + offset_next - 4)) {
            printf("ERROR: mbgen TLV crc failed @ %u\n", offset);
            goto error;
        }
        if (tag < MB_DBG_TAG_COUNT) {
            sections[tag] = b + offset + 8;
            section_sizes[tag] = sz;
        }
        offset = offset_next;
    }

    if (NULL == sections[MB_DBG_TAG_STRINGS]) {
        printf("ERROR: mbgen missing strings section\n");
        goto error;
    }
    self->strings = (const char *) sections[MB_DBG_TAG_STRINGS];
    self->strings_size = section_sizes[MB_DBG_TAG_STRINGS];
    if ((0 == self->strings_size) || (0 != self->strings[self->strings_size - 1])) {
        printf("ERROR: mbgen strings section not terminated\n");
        goto error;
    }
    /* TLV values start 8-byte aligned within the malloc'd buffer */
    self->files = (const uint32_t *) sections[MB_DBG_TAG_FILES];
    self->files_count = section_sizes[MB_DBG_TAG_FILES] / 4;
    self->irqs = (const uint32_t *) sections[MB_DBG_TAG_IRQS];
    self->irqs_count = section_sizes[MB_DBG_TAG_IRQS] / 4;
    self->tasks = (const uint32_t *) sections[MB_DBG_TAG_TASKS];
    self->tasks_count = section_sizes[MB_DBG_TAG_TASKS] / 4;
    self->logs = (const uint32_t *) sections[MB_DBG_TAG_LOGS];
    self->logs_count = section_sizes[MB_DBG_TAG_LOGS] / 8;

    uint32_t mb_ver = rd_u32(b + 24);
    uint32_t app_ver = rd_u32(b + 28);
    printf("mbgen: loaded %s\n", path);
    printf("mbgen: minibitty %u.%u.%u, app %u.%u.%u, vendor 0x%04x, product 0x%04x\n",
           (mb_ver >> 24) & 0xff, (mb_ver >> 16) & 0xff, mb_ver & 0xffff,
           (app_ver >> 24) & 0xff, (app_ver >> 16) & 0xff, app_ver & 0xffff,
           b[40] | (((uint32_t) b[41]) << 8), b[42] | (((uint32_t) b[43]) << 8));
    return 0;

error:
    mbgen_finalize(self);
    return 1;
}

void mbgen_finalize(struct mbgen_s * self) {
    if (self->data) {
        free(self->data);
    }
    memset(self, 0, sizeof(*self));
}

static const char * str_get(const struct mbgen_s * self, uint32_t offset) {
    if ((NULL == self) || (NULL == self->strings) || (offset >= self->strings_size)) {
        return NULL;
    }
    const char * s = self->strings + offset;
    if (0 == strcmp(s, "__unknown__")) {
        return NULL;
    }
    return s;
}

static const char * table_get(const struct mbgen_s * self, const uint32_t * table,
                              uint32_t count, uint32_t idx) {
    if ((NULL == self) || (NULL == table) || (idx >= count)) {
        return NULL;
    }
    return str_get(self, table[idx]);
}

const char * mbgen_task_name(const struct mbgen_s * self, uint32_t task_id) {
    return (NULL == self) ? NULL : table_get(self, self->tasks, self->tasks_count, task_id);
}

const char * mbgen_isr_name(const struct mbgen_s * self, uint32_t irq_id) {
    return (NULL == self) ? NULL : table_get(self, self->irqs, self->irqs_count, irq_id);
}

const char * mbgen_file_name(const struct mbgen_s * self, uint32_t file_id) {
    return (NULL == self) ? NULL : table_get(self, self->files, self->files_count, file_id);
}

const char * mbgen_log_fmt(const struct mbgen_s * self, uint32_t file_id, uint32_t line) {
    if ((NULL == self) || (NULL == self->logs)) {
        return NULL;
    }
    uint32_t srcloc = (file_id << 16) | (line & 0xffff);
    uint32_t lo = 0;
    uint32_t hi = self->logs_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint32_t k = self->logs[mid * 2];
        if (k == srcloc) {
            return str_get(self, self->logs[mid * 2 + 1]);
        } else if (k < srcloc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static void append(char * dst, size_t dst_size, size_t * pos, const char * s, size_t s_len) {
    while (s_len-- && (*pos + 1) < dst_size) {
        dst[(*pos)++] = *s++;
    }
}

int mbgen_log_format(const struct mbgen_s * self, char * dst, size_t dst_size,
                     uint32_t file_id, uint32_t line,
                     const uint32_t * args, uint32_t arg_count) {
    size_t pos = 0;
    uint32_t argi = 0;
    char item[48];
    if (dst_size < 2) {
        if (dst_size) {
            dst[0] = 0;
        }
        return 1;
    }
    const char * fmt = mbgen_log_fmt(self, file_id, line);
    int rc = 0;
    if (NULL == fmt) {
        rc = 1;
        fmt = "";
    }
    while (*fmt && ((pos + 1) < dst_size)) {
        if (*fmt != '%') {
            dst[pos++] = *fmt++;
            continue;
        }
        const char * spec_start = fmt++;
        if (*fmt == '%') {
            dst[pos++] = '%';
            ++fmt;
            continue;
        }
        /* rebuild a sanitized specifier: flags, width, precision only */
        char spec[20];
        size_t spec_len = 0;
        spec[spec_len++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && (spec_len < 6)) {
            spec[spec_len++] = *fmt++;
        }
        while (isdigit((unsigned char) *fmt) && (spec_len < 9)) {
            spec[spec_len++] = *fmt++;
        }
        if (*fmt == '.') {
            spec[spec_len++] = *fmt++;
            while (isdigit((unsigned char) *fmt) && (spec_len < 13)) {
                spec[spec_len++] = *fmt++;
            }
        }
        while ((*fmt == 'l') || (*fmt == 'h') || (*fmt == 'z')) {
            ++fmt;  /* all trace arguments are u32; drop length modifiers */
        }
        char conv = *fmt;
        int consumed = 0;
        switch (conv) {
            case 'd': case 'i':
                spec[spec_len++] = 'd';
                spec[spec_len] = 0;
                if (argi < arg_count) {
                    snprintf(item, sizeof(item), spec, (int32_t) args[argi++]);
                    consumed = 1;
                }
                break;
            case 'u': case 'x': case 'X': case 'o': case 'c':
                spec[spec_len++] = conv;
                spec[spec_len] = 0;
                if (argi < arg_count) {
                    snprintf(item, sizeof(item), spec, args[argi++]);
                    consumed = 1;
                }
                break;
            case 'p':
                if (argi < arg_count) {
                    snprintf(item, sizeof(item), "0x%08x", args[argi++]);
                    consumed = 1;
                }
                break;
            default:
                break;  /* unsupported (%s, %f, ...) or missing arg: emit literally */
        }
        if (consumed) {
            append(dst, dst_size, &pos, item, strlen(item));
            ++fmt;
        } else if (conv) {
            append(dst, dst_size, &pos, spec_start, (size_t) (fmt - spec_start) + 1);
            ++fmt;
        } else {
            append(dst, dst_size, &pos, spec_start, (size_t) (fmt - spec_start));
        }
    }
    for (; argi < arg_count; ++argi) {
        snprintf(item, sizeof(item), "%s0x%08x", pos ? " " : "", args[argi]);
        append(dst, dst_size, &pos, item, strlen(item));
    }
    dst[pos] = 0;
    return rc;
}

int mbgen_log_line(const struct mbgen_s * self, char * dst, size_t dst_size,
                   uint32_t level, uint32_t file_id, uint32_t line,
                   const uint32_t * args, uint32_t arg_count) {
    /* level chars match enum mb_log_level_e / jsdrv_log_level_char */
    static const char LEVEL_CHARS[] = "!ACEWNIDDD";
    char level_char = (level < (sizeof(LEVEL_CHARS) - 1)) ? LEVEL_CHARS[level] : '?';
    const char * file = mbgen_file_name(self, file_id);
    size_t pos = 0;
    char prefix[64];
    if (NULL != file) {
        const char * basename = strrchr(file, '/');
        file = (NULL == basename) ? file : (basename + 1);
        snprintf(prefix, sizeof(prefix), "%c %s:%u ", level_char, file, line);
    } else {
        snprintf(prefix, sizeof(prefix), "%c %u:%u ", level_char, file_id, line);
    }
    append(dst, dst_size, &pos, prefix, strlen(prefix));
    if ((pos + 1) >= dst_size) {
        dst[pos] = 0;
        return 1;
    }
    return mbgen_log_format(self, dst + pos, dst_size - pos, file_id, line, args, arg_count);
}
