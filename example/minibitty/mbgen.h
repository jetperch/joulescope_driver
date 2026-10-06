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

/**
 * @file
 *
 * @brief Load and query MiniBitty build debug metadata (mbgen.bin).
 *
 * The MiniBitty build system generates "mbgen.bin" for each target using
 * the "mb_dbg" format defined by minibitty's include/mb/dbg.h and written
 * by pyminibitty/dbg.py.  The file maps trace record ids to human-friendly
 * names: task id -> task name, IRQ id -> interrupt name, file id -> source
 * path, and (file_id, line) -> log format string.
 */

#ifndef JSDRV_EXAMPLE_MINIBITTY_MBGEN_H__
#define JSDRV_EXAMPLE_MINIBITTY_MBGEN_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mbgen_s {
    uint8_t * data;                 ///< The owned file contents.
    uint32_t size;                  ///< The file size in bytes.
    const uint32_t * files;         ///< String offsets indexed by file id.
    uint32_t files_count;
    const uint32_t * irqs;          ///< String offsets indexed by IRQ id.
    uint32_t irqs_count;
    const uint32_t * tasks;         ///< String offsets indexed by task id.
    uint32_t tasks_count;
    const uint32_t * logs;          ///< Sorted (srcloc, string offset) pairs.
    uint32_t logs_count;            ///< The number of pairs.
    const char * strings;           ///< The packed string pool.
    uint32_t strings_size;
};

/**
 * @brief Load and validate an mbgen.bin file.
 *
 * @param self The instance to populate.
 * @param path The mbgen.bin path.
 * @return 0 or error code.  On success, call mbgen_finalize() when done.
 */
int mbgen_load(struct mbgen_s * self, const char * path);

/**
 * @brief Free resources allocated by mbgen_load().
 *
 * @param self The instance.
 */
void mbgen_finalize(struct mbgen_s * self);

/**
 * @brief Look up a task name.
 *
 * @param self The instance.
 * @param task_id The trace record obj_id for a task object.
 * @return The task name or NULL when unknown.
 */
const char * mbgen_task_name(const struct mbgen_s * self, uint32_t task_id);

/**
 * @brief Look up an interrupt name.
 *
 * @param self The instance.
 * @param irq_id The trace record obj_id for an ISR object (NVIC IRQn).
 * @return The interrupt name or NULL when unknown.
 */
const char * mbgen_isr_name(const struct mbgen_s * self, uint32_t irq_id);

/**
 * @brief Look up a source file path.
 *
 * @param self The instance.
 * @param file_id The trace record file id.
 * @return The collapsed source path or NULL when unknown.
 */
const char * mbgen_file_name(const struct mbgen_s * self, uint32_t file_id);

/**
 * @brief Look up a log format string.
 *
 * @param self The instance.
 * @param file_id The trace record file id.
 * @param line The trace record line number.
 * @return The printf-style format string or NULL when unknown.
 */
const char * mbgen_log_fmt(const struct mbgen_s * self, uint32_t file_id, uint32_t line);

/**
 * @brief Format a log record into human-friendly text.
 *
 * @param self The instance.
 * @param dst The destination buffer.
 * @param dst_size The destination buffer size in bytes.
 * @param file_id The trace record file id.
 * @param line The trace record line number.
 * @param args The log arguments.
 * @param arg_count The number of log arguments.
 * @return 0 on success (format string found) or 1 when no format string
 *      exists; dst then contains the raw hex representation.
 *
 * Only integer conversions are substituted; the untrusted format string is
 * never passed to printf directly.  Unconsumed arguments are appended as hex.
 */
int mbgen_log_format(const struct mbgen_s * self, char * dst, size_t dst_size,
                     uint32_t file_id, uint32_t line,
                     const uint32_t * args, uint32_t arg_count);

/**
 * @brief Format a full log line: "{level char} {filename}:{line} {message}".
 *
 * @param self The instance.
 * @param dst The destination buffer.
 * @param dst_size The destination buffer size in bytes.
 * @param level The log level (enum mb_log_level_e: 0=emergency ... 9=debug3).
 * @param file_id The trace record file id.
 * @param line The trace record line number.
 * @param args The log arguments.
 * @param arg_count The number of log arguments.
 * @return 0 on success or 1 when no format string exists (raw hex message).
 *
 * The filename is the basename of the source path; the raw file id is used
 * when the file is not in the metadata.
 */
int mbgen_log_line(const struct mbgen_s * self, char * dst, size_t dst_size,
                   uint32_t level, uint32_t file_id, uint32_t line,
                   const uint32_t * args, uint32_t arg_count);

#ifdef __cplusplus
}
#endif

#endif  /* JSDRV_EXAMPLE_MINIBITTY_MBGEN_H__ */
