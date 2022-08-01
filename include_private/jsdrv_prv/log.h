/*
 * Copyright 2014-2021 Jetperch LLC
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

/*!
 * \file
 *
 * \brief Trivial logging support.
 */

#ifndef JSDRV_LOG_H_
#define JSDRV_LOG_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/log.h"

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_log Console logging
 *
 * @brief Generic console logging with compile-time levels.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @def JSDRV_LOG_GLOBAL_LEVEL
 *
 * @brief The global logging level.
 *
 * The maximum level to compile regardless of the individual module level.
 * This value should be defined in the project CMake (makefile).
 */
#ifndef JSDRV_LOG_GLOBAL_LEVEL
#define JSDRV_LOG_GLOBAL_LEVEL JSDRV_LOG_LEVEL_ALL
#endif

/**
 * @def JSDRV_LOG_LEVEL
 *
 * @brief The module logging level.
 *
 * Typical usage 1:  (not MISRA C compliant, but safe)
 *
 *      #define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_WARNING
 *      #include "log.h"
 */
#ifndef JSDRV_LOG_LEVEL
#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_INFO
#endif

/**
 * @def \_\_FILENAME\_\_
 *
 * @brief The filename to display for logging.
 *
 * When compiling C and C++ code, the __FILE__ define may contain a long path
 * that just confuses the log output.  The build tools, such as make and cmake,
 * can define __FILENAME__ to produce more meaningful results.
 *
 * A good Makefile usage includes:
 *
 */
#ifndef __FILENAME__
#define __FILENAME__ __FILE__
#endif

/**
 * @brief Publish a log message.
 *
 * @param level The fbp_log_level_e.
 * @param filename The source filename.
 * @param line The source line in filename.
 * @param format The printf-compatible format specification.
 * @param ... The formatting arguments.
 *
 * This implementation is thread safe.
 */
void jsdrv_log_publish(uint8_t level, const char * filename, uint32_t line, const char * format, ...);

/**
 * @brief The printf-style variadic arguments define to handle log messages.
 *
 * @param level The level for this log message
 * @param format The formatting string
 * @param ... The arguments for the formatting string
 */
#define JSDRV_LOG_PRINTF(level, format, ...) \
    jsdrv_log_publish(level, __FILENAME__, __LINE__, format, __VA_ARGS__)


/** Detailed messages for the software developer. */
#define JSDRV_LOG_LEVEL_DEBUG JSDRV_LOG_LEVEL_DEBUG1

/**
 * @brief Map log level to a string name.
 */
extern char const * const jsdrv_log_level_str[JSDRV_LOG_LEVEL_ALL + 1];

/**
 * @brief Map log level to a single character.
 */
extern char const jsdrv_log_level_char[JSDRV_LOG_LEVEL_ALL + 1];

/**
 * @brief Check the current level against the static logging configuration.
 *
 * @param level The level to query.
 * @return True if logging at level is permitted.
 */
#define JSDRV_LOG_CHECK_STATIC(level) ((level <= JSDRV_LOG_GLOBAL_LEVEL) && (level <= JSDRV_LOG_LEVEL) && (level >= 0))

/**
 * @brief Check a log level against a configured level.
 *
 * @param level The level to query.
 * @param cfg_level The configured logging level.
 * @return True if level is permitted given cfg_level.
 */
#define JSDRV_LOG_LEVEL_CHECK(level, cfg_level) (level <= cfg_level)

/*!
 * \brief Macro to log a printf-compatible formatted string.
 *
 * \param level The jsdrv_log_level_e.
 * \param format The printf-compatible formatting string.
 * \param ... The arguments to the formatting string.
 */
#define JSDRV_LOG(level, format, ...) do {            \
    if (JSDRV_LOG_CHECK_STATIC(level)) {              \
        JSDRV_LOG_PRINTF(level, format, __VA_ARGS__); \
    }                                               \
} while (0)


