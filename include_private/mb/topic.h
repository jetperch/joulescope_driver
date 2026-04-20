/*
 * SPDX-FileCopyrightText: Copyright 2021-2024 Jetperch LLC
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
 * @brief Topic construction and manipulation.
 */

#ifndef MB_TOPIC_H__
#define MB_TOPIC_H__

#include "mb/cdef.h"
#include <stdint.h>

/**
 * @ingroup mb
 * @defgroup mb_topic Topic manipulation
 *
 * @brief Topic string utility functions.
 *
 * @{
 */

MB_CPP_GUARD_START

/// The maximum string length in bytes for a topic, including the null terminator.
#define MB_TOPIC_LENGTH_MAX (32U)

/// The maximum string length in bytes for each hierarchical topic level.
#define MB_TOPIC_LENGTH_PER_LEVEL (8U)

/// The maximum number of hierarchical topic levels.
#define MB_TOPIC_LEVELS_MAX (6U)

/// The topic hierarchy separator
#define MB_TOPIC_SEP                  '/'


/// The topic structure.
struct mb_topic_s {
    /// The topic string.
    char topic[MB_TOPIC_LENGTH_MAX];
    /// The length in bytes ignoring the null terminator.
    uint8_t length;
};

/// Empty topic structure initializer
#define MB_TOPIC_INIT ((struct mb_topic_s) {.topic={0}, .length=0})

/**
 * @brief Clear a topic structure instance to reset it to zero length.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 */
MB_INLINE_FN void mb_topic_clear(struct mb_topic_s * topic) {
    topic->topic[0] = 0;
    topic->length = 0;
}

/**
 * @brief Truncate a topic structure to a specified length.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param length The desired length.
 *
 * If you store length before calling mb_topic_append(), you can
 * use this function to revert the append.
 */
MB_INLINE_FN void mb_topic_truncate(struct mb_topic_s * topic, uint8_t length) {
    if (length < topic->length) {
        topic->topic[length] = 0;
        topic->length = length;
    }
}

/**
 * @brief Append a subtopic to a topic structure.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param subtopic The subtopic string to add.
 * @see mb_topic_remove
 *
 * This function intelligently adds the '/' separator.  If the topic
 * does not already end with '/', it will be inserted first.
 */
void mb_topic_append(struct mb_topic_s * topic, const char * subtopic);

/**
 * @brief Remove a subtopic from the end of a topic structure.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @return The number of characters removed.
 * @see mb_topic_append
 *
 * This function intelligently removes the '/' separator.  If the topic
 * already ends with '/', then this it is ignored.  This function then
 * removes the "/subtopic".  The resulting topic does NOT end in '/'.
 */
uint8_t mb_topic_remove(struct mb_topic_s * topic);

/**
 * @brief Set the topic to the provided value.
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param str The desired topic value.
 *
 * This function will assert if no room remains.
 */
void mb_topic_set(struct mb_topic_s * topic, const char * str);

/**
 * @brief Construct a topic string [convenience function].
 *
 * @param[inout] topic The topic structure, which is modified in place.
 * @param prefix The topic prefix, provided to mb_topic_set(topic, prefix).
 * @param suffix The topic suffix, provided to mb_topic_append(topix, suffix).
 * @return topic.topic for convenience.
 */
const char * mb_topic_make(struct mb_topic_s * topic, const char * prefix, const char * suffix);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_TOPIC_H__ */
