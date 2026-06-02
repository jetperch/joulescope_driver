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

#define _CRT_SECURE_NO_WARNINGS  // allow gmtime() on MSVC
#include "boot_info.h"
#include <string.h>
#include <inttypes.h>
#include <time.h>

// minibitty/jsdrv time64: signed int64, 2^-30 s, epoch 2018-01-01T00:00:00Z.
// See joulescope_driver/include/jsdrv/time.h (JSDRV_TIME_Q,
// JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS).
#define MB_TIME64_Q             30
#define MB_TIME64_EPOCH_UNIX    1514764800LL

// mb_boot_reset_cause_e (minibitty/include/mb/boot.h)
static const char * const RESET_CAUSE_NAMES[8] = {
    "POWER_ON", "BROWN_OUT", "HARDWARE_RESET", "SOFTWARE_RESET",
    "WATCHDOG", "RSV5", "HARDWARE_FAULT", "SOFTWARE_FAULT",
};

// mb_boot_cmd_e operation = (command >> 2) & 3 (minibitty/include/mb/boot.h)
static const char * const CMD_OP_NAMES[4] = {
    "LAUNCH", "UPDATE", "(invalid)", "ERASE",
};

#define MB_BOOT_CMD_VALID          (0x80U)
#define MB_BOOT_RESET_CAUSE_VALID  (0x80U)
#define MB_BOOT_FAULT_FLAG_LOCATION  (1U << 0)
#define MB_BOOT_FAULT_FLAG_REGISTERS (1U << 1)


