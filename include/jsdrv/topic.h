/*
 * Copyright 2021 Jetperch LLC
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
 * @brief Topic construction and manipulation.
 */

#ifndef JSDRV_TOPIC_H__
#define JSDRV_TOPIC_H__

#include "jsdrv.h"
#include <stdint.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_topic Topic manipulation
 *
 * @brief Topic string utility functions.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/// The topic structure.
struct jsdrv_topic_s {
    /// The topic string.
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    /// The length in bytes ignoring the null terminator.
    uint8_t length;
};

/// Empty topic structure initializer
#define JSDRV_TOPIC_INIT ((struct jsdrv_topic_s) {.topic={0}, .length=0})

/**
 * @brief Clear a topic structure instance to reset it to zero length.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 */
JSDRV_API void jsdrv_topic_clear(struct jsdrv_topic_s * topic);

/**
 * @brief Truncate a topic structure to a specified length.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param length The desired length.
 *
 * If you store length before calling jsdrv_topic_append(), you can
 * use this function to revert the append.
 */
JSDRV_API void jsdrv_topic_truncate(struct jsdrv_topic_s * topic, uint8_t length);

/**
 * @brief Append a subtopic to a topic structure.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param subtopic The subtopic string to add.
 * @see jsdrv_topic_remove
 *
 * This function intelligently adds the '/' separator.  If the topic
 * does not already end with '/', it will be inserted first.
 */
JSDRV_API void jsdrv_topic_append(struct jsdrv_topic_s * topic, const char * subtopic);

/**
 * @brief Remove a subtopic from the end of a topic structure.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @return The number of characters removed.
 * @see jsdrv_topic_append
 *
 * This function intelligently removes the '/' separator.  If the topic
 * already ends with '/', then this it is ignored.  This function then
 * removes the "/subtopic".  The resulting topic does NOT end in '/'.
 */
JSDRV_API int32_t jsdrv_topic_remove(struct jsdrv_topic_s * topic);

/**
 * @brief Set the topic to the provided value.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param str The desired topic value.
 *
 * This function will assert if no room remains.
 */
JSDRV_API void jsdrv_topic_set(struct jsdrv_topic_s * topic, const char * str);

/**
 * @brief Adds a suffix character to the topic.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param ch The special character to append.
 *
 * This function will assert if no room remains.
 */
JSDRV_API void jsdrv_topic_suffix_add(struct jsdrv_topic_s * topic, char ch);

/**
 * @brief Remove the suffix character from a topic.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @return ch The suffix character removed or 0 if no character was removed.
 */
JSDRV_API char jsdrv_topic_suffix_remove(struct jsdrv_topic_s * topic);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_TOPIC_H__ */
