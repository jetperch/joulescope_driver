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
 * @brief Decode and display the mb_boot_s and public mb_personality_app_s
 *        structures read over c/sys (targets 1 and 2).
 *
 * The decoders are pure (no jsdrv dependency) and parse the raw little-endian
 * bytes by offset.  The boot structure must be parsed by offset because the
 * on-device layout uses a 32-bit personality pointer and a core-dependent
 * fault_registers[] array, neither of which match a host (64-bit) struct.
 *
 * References:
 *   minibitty/include/mb/boot.h        (struct mb_boot_s)
 *   minibitty/include/mb/personality.h (struct mb_personality_app_s)
 */

#ifndef MB_EXAMPLE_BOOT_INFO_H__
#define MB_EXAMPLE_BOOT_INFO_H__

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_INFO_MAGIC          (0x55AACC33U)
#define BOOT_INFO_HISTORY_MAX    (8U)
#define PERSONALITY_PUBLIC_SIZE  (76U)

struct boot_info_s {
    int valid;                       ///< 1 if decode succeeded (buffer large enough)
    uint32_t magic;
    uint32_t bootloader_version;
    uint32_t image_version[4];
    uint64_t command_arg0;
    uint8_t command;
    uint8_t command_result;
    uint8_t stable;
    uint8_t fault_registers_length;
    uint8_t history_length;
    uint8_t reset_cause;
    uint8_t fault_flags;
    uint32_t fault_location;
    uint32_t history[BOOT_INFO_HISTORY_MAX];
};

struct personality_info_s {
    int valid;                       ///< 1 if decode succeeded (buffer large enough)
    uint8_t format_version;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t subtype;
    uint8_t hw_ver;
    uint64_t hw_opts;
    int64_t mfg_time;
    char mfg_info[33];               ///< 32 bytes + NUL
    char serial_number[13];          ///< 12 bytes + NUL
    uint8_t app[8];
};

/**
 * @brief Decode the raw mb_boot_s bytes.
 * @param buf The raw little-endian bytes read from c/sys target 1.
 * @param len The number of valid bytes in buf.
 * @param info The decoded output.
 * @return 0 on success, 1 if the buffer is too small.
 */
int boot_info_decode(const uint8_t * buf, uint32_t len, struct boot_info_s * info);

/**
 * @brief Decode the public mb_personality_app_s bytes.
 * @param buf The raw little-endian bytes read from c/sys target 2.
 * @param len The number of valid bytes in buf.
 * @param info The decoded output.
 * @return 0 on success, 1 if the buffer is too small.
 */
int personality_info_decode(const uint8_t * buf, uint32_t len, struct personality_info_s * info);

/// Print the decoded boot structure as human-readable text.
void boot_info_print(FILE * f, const struct boot_info_s * info);

/// Print the decoded personality as human-readable text.
void personality_info_print(FILE * f, const struct personality_info_s * info);

#ifdef __cplusplus
}
#endif

#endif  /* MB_EXAMPLE_BOOT_INFO_H__ */
