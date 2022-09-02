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

#include "jsdrv/topic.h"
#include "jsdrv_prv/assert.h"

void jsdrv_topic_clear(struct jsdrv_topic_s * topic) {
    topic->topic[0] = 0;
    topic->length = 0;
}

/**
 * @brief Truncate a topic structure to a specified length.
 *
 * @param topic[inout] The topic structure, which is modified in place.
 * @param length The desired length.
 *
 * If you store length before calling jsdrv_topic_append(), you can
 * use this function to revert the append.
 */
void jsdrv_topic_truncate(struct jsdrv_topic_s * topic, uint8_t length) {
    if (length < topic->length) {
        topic->topic[length] = 0;
        topic->length = length;
    }
}

void jsdrv_topic_append(struct jsdrv_topic_s * topic, const char * subtopic) {
    char * topic_end = &topic->topic[JSDRV_TOPIC_LENGTH_MAX];
    if (topic->length && ('/' != topic->topic[topic->length -1])) {
        topic->topic[topic->length++] = '/';
    }

    char * t = &topic->topic[topic->length];
    while (*subtopic && (t < topic_end)) {
        char c = *subtopic++;
        *t++ = c;
    }
    JSDRV_ASSERT(t < topic_end);
    *t = 0;
    topic->length = (uint8_t) (t - topic->topic);
}

int32_t jsdrv_topic_remove(struct jsdrv_topic_s * topic) {
    int32_t rv = 0;
    if ((topic->length > 0) && (topic->topic[topic->length - 1] == '/')) {
        topic->topic[topic->length - 1] = 0;
        --topic->length;
        ++rv;
    }
    while (topic->length) {
        char * c = &topic->topic[topic->length - 1];
        char ch = *c;
        ++rv;
        *c = 0;
        --topic->length;
        if (ch == '/') {
            break;
        }
    }
    return rv;
}

void jsdrv_topic_set(struct jsdrv_topic_s * topic, const char * str) {
    jsdrv_topic_clear(topic);
    while (*str && (topic->length < JSDRV_TOPIC_LENGTH_MAX)) {
        topic->topic[topic->length++] = *str++;
    }
    JSDRV_ASSERT(topic->length < JSDRV_TOPIC_LENGTH_MAX);
    topic->topic[topic->length] = 0;
}

bool is_suffix_char(char ch) {
    switch (ch) {
        case JSDRV_TOPIC_SUFFIX_METADATA_REQ:    /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_METADATA_RSP:    /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_QUERY_REQ:       /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_QUERY_RSP:       /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_RETURN_CODE:     /** intentional fall-through */
            return true;
        default:
            return false;
    }
}


void jsdrv_topic_suffix_add(struct jsdrv_topic_s * topic, char ch) {
    JSDRV_ASSERT(topic->length < (JSDRV_TOPIC_LENGTH_MAX - 1));
    JSDRV_ASSERT(is_suffix_char(ch));
    topic->topic[topic->length++] = ch;
    topic->topic[topic->length] = 0;
}

char jsdrv_topic_suffix_remove(struct jsdrv_topic_s * topic) {
    char ch = 0;
    if (!topic->length) {
        return ch;
    }
    if (is_suffix_char(topic->topic[topic->length - 1])) {
        ch = topic->topic[topic->length - 1];
        topic->topic[topic->length - 1] = 0;
        topic->length--;
    }
    return ch;
}
