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
 * @brief Joulescope host driver logging facility.
 */

#ifndef JSDRV_LOG_INCLUDE_H_
#define JSDRV_LOG_INCLUDE_H_

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

#ifndef JSDRV_LOG_FILENAME_SIZE_MAX
/// The filename maximum size, including the null terminator character.
#define JSDRV_LOG_FILENAME_SIZE_MAX (1024)
#endif

#ifndef JSDRV_LOG_MESSAGE_SIZE_MAX
/// The log message maximum size, including the null terminator character.
#define JSDRV_LOG_MESSAGE_SIZE_MAX (1024)
#endif

/// The record format version.
#define JSDRV_LOG_VERSION  (1)

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_log Logging
 *
 * @brief The logging facility.
 *
 * @{
 */
JSDRV_CPP_GUARD_START

/**
 * @brief The available logging levels.
 */
enum jsdrv_log_level_e {
    /** Logging functionality is disabled. */
    JSDRV_LOG_LEVEL_OFF         = -1,
    /** A "panic" condition that may result in significant harm. */
    JSDRV_LOG_LEVEL_EMERGENCY   = 0,
    /** A condition requiring immediate action. */
    JSDRV_LOG_LEVEL_ALERT       = 1,
    /** A critical error which prevents further functions. */
    JSDRV_LOG_LEVEL_CRITICAL    = 2,
    /** An error which prevents the current operation from completing or
     *  will adversely effect future functionality. */
    JSDRV_LOG_LEVEL_ERROR       = 3,
    /** A warning which may adversely affect the current operation or future
     *  operations. */
    JSDRV_LOG_LEVEL_WARNING     = 4,
    /** A notification for interesting events. */
    JSDRV_LOG_LEVEL_NOTICE      = 5,
    /** An informative message. */
    JSDRV_LOG_LEVEL_INFO        = 6,
    /** Detailed messages for the software developer. */
    JSDRV_LOG_LEVEL_DEBUG1      = 7,
    /** Very detailed messages for the software developer. */
    JSDRV_LOG_LEVEL_DEBUG2      = 8,
    /** Insanely detailed messages for the software developer. */
    JSDRV_LOG_LEVEL_DEBUG3      = 9,
    /** All logging functionality is enabled. */
    JSDRV_LOG_LEVEL_ALL         = 10,
};

/**
 * @brief The log record header format.
 */
struct jsdrv_log_header_s {
    /// The JSDRV_LOG_VERSION log message format major version.
    uint8_t version;
    /// The jsdrv_log_level_e.
    uint8_t level;
    uint8_t rsvu8_1;    ///< reserved u8 for future use
    uint8_t rsvu8_2;    ///< reserved u8 for future use
    /// The originating file line number number.
    uint32_t line;
    /// The Joulescope driver UTC time.
    int64_t timestamp;
};

/**
 * @brief Receive a log message.
 *
 * @param user_data The arbitrary user data.
 * @param header The log record header.
 * @param filename The log record filename.
 * @param message The log message.
 * @see jsdrv_log_register().
 *
 * All parameters only remain valid for the duration of the call.
 * The caller retains ownership of all parameters.
 *
 * This function will be called from the singleton log handler thread.
 * This function is responsible for any synchronization, as needed.
 */
typedef void (*jsdrv_log_recv)(void * user_data, struct jsdrv_log_header_s const * header,
                               const char * filename, const char * message);

/**
 * @brief Publish a new log message.
 *
 * @param level The logging level.
 * @param filename The source filename.
 * @param line The source line number.
 * @param format The formatting string for the arguments.
 * @param ... The arguments to format.
 *
 * This function is thread-safe.
 */
JSDRV_API void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...);

/**
 * @brief Register a callback for log message dispatch.
 *
 * @param fn A function to call on a received log message.
 *      This function will be called from within the logging thread.
 * @param user_data The arbitrary user data for fn.
 * @return 0 or error code:
 *      - JSDRV_ERROR_UNAVAILABLE if jsdrv_log_initialize was not yet called.
 */
JSDRV_API int32_t jsdrv_log_register(jsdrv_log_recv fn, void * user_data);

/**
 * @brief Unregister a callback.
 *
 * @param fn The function previous registered jsdrv_log_register().
 * @param user_data The same arbitrary data provided to jsdrv_log_register().
 * @return 0 or JSDRV_ERROR_NOT_FOUND.
 */
JSDRV_API int32_t jsdrv_log_unregister(jsdrv_log_recv fn, void * user_data);

/**
 * @brief Dynamically set the maximum log level.
 *
 * @param level The maximum jsdrv_log_level_e to process to callbacks.
 *
 * The level initializes to OFF, so you need to call this function
 * first into order to process log messages.  Also, ensure that
 * JSDRV_LOG_GLOBAL_LEVEL is set correctly.  Any messages higher than
 * this level will not be processed, regardless of what you
 * pass to this function.
 */
JSDRV_API void jsdrv_log_level_set(int8_t level);

/**
 * @brief Get the current maximum log level.
 *
 * @return The maximum jsdrv_log_level_e to process to callbacks.
 */
JSDRV_API int8_t jsdrv_log_level_get();

/**
 * @brief Initialize the singleton log handler.
 *
 * This function allocates memory and starts a task.
 * This function may be called multiple times.  To shut
 * down gracefully, each call to jsdrv_log_initialize()
 * should be matched with a corresponding call to
 * jsdrv_log_finalize().  The implementation uses reference
 * counting.
 *
 * Note that the log services ALL active jsdrv_context_s instances.
 */
JSDRV_API void jsdrv_log_initialize();

/**
 * @brief Finalize the singleton log handler.
 *
 * This function releases resources based upon reference counting.
 */
JSDRV_API void jsdrv_log_finalize();

/**
 * @brief Convert a log level to a user-meaningful string description.
 *
 * @param level The log level.
 * @return The string description.
 */
JSDRV_API const char * jsdrv_log_level_to_str(int8_t level);

/**
 * @brief Convert a log level to a user-meaningful character.
 *
 * @param level The log level.
 * @return The character representing the log level.
 */
JSDRV_API char jsdrv_log_level_to_char(int8_t level);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_LOG_INCLUDE_H_ */
