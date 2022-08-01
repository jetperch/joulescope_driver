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
 * @brief JSON parser.
 */

#ifndef JSDRV_JSON_H__
#define JSDRV_JSON_H__

#include "jsdrv/cmacro_inc.h"
#include "jsdrv/union.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_json Simple JSON parser.
 *
 * @brief Parse JSON into a token stream.
 *
 * This JSON parser is designed to support the JSON-formatted Joulescope driver
 * metadata.  It operates completely from stack memory with no required
 * dynamic or static memory.  The parser is SAX-like with callbacks.
 * The parser uses jsdrv_union_s as the callback tokens.
 *
 * The most common way to use this JSON parser is to create a structure on the
 * stack that is the callback user_data.  The callback then uses this state
 * and implements a finite state machine to process the expected tokens.
 *
 * This parser also supports the non-standard "NaN" literal which
 * is used by the JS110 calibration JSON record.
 *
 * Alternatives include:
 *   - https://github.com/zserge/jsmn
 *   - https://github.com/DaveGamble/cJSON
 *   - https://github.com/Tencent/rapidjson
 *
 * References:
 *   - https://www.json.org
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @brief The token types emitted by the parser.
 */
enum jsdrv_json_token_e {
    JSDRV_JSON_VALUE,         // dtype: string, i32, f32, null
    JSDRV_JSON_KEY,           // dtype: string
    JSDRV_JSON_OBJ_START,     // dtype: null
    JSDRV_JSON_OBJ_END,       // dtype: null
    JSDRV_JSON_ARRAY_START,   // dtype: null
    JSDRV_JSON_ARRAY_END,     // dtype: null
};

/**
 * @brief The function to call for each token.
 *
 * @param user_data The arbitrary user data.
 * @param token The next parsed token.  The value only remains valid for the
 *      duration of the callback.  String values are NOT null terminated, and
 *      this function must use the token->size field.
 *      The "op" field contains jsdrv_json_token_e.
 * @return 0 or error code to stop processing.  Use JSDRV_ERROR_ABORTED to signal
 *      that processing completed as expected.  All other error codes will be
 *      returned by jsdrv_json_parse().
 */
typedef int32_t (*jsdrv_json_fn)(void * user_data, const struct jsdrv_union_s * token);

/**
 * @brief Parse JSON into tokens.
 *
 * @param json The null-terminated JSON string to parse.
 * @param cbk_fn The function to all for each token.
 * @param cbk_user_data The arbitrary data to provide to cbk_fn.
 * @return 0 or error code.
 */
JSDRV_API int32_t jsdrv_json_parse(const char * json, jsdrv_json_fn cbk_fn, void * cbk_user_data);

/**
 * @brief Compare string to token.
 *
 * @param str The null-terminated string.
 * @param token The token, usually provided to the jsdrv_json_parse() callback.
 * @return 1 if equal, 0 if not equal.
 *
 * Since token strings are NOT null terminated, C-standard strcmp does not work.
 * memcmp works if the string lengths match, which is not guaranteed.
 */
JSDRV_API int32_t jsdrv_json_strcmp(const char * str, const struct jsdrv_union_s * token);


JSDRV_CPP_GUARD_END

/** @} */

#endif  /* JSDRV_JSON_H__ */
