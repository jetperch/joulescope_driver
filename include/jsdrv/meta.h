/*
 * Copyright 2022 Jetperch LLC
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
 * @brief Metadata handling
 */

#ifndef JSDRV_META_H_
#define JSDRV_META_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/union.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_meta Metadata handling
 *
 * @brief Handle JSON-formatted metadata.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @brief Check the JSON metadata syntax.
 *
 * @param meta The JSON metadata.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_meta_syntax_check(const char * meta);

/**
 * @brief Get the data type.
 *
 * @param meta The JSON metadata.
 * @param[out] dtype The jsdrv_union_e data type.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_meta_dtype(const char * meta, uint8_t * dtype);

/**
 * @brief Get the default value.
 *
 * @param meta The JSON metadata.
 * @param[out] value The parsed default value.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_meta_default(const char * meta, struct jsdrv_union_s * value);

/**
 * @brief Validate a parameter value using the metadata.
 *
 * @param meta The JSON metadata.
 * @param[inout] value The value, which is modified in place.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_meta_value(const char * meta, struct jsdrv_union_s * value);

/**
 * @brief Metadata flag bits (mirrors minibitty's mb_pubsub_meta_flags_e).
 */
enum jsdrv_meta_flag_e {
    JSDRV_META_FLAG_RO   = 0x01,    ///< Topic is read-only; cannot be updated by the host.
    JSDRV_META_FLAG_HIDE = 0x02,    ///< Topic should not appear in the user interface.
    JSDRV_META_FLAG_DEV  = 0x04,    ///< Developer option; not for production use.
};

/**
 * @brief Get the flags bitmask from the metadata.
 *
 * Parses the "flags" key from the JSON metadata, which is a list
 * of string tokens: "ro", "hide", "dev".  Returns a bitmask of
 * jsdrv_meta_flag_e.  If "flags" is absent, *flags is set to 0.
 *
 * @param meta The JSON metadata.
 * @param[out] flags The resulting bitmask.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_meta_flags(const char * meta, uint32_t * flags);

JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_META_H_ */