static uint32_t rd_u32(const uint8_t * p) {
    return ((uint32_t) p[0]) | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t rd_u64(const uint8_t * p) {
    return ((uint64_t) rd_u32(p)) | ((uint64_t) rd_u32(p + 4) << 32);
}

static uint16_t rd_u16(const uint8_t * p) {
    return ((uint16_t) p[0]) | ((uint16_t) p[1] << 8);
}


int boot_info_decode(const uint8_t * buf, uint32_t len, struct boot_info_s * info) {
    memset(info, 0, sizeof(*info));
    // Fixed prefix: magic..fault_location = 11 u32 words = 44 bytes.
    if ((NULL == buf) || (len < 44)) {
        return 1;
    }
    info->magic = rd_u32(buf + 0);
    info->bootloader_version = rd_u32(buf + 4);
    for (int i = 0; i < 4; ++i) {
        info->image_version[i] = rd_u32(buf + 8 + i * 4);
    }
    info->command_arg0 = rd_u64(buf + 24);
    uint32_t w8 = rd_u32(buf + 32);
    info->command = (uint8_t) (w8 & 0xFF);
    info->command_result = (uint8_t) ((w8 >> 8) & 0xFF);
    info->stable = (uint8_t) ((w8 >> 16) & 0xFF);
    uint32_t w9 = rd_u32(buf + 36);
    info->fault_registers_length = (uint8_t) (w9 & 0xFF);
    info->history_length = (uint8_t) ((w9 >> 8) & 0xFF);
    info->reset_cause = (uint8_t) ((w9 >> 16) & 0xFF);
    info->fault_flags = (uint8_t) ((w9 >> 24) & 0xFF);
    info->fault_location = rd_u32(buf + 40);

    // history[] follows fault_registers[fault_registers_length] and the
    // 32-bit personality pointer: word index 11 + fault_registers_length + 1.
    uint32_t hist_word = 11u + info->fault_registers_length + 1u;
    uint32_t n = info->history_length;
    if (n > BOOT_INFO_HISTORY_MAX) {
        n = BOOT_INFO_HISTORY_MAX;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t word = hist_word + i;
        uint32_t off = word * 4u;
        if ((off + 4u) > len) {
            break;
        }
        info->history[i] = rd_u32(buf + off);
    }
    info->valid = 1;
    return 0;
}

int personality_info_decode(const uint8_t * buf, uint32_t len, struct personality_info_s * info) {
    memset(info, 0, sizeof(*info));
    if ((NULL == buf) || (len < PERSONALITY_PUBLIC_SIZE)) {
        return 1;
    }
    info->format_version = buf[0];
    info->vendor_id = rd_u16(buf + 2);
    info->product_id = rd_u16(buf + 4);
    info->subtype = buf[6];
    info->hw_ver = buf[7];
    info->hw_opts = rd_u64(buf + 8);
    info->mfg_time = (int64_t) rd_u64(buf + 16);
    memcpy(info->mfg_info, buf + 24, 32);
    info->mfg_info[32] = 0;
    memcpy(info->serial_number, buf + 56, 12);
    info->serial_number[12] = 0;
    memcpy(info->app, buf + 68, 8);
    info->valid = 1;
    return 0;
}


static const char * reset_cause_name(uint8_t cause3) {
    return RESET_CAUSE_NAMES[cause3 & 0x7];
}

void boot_info_print(FILE * f, const struct boot_info_s * info) {
    fprintf(f, "Boot:\n");
    if (!info->valid) {
        fprintf(f, "    <unavailable or too short>\n");
        return;
    }
    fprintf(f, "    magic              = 0x%08" PRIX32 "%s\n", info->magic,
            (info->magic == BOOT_INFO_MAGIC) ? "" : "  [MISMATCH!]");
    fprintf(f, "    bootloader_version = %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
            info->bootloader_version >> 24,
            (info->bootloader_version >> 16) & 0xFF,
            info->bootloader_version & 0xFFFF);
    // Version format is major8.minor8.patch16.  0xFFFFFFFF means the slot has
    // no valid image; 0 (0.0.0) means a valid image whose version was not set.
    for (int i = 0; i < 4; ++i) {
        uint32_t v = info->image_version[i];
        if (v == 0xFFFFFFFFU) {
            fprintf(f, "    image_version[%d]   = none (no valid image)\n", i);
        } else {
            fprintf(f, "    image_version[%d]   = %" PRIu32 ".%" PRIu32 ".%" PRIu32 "%s\n",
                    i, v >> 24, (v >> 16) & 0xFF, v & 0xFFFF,
                    (v == 0) ? "  (not set)" : "");
        }
    }
    fprintf(f, "    command_arg0       = 0x%016" PRIX64 "\n", info->command_arg0);
    if (info->command & MB_BOOT_CMD_VALID) {
        fprintf(f, "    command            = 0x%02X (pending: %s image %u)\n",
                info->command, CMD_OP_NAMES[(info->command >> 2) & 3], info->command & 3);
    } else {
        fprintf(f, "    command            = 0x%02X (none pending)\n", info->command);
    }
    fprintf(f, "    command_result     = %u%s\n", info->command_result,
            (info->command_result == 0) ? " (SUCCESS)" : "");
    fprintf(f, "    stable             = %u\n", info->stable);
    if (info->reset_cause & MB_BOOT_RESET_CAUSE_VALID) {
        uint8_t cause = (info->reset_cause >> 4) & 0x7;
        fprintf(f, "    reset_cause        = %s (index %u)\n",
                reset_cause_name(cause), info->reset_cause & 0x0F);
    } else {
        fprintf(f, "    reset_cause        = 0x%02X (invalid)\n", info->reset_cause);
    }
    fprintf(f, "    fault_flags        = 0x%02X%s%s\n", info->fault_flags,
            (info->fault_flags & MB_BOOT_FAULT_FLAG_LOCATION) ? " LOCATION" : "",
            (info->fault_flags & MB_BOOT_FAULT_FLAG_REGISTERS) ? " REGISTERS" : "");
    if (info->fault_flags & MB_BOOT_FAULT_FLAG_LOCATION) {
        fprintf(f, "    fault_location     = 0x%08" PRIX32 "\n", info->fault_location);
    }
    fprintf(f, "    history (%u entries, newest first):\n", info->history_length);
    uint32_t n = info->history_length;
    if (n > BOOT_INFO_HISTORY_MAX) {
        n = BOOT_INFO_HISTORY_MAX;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t e = info->history[i];
        if (!((e >> 31) & 1)) {
            fprintf(f, "        [%u] 0x%08" PRIX32 "  invalid\n", i, e);
            continue;
        }
        uint8_t rc_byte = (uint8_t) ((e >> 8) & 0xFF);
        fprintf(f, "        [%u] 0x%08" PRIX32 "  unstable=%u resolved=%u launched=%u "
                   "fault=0x%02X reset=%s\n",
                i, e,
                (e >> 30) & 1,
                (e >> 26) & 3,
                (e >> 24) & 3,
                (e >> 16) & 0xFF,
                reset_cause_name((rc_byte >> 4) & 0x7));
    }
}

void personality_info_print(FILE * f, const struct personality_info_s * info) {
    fprintf(f, "Personality:\n");
    if (!info->valid) {
        fprintf(f, "    <unavailable or too short>\n");
        return;
    }
    fprintf(f, "    format_version     = %u\n", info->format_version);
    fprintf(f, "    vendor_id          = 0x%04X\n", info->vendor_id);
    fprintf(f, "    product_id         = 0x%04X\n", info->product_id);
    fprintf(f, "    subtype            = %u\n", info->subtype);
    fprintf(f, "    hw_ver             = %u\n", info->hw_ver);
    fprintf(f, "    hw_opts            = 0x%016" PRIX64 "\n", info->hw_opts);
    if (info->mfg_time <= 0) {
        fprintf(f, "    mfg_time           = not set (%" PRId64 ")\n", info->mfg_time);
    } else {
        time_t t = (time_t) ((info->mfg_time >> MB_TIME64_Q) + MB_TIME64_EPOCH_UNIX);
        struct tm * tm_utc = gmtime(&t);
        char tbuf[32];
        if (tm_utc && strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S UTC", tm_utc)) {
            fprintf(f, "    mfg_time           = %s\n", tbuf);
        } else {
            fprintf(f, "    mfg_time           = %" PRId64 "\n", info->mfg_time);
        }
    }
    fprintf(f, "    mfg_info           = \"%s\"\n", info->mfg_info);
    fprintf(f, "    serial_number      = \"%s\"\n", info->serial_number);
    fprintf(f, "    app                = %02X %02X %02X %02X %02X %02X %02X %02X\n",
            info->app[0], info->app[1], info->app[2], info->app[3],
            info->app[4], info->app[5], info->app[6], info->app[7]);
}
