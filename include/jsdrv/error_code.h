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

/**
 * @file
 *
 * @brief Standard jsdrv status and error codes.
 */

#ifndef JSDRV_ERROR_CODE_H_
#define JSDRV_ERROR_CODE_H_

#include "jsdrv/cmacro_inc.h"

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_ec Error codes
 *
 * @brief Standardize error code definitions.
 *
 * See <a href="http://www.cplusplus.com/reference/system_error/errc/">errc</a>
 *
 * @{
 */

/*
This file is parsed by a python script to generate the enums.
This section defines all the available error codes:

int  name                      description

DEF_ERROR_CODE_START
0    SUCCESS                   "Success (no error)"
1    UNSPECIFIED               "Unspecified error"
2    NOT_ENOUGH_MEMORY         "Insufficient memory to complete the operation"
3    NOT_SUPPORTED             "Operation is not supported"
4    IO                        "Input/output error"
5    PARAMETER_INVALID         "The parameter value is invalid"
6    INVALID_RETURN_CONDITION  "The function return condition is invalid"
7    INVALID_CONTEXT           "The context is invalid"
8    INVALID_MESSAGE_LENGTH    "The message length in invalid"
9    MESSAGE_INTEGRITY         "The message integrity check failed"
10   SYNTAX_ERROR              "A syntax error was detected"
11   TIMED_OUT                 "The operation did not complete in time"
12   FULL                      "The target of the operation is full"
13   EMPTY                     "The target of the operation is empty"
14   TOO_SMALL                 "The target of the operation is too small"
15   TOO_BIG                   "The target of the operation is too big"
16   NOT_FOUND                 "The requested resource was not found"
17   ALREADY_EXISTS            "The requested resource already exists"
18   PERMISSIONS               "Insufficient permissions to perform the operation."
19   BUSY                      "The requested resource is currently busy."
20   UNAVAILABLE               "The requested resource is currently unavailable."
21   IN_USE                    "The requested resource is currently in use."
22   CLOSED                    "The requested resource is currently closed."
23   SEQUENCE                  "The requested operation was out of sequence."
24   ABORTED                   "The requested operation was previously aborted."
25   SYNCHRONIZATION           "The target is not synchronized with the originator."
DEF_ERROR_CODE_END
 */

/**
 * @brief The list of error codes.
 */
enum jsdrv_error_code_e {
    // ENUM_ERROR_CODE_START
    JSDRV_ERROR_SUCCESS = 0,  ///< Success (no error)
    JSDRV_ERROR_UNSPECIFIED = 1,  ///< Unspecified error
    JSDRV_ERROR_NOT_ENOUGH_MEMORY = 2,  ///< Insufficient memory to complete the operation
    JSDRV_ERROR_NOT_SUPPORTED = 3,  ///< Operation is not supported
    JSDRV_ERROR_IO = 4,  ///< Input/output error
    JSDRV_ERROR_PARAMETER_INVALID = 5,  ///< The parameter value is invalid
    JSDRV_ERROR_INVALID_RETURN_CONDITION = 6,  ///< The function return condition is invalid
    JSDRV_ERROR_INVALID_CONTEXT = 7,  ///< The context is invalid
    JSDRV_ERROR_INVALID_MESSAGE_LENGTH = 8,  ///< The message length in invalid
    JSDRV_ERROR_MESSAGE_INTEGRITY = 9,  ///< The message integrity check failed
    JSDRV_ERROR_SYNTAX_ERROR = 10,  ///< A syntax error was detected
    JSDRV_ERROR_TIMED_OUT = 11,  ///< The operation did not complete in time
    JSDRV_ERROR_FULL = 12,  ///< The target of the operation is full
    JSDRV_ERROR_EMPTY = 13,  ///< The target of the operation is empty
    JSDRV_ERROR_TOO_SMALL = 14,  ///< The target of the operation is too small
    JSDRV_ERROR_TOO_BIG = 15,  ///< The target of the operation is too big
    JSDRV_ERROR_NOT_FOUND = 16,  ///< The requested resource was not found
    JSDRV_ERROR_ALREADY_EXISTS = 17,  ///< The requested resource already exists
    JSDRV_ERROR_PERMISSIONS = 18,  ///< Insufficient permissions to perform the operation.
    JSDRV_ERROR_BUSY = 19,  ///< The requested resource is currently busy.
    JSDRV_ERROR_UNAVAILABLE = 20,  ///< The requested resource is currently unavailable.
    JSDRV_ERROR_IN_USE = 21,  ///< The requested resource is currently in use.
    JSDRV_ERROR_CLOSED = 22,  ///< The requested resource is currently closed.
    JSDRV_ERROR_SEQUENCE = 23,  ///< The requested operation was out of sequence.
    JSDRV_ERROR_ABORTED = 24,  ///< The requested operation was previously aborted.
    JSDRV_ERROR_SYNCHRONIZATION = 25,  ///< The target is not synchronized with the originator.
    // ENUM_ERROR_CODE_END
};

/// A shorter, less confusing alias for success.
#define JSDRV_SUCCESS JSDRV_ERROR_SUCCESS

JSDRV_CPP_GUARD_START

/**
 * @brief Convert an error code into its short name.
 *
 * @param[in] ec The error code (jsdrv_error_code_e).
 * @return The short string name for the error code.
 */
JSDRV_API const char * jsdrv_error_code_name(int ec);

/**
 * @brief Convert an error code into its description.
 *
 * @param[in] ec The error code (jsdrv_error_code_e).
 * @return The user-meaningful description of the error.
 */
JSDRV_API const char * jsdrv_error_code_description(int ec);

JSDRV_CPP_GUARD_END

/** @} */

#endif /* JSDRV_ERROR_CODE_H_ */
