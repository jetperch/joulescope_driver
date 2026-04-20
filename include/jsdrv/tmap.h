/*
* Copyright 2023-2026 Jetperch LLC
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
 * @brief JLS timestamp <-> sample_id mapping for FSR channels.
 */

#ifndef JSDRV_TMAP_H_
#define JSDRV_TMAP_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/time.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_tmap Time map between sample and UTC times.
 *
 * @brief Map UTC time to sample time and back.
 *
 * This implementation is used by buffers to maintain a fixed
 * mapping for all samples in a buffer.  Once a time is assigned to a
 * sample, this implementation assures it does not change.
 *
 * The Joulescope UI creates annotations with UTC time.  This implementation
 * ensures that samples remained aligned with the annotations.  Using
 * a simple jsdrv_time_map_s of the most recent data causes
 * older samples to "jump around" with respect to UTC.
 *
 * Each tmap instance is single-owner.  The writer holds one instance
 * and mutates it freely.  To publish a stable view to a consumer, the
 * writer uses jsdrv_tmap_copy() to produce an independent snapshot.
 * The consumer owns that snapshot and must call jsdrv_tmap_free()
 * when finished.  There is no sharing and no locking.
 *
 * Writers use: jsdrv_tmap_alloc(), jsdrv_tmap_add(),
 * jsdrv_tmap_expire_by_sample_id(), jsdrv_tmap_clear(),
 * jsdrv_tmap_copy() (to produce a snapshot to publish), and
 * jsdrv_tmap_free().
 *
 * Consumers use: jsdrv_tmap_length(), jsdrv_tmap_get(),
 * jsdrv_tmap_sample_id_to_timestamp(),
 * jsdrv_tmap_timestamp_to_sample_id(), and jsdrv_tmap_free().
 *
 *
 * References:
 *   - https://github.com/jetperch/jls/blob/main/include_prv/jls/tmap.h
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/// The opaque instance.
struct jsdrv_tmap_s;

// --- Lifecycle ---

/**
 * @brief Allocate a new tmap instance.
 *
 * @param initial_size The initial allocation size.  0 to use default.
 * @return The new tmap instance.
 *
 * The returned instance is single-owner.  Free with jsdrv_tmap_free().
 */
JSDRV_API struct jsdrv_tmap_s * jsdrv_tmap_alloc(size_t initial_size);

/**
 * @brief Free a tmap instance.
 *
 * @param self The instance.  May be NULL.
 *
 * After this call, the caller must not use the instance pointer.
 */
JSDRV_API void jsdrv_tmap_free(struct jsdrv_tmap_s * self);

/**
 * @brief Allocate an independent copy of a tmap instance.
 *
 * @param src The source instance to copy.  May be NULL, in which case
 *      NULL is returned.
 * @return A new tmap instance with the same entries as src, or NULL if
 *      src is NULL.  The caller owns the returned instance and must
 *      free it with jsdrv_tmap_free().
 *
 * The copy is independent: subsequent mutations to src do not affect
 * the copy, and mutations to the copy do not affect src.
 */
JSDRV_API struct jsdrv_tmap_s * jsdrv_tmap_copy(const struct jsdrv_tmap_s * src);

// --- Mutation (writer path) ---

/**
 * @brief Add a new time map entry to this instance.
 *
 * @param self The instance.  May be NULL (no-op).
 * @param time_map The new time map.  May be NULL (no-op).
 */
JSDRV_API void jsdrv_tmap_add(struct jsdrv_tmap_s * self, const struct jsdrv_time_map_s * time_map);

/**
 * @brief Expire old time map entries.
 *
 * @param self The instance.  May be NULL (no-op).
 * @param sample_id The oldest sample_id that this instance needs to represent.
 */
JSDRV_API void jsdrv_tmap_expire_by_sample_id(struct jsdrv_tmap_s * self, uint64_t sample_id);

/**
 * @brief Clear all data from this instance.
 *
 * @param self The instance.  May be NULL (no-op).
 */
JSDRV_API void jsdrv_tmap_clear(struct jsdrv_tmap_s * self);

// --- Query (consumer path) ---

/**
 * @brief Get the current number of entries in the time map.
 *
 * @param self The instance.  May be NULL (returns 0).
 * @return The number of entries in this instance.
 */
JSDRV_API size_t jsdrv_tmap_length(struct jsdrv_tmap_s * self);

/**
 * @brief Map sample id to a UTC timestamp.
 *
 * @param self The instance.
 * @param sample_id The sample id to map.
 * @param[out] timestamp The timestamp corresponding to sample id.
 * @return 0 on success or JSDRV_ERROR_UNAVAILABLE if the instance is empty.
 *
 * For sample_id values outside the range of stored entries, the result
 * is clamped to the nearest endpoint (oldest or newest) entry.  Only an
 * empty instance produces an error.
 */
JSDRV_API int32_t jsdrv_tmap_sample_id_to_timestamp(struct jsdrv_tmap_s * self, uint64_t sample_id, int64_t * timestamp);

/**
 * @brief Map UTC timestamp to a sample id.
 *
 * @param self The instance.
 * @param timestamp The timestamp to map.
 * @param[out] sample_id The sample id corresponding to timestamp.
 * @return 0 on success or JSDRV_ERROR_UNAVAILABLE if the instance is empty.
 *
 * For timestamp values outside the range of stored entries, the result
 * is clamped to the nearest endpoint (oldest or newest) entry.  Only an
 * empty instance produces an error.
 */
JSDRV_API int32_t jsdrv_tmap_timestamp_to_sample_id(struct jsdrv_tmap_s * self, int64_t timestamp, uint64_t * sample_id);

/**
 * @brief Get an entry from the tmap instance.
 *
 * @param self The instance.  May be NULL (returns JSDRV_ERROR_UNAVAILABLE).
 * @param index The entry index. 0 is oldest.  jsdrv_tmap_length() - 1 is newest.
 * @param[out] time_map The entry for the corresponding index.
 * @return 0 or JSDRV_ERROR_UNAVAILABLE.
 */
JSDRV_API int32_t jsdrv_tmap_get(struct jsdrv_tmap_s * self, size_t index, struct jsdrv_time_map_s * time_map);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_TMAP_H_ */
