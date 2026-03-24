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
 * @brief Parse pubsub metadata binary blobs into per-topic JSON.
 */

#ifndef JSDRV_PRV_META_BINARY_H_
#define JSDRV_PRV_META_BINARY_H_

#include <stdint.h>

/**
 * @brief Callback for each topic's JSON metadata.
 *
 * @param user_data The user data from meta_binary_parse.
 * @param topic The topic path (e.g. "././info", "./app/fw/version").
 * @param json_meta The JSON metadata string.
 */
typedef void (*meta_binary_on_topic_fn)(
    void * user_data,
    const char * topic,
    const char * json_meta);

/**
 * @brief Parse a pubsub metadata binary blob.
 *
 * @param blob The binary blob data.
 * @param blob_size The blob data size in bytes.
 * @param on_topic Callback invoked for each topic with JSON metadata.
 * @param user_data Passed to on_topic callback.
 * @return 0 or error code.
 */
int32_t meta_binary_parse(
    const uint8_t * blob, uint32_t blob_size,
    meta_binary_on_topic_fn on_topic,
    void * user_data);

#endif  /* JSDRV_PRV_META_BINARY_H_ */