#ifdef _MSC_VER
/* Microsoft Visual Studio compiler support */
/** Log a emergency using printf-style arguments. */
#define JSDRV_LOG_EMERGENCY(format, ...)  JSDRV_LOG(JSDRV_LOG_LEVEL_EMERGENCY, format, __VA_ARGS__)
/** Log a alert using printf-style arguments. */
#define JSDRV_LOG_ALERT(format, ...)  JSDRV_LOG(JSDRV_LOG_LEVEL_ALERT, format, __VA_ARGS__)
/** Log a critical failure using printf-style arguments. */
#define JSDRV_LOG_CRITICAL(format, ...)  JSDRV_LOG(JSDRV_LOG_LEVEL_CRITICAL, format, __VA_ARGS__)
/** Log an error using printf-style arguments. */
#define JSDRV_LOG_ERROR(format, ...)     JSDRV_LOG(JSDRV_LOG_LEVEL_ERROR, format, __VA_ARGS__)
/** Log a warning using printf-style arguments. */
#define JSDRV_LOG_WARNING(format, ...)      JSDRV_LOG(JSDRV_LOG_LEVEL_WARNING, format, __VA_ARGS__)
/** Log a notice using printf-style arguments. */
#define JSDRV_LOG_NOTICE(format, ...)    JSDRV_LOG(JSDRV_LOG_LEVEL_NOTICE,   format, __VA_ARGS__)
/** Log an informative message using printf-style arguments. */
#define JSDRV_LOG_INFO(format, ...)      JSDRV_LOG(JSDRV_LOG_LEVEL_INFO,     format, __VA_ARGS__)
/** Log a detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG1(format, ...)    JSDRV_LOG(JSDRV_LOG_LEVEL_DEBUG1,    format, __VA_ARGS__)
/** Log a very detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG2(format, ...)    JSDRV_LOG(JSDRV_LOG_LEVEL_DEBUG2,  format, __VA_ARGS__)
/** Log an insanely detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG3(format, ...)    JSDRV_LOG(JSDRV_LOG_LEVEL_DEBUG3,  format, __VA_ARGS__)

#else
/* GCC compiler support */
// zero length variadic arguments are not allowed for macros
// this hack ensures that LOG(message) and LOG(format, args...) are both supported.
// https://stackoverflow.com/questions/5588855/standard-alternative-to-gccs-va-args-trick
#define _JSDRV_LOG_SELECT(PREFIX, _11, _10, _9, _8, _7, _6, _5, _4, _3, _2, _1, SUFFIX, ...) PREFIX ## _ ## SUFFIX
#define _JSDRV_LOG_1(level, message) JSDRV_LOG(level, "%s", message)
#define _JSDRV_LOG_N(level, format, ...) JSDRV_LOG(level, format, __VA_ARGS__)
#define _JSDRV_LOG_DISPATCH(level, ...)  _JSDRV_LOG_SELECT(_JSDRV_LOG, __VA_ARGS__, N, N, N, N, N, N, N, N, N, N, 1, 0)(level, __VA_ARGS__)

/** Log a emergency using printf-style arguments. */
#define JSDRV_LOG_EMERGENCY(...)  _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_EMERGENCY, __VA_ARGS__)
/** Log a alert using printf-style arguments. */
#define JSDRV_LOG_ALERT(...)  _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_ALERT, __VA_ARGS__)
/** Log a critical failure using printf-style arguments. */
#define JSDRV_LOG_CRITICAL(...)  _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_CRITICAL, __VA_ARGS__)
/** Log an error using printf-style arguments. */
#define JSDRV_LOG_ERROR(...)     _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_ERROR, __VA_ARGS__)
/** Log a warning using printf-style arguments. */
#define JSDRV_LOG_WARNING(...)      _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_WARNING, __VA_ARGS__)
/** Log a notice using printf-style arguments. */
#define JSDRV_LOG_NOTICE(...)    _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_NOTICE,   __VA_ARGS__)
/** Log an informative message using printf-style arguments. */
#define JSDRV_LOG_INFO(...)      _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_INFO,     __VA_ARGS__)
/** Log a detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG1(...)    _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_DEBUG1,    __VA_ARGS__)
/** Log a very detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG2(...)    _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_DEBUG2,  __VA_ARGS__)
/** Log an insanely detailed debug message using printf-style arguments. */
#define JSDRV_LOG_DEBUG3(...)    _JSDRV_LOG_DISPATCH(JSDRV_LOG_LEVEL_DEBUG3,  __VA_ARGS__)
#endif

/** Log an error using printf-style arguments.  Alias for JSDRV_LOG_ERROR. */
#define JSDRV_LOG_ERR JSDRV_LOG_ERROR
/** Log a warning using printf-style arguments.  Alias for JSDRV_LOG_WARNING. */
#define JSDRV_LOG_WARN JSDRV_LOG_WARNING
/** Log a detailed debug message using printf-style arguments.  Alias for JSDRV_LOG_DEBUG1. */
#define JSDRV_LOG_DEBUG JSDRV_LOG_DEBUG1
/** Log a detailed debug message using printf-style arguments.  Alias for JSDRV_LOG_DEBUG1. */
#define JSDRV_LOG_DBG JSDRV_LOG_DEBUG1

#define JSDRV_LOGE JSDRV_LOG_ERROR
#define JSDRV_LOGW JSDRV_LOG_WARNING
#define JSDRV_LOGN JSDRV_LOG_NOTICE
#define JSDRV_LOGI JSDRV_LOG_INFO
#define JSDRV_LOGD JSDRV_LOG_DEBUG1
#define JSDRV_LOGD1 JSDRV_LOG_DEBUG1
#define JSDRV_LOGD2 JSDRV_LOG_DEBUG2
#define JSDRV_LOGD3 JSDRV_LOG_DEBUG3

JSDRV_CPP_GUARD_END

/** @} */

#endif /* JSDRV_LOG_H_ */
