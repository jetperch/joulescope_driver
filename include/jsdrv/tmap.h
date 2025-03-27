/*
* Copyright 2023-2025 Jetperch LLC
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

#ifndef JSDRV_TMAP_H__
#define JSDRV_TMAP_H__

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
 *
 * References:
 *   - https://github.com/jetperch/jls/blob/main/include_prv/jls/tmap.h
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/// The opaque instance.
struct jsdrv_tmap_s;

/**
 * @brief Allocate a new tmap instance.
 *
 * @param initial_size The initial allocation size.  0 to use default.
 * @return The new tmap instance.
 *
 * tmap instances are designed for thread safety with a single writer
 * and multiple readers.  Each time the writer shares this instance
 * with a reader, the writer must call jsdrv_tmp_ref_incr().
 * The reader then calls jsdrv_tmap_reader_enter() and jsdrv_tmap_reader_exit()
 * whenever it wants to use the reader methods.  When the reader is done
 * with the instance, it must call jsdrv_tmp_ref_decr.
 * When the writer finalizes, it also must call jsdrv_tmp_ref_decr().
 */
struct jsdrv_tmap_s * jsdrv_tmap_alloc(size_t initial_size);

/**
 * @brief Increment the reference count on this instance.
 *
 * @param self The instance.
 * @see jsdrv_tmap_ref_decr()
 *
 * Whenever the writer shares this instance with a reader or
 * a reader shares this instance with a reader in another thread,
 * call this function.
 *
 * The receiving reader must call jsdrv_tmap_ref_decr() when
 * it is done with the instance.
 */
void jsdrv_tmap_ref_incr(struct jsdrv_tmap_s * self);

/**
 * @brief Decrement the reference count on this instance.
 * @param self The instance.
 *
 * When the reference count reaches zero, the underlying instance
 * is freed.  The caller must not hold on to the instance pointer
 * after calling jsdrv_tmap_ref_decr().
 */
void jsdrv_tmap_ref_decr(struct jsdrv_tmap_s * self);

/**
 * @brief Clear all data from this instance.
 *
 * @param self The instance.
 * @note Unlike other writer functions, this function blocks on the reader mutex.
 */
void jsdrv_tmap_clear(struct jsdrv_tmap_s * self);

/**
 * @brief Get the current size.
 *
 * @param self The instance
 * @return The number of entries in this instance.
 * @note Readers should call this from within jsdrv_tmap_reader_enter().
 */
size_t jsdrv_tmap_size(struct jsdrv_tmap_s * self);

/**
 * @brief Add a new time map entry to this instance.
 *
 * @param self The instance.
 * @param time_map The new time map.
 *
 * This function does not block, but the update will be deferred
 * until no readers are actively using this instance.
 */
void jsdrv_tmap_add(struct jsdrv_tmap_s * self, const struct jsdrv_time_map_s * time_map);

/**
 * @brief Expire old time map entries.
 *
 * @param self The instance.
 * @param sample_id The oldest sample_id that this instance needs to represent.
 *
 * This function does not block, but the update will be deferred
 * until no readers are actively using this instance.
 */
void jsdrv_tmap_expire_by_sample_id(struct jsdrv_tmap_s * self, uint64_t sample_id);

/**
 * @brief Indicate that a reader is actively using this instance.
 *
 * @param self The instance.
 * @see jsdrv_tmap_reader_exit
 *
 * This implementation provides for a single writer (updater) and
 * multiple concurrent readers.  Readers indicate that they are
 * active by calling this function before calling any of the
 * following functions:
 * - jsdrv_tmap_size()
 * - jsdrv_tmap_sample_id_to_timestamp()
 * - jsdrv_tmap_timestamp_to_sample_id()
 * - jsdrv_tmap_get()
 *
 * To prevent blocking the writer, updates are posted and deferred
 * when any reader is active.  Readers should keep their blocking
 * sections as short as possible.
 * Call jsdrv_tmap_reader_exit() when done.
 */
void jsdrv_tmap_reader_enter(struct jsdrv_tmap_s * self);

/**
 * @brief Indicate that a reader is done using this instance.
 *
 * @param self The instance.
 * @see jsdrv_tmap_reader_enter
 */
void jsdrv_tmap_reader_exit(struct jsdrv_tmap_s * self);

/**
 * @brief Map sample id to a UTC timestamp.
 *
 * @param self The instance.
 * @param sample_id The sample id to map
 * @param timestamp[out] The timestamp corresponding to sample id.
 * @return 0 or JSDRV_ERROR_UNAVAILABLE.
 * @note Must be in a reader section initiated by jsdrv_tmap_reader_enter()
 */
int32_t jsdrv_tmap_sample_id_to_timestamp(struct jsdrv_tmap_s * self, uint64_t sample_id, int64_t * timestamp);

/**
 * @brief Map UTC timestamp to a sample id.
 *
 * @param self The instance.
 * @param timestamp The timestamp to map.
 * @param sample_id[out] The sample id corresponding to timestamp.
 * @return 0 or JSDRV_ERROR_UNAVAILABLE.
 * @note Must be in a reader section initiated by jsdrv_tmap_reader_enter()
 */
int32_t jsdrv_tmap_timestamp_to_sample_id(struct jsdrv_tmap_s * self, int64_t timestamp, uint64_t * sample_id);

/**
 * @brief Get an entry from the tmap instance.
 *
 * @param self The instance.
 * @param index The entry index. 0 is oldest.  jsdrv_tmap_size() - 1 is newest.
 * @return The entry for the corresponding index.
 * @note Must be in a reader section initiated by jsdrv_tmap_reader_enter()
 */
struct jsdrv_time_map_s * jsdrv_tmap_get(struct jsdrv_tmap_s * self, size_t index);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_TMAP_H__ */
