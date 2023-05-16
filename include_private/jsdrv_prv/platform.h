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

#ifndef JSDRV_PLATFORM_DEPENDENCIES_H_
#define JSDRV_PLATFORM_DEPENDENCIES_H_

/**
 * @file
 *
 * @brief Platform
 */

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if _WIN32
#include "jsdrv_prv/windows.h"
#else
#include <poll.h>
#include <unistd.h>
#endif

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_platform platform
 *
 * @brief Platform dependencies that must be defined for each target platform.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @brief Handle a fatal condition.
 *
 * @param msg The fatal error message.
 */
#define JSDRV_FATAL(msg) jsdrv_fatal(__FILE__, __LINE__, msg)

/**
 * @brief Handle a fatal condition.
 *
 * @param file The file name.
 * @param line The line number in the file.
 * @param msg The fatal error message.
 * @see JSDRV_FATAL
 *
 * This function may not return.
 * Implementation is thread safe and may be called from any
 * JSDRV thread or an external thread.
 */
void jsdrv_fatal(const char * file, uint32_t line, const char * msg);

/**
 * @brief Fill the first num total_bytes of the memory buffer to value.
 *
 * @param ptr The memory buffer to fill.
 * @param value The new value for each byte.
 * @param num The number of total_bytes to fill.
 */
JSDRV_INLINE_FN void jsdrv_memset(void * ptr, int value, size_t num) {
    memset(ptr, value, num);
}

/**
 * @brief Copy data from one buffer to another.
 *
 * @param destination The destination buffer.
 * @param source The source buffer.
 * @param num The number of total_bytes to copy.
 *
 * The buffers destination and source must not overlap or the buffers may
 * be corrupted by this function!
 */
JSDRV_INLINE_FN void jsdrv_memcpy(void * destination, void const * source, size_t num) {
    memcpy(destination, source, num);
}

/**
 * \brief Function to deallocate memory provided by jsdrv_alloc() or jsdrv_alloc_clr().
 *
 * \param ptr The pointer to the memory to free.
 */
void jsdrv_free(void * ptr);

/**
 * @brief Allocate memory from the heap.
 *
 * @param size_bytes The number of total_bytes to allocate.
 * @return The pointer to the allocated memory.
 *
 * This function will assert on out of memory conditions.
 * For platforms that support freeing memory, use jsdrv_free() to return the
 * memory to the heap.
 */
JSDRV_COMPILER_ALLOC(jsdrv_free) void * jsdrv_alloc(size_t size_bytes);

/**
 * @brief Allocate memory from the heap and clear to 0.
 *
 * @param size_bytes The number of total_bytes to allocate.
 * @return The pointer to the allocated memory.
 *
 * This function will assert on out of memory conditions.
 * For platforms that support freeing memory, use jsdrv_free() to return the
 * memory to the heap.
 */
JSDRV_COMPILER_ALLOC(jsdrv_free) JSDRV_INLINE_FN void * jsdrv_alloc_clr(size_t size_bytes) {
    void * ptr = jsdrv_alloc(size_bytes);
    jsdrv_memset(ptr, 0, size_bytes);
    return ptr;
}

/**
 * @brief Get the UTC time as a 34Q30 fixed point number.
 *
 * @return The current time.  This value is not guaranteed to be monotonic.
 *      The device may synchronize to external clocks which can cause
 *      discontinuous jumps, both backwards and forwards.
 *
 *      At power-on, the time will start from 0 unless the system has
 *      a real-time clock.  When the current time first synchronizes to
 *      an external host, it may have a large skip.
 *
 * Be sure to verify your time for each platform using python:
 *
 *      python
 *      import datetime
 *      import dateutil.parser
 *      epoch = dateutil.parser.parse('2018-01-01T00:00:00Z').timestamp()
 *      datetime.datetime.fromtimestamp((my_time >> 30) + epoch)
 */
JSDRV_API int64_t jsdrv_time_utc(void);

/**
 * @brief Get a monotonically incrementing counter in milliseconds.
 *
 * @return The current tick value.  This rolls over every 49 days.
 */
JSDRV_API uint32_t jsdrv_time_ms_u32(void);

/**
 * @brief Initialize any platform-specific features.
 *
 * @return 0 or error code.
 */
int32_t jsdrv_platform_initialize(void);

/**
 * Additional functions to define:
 *
 * mutex.h
 * - jsdrv_os_mutex_t:        typedef in config.h
 * - jsdrv_os_mutex_alloc()
 * - jsdrv_os_mutex_free()
 * - jsdrv_os_mutex_lock()
 * - jsdrv_os_mutex_unlock()
 */


JSDRV_CPP_GUARD_END

/** @} */

#endif /* JSDRV_PLATFORM_DEPENDENCIES_H_ */
