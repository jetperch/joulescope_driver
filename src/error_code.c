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

#include "jsdrv/error_code.h"

const char * jsdrv_error_code_name(int ec) {
    switch (ec) {
        // CASE_NAME_START
        case JSDRV_ERROR_SUCCESS: return "SUCCESS";
        case JSDRV_ERROR_UNSPECIFIED: return "UNSPECIFIED";
        case JSDRV_ERROR_NOT_ENOUGH_MEMORY: return "NOT_ENOUGH_MEMORY";
        case JSDRV_ERROR_NOT_SUPPORTED: return "NOT_SUPPORTED";
        case JSDRV_ERROR_IO: return "IO";
        case JSDRV_ERROR_PARAMETER_INVALID: return "PARAMETER_INVALID";
        case JSDRV_ERROR_INVALID_RETURN_CONDITION: return "INVALID_RETURN_CONDITION";
        case JSDRV_ERROR_INVALID_CONTEXT: return "INVALID_CONTEXT";
        case JSDRV_ERROR_INVALID_MESSAGE_LENGTH: return "INVALID_MESSAGE_LENGTH";
        case JSDRV_ERROR_MESSAGE_INTEGRITY: return "MESSAGE_INTEGRITY";
        case JSDRV_ERROR_SYNTAX_ERROR: return "SYNTAX_ERROR";
        case JSDRV_ERROR_TIMED_OUT: return "TIMED_OUT";
        case JSDRV_ERROR_FULL: return "FULL";
        case JSDRV_ERROR_EMPTY: return "EMPTY";
        case JSDRV_ERROR_TOO_SMALL: return "TOO_SMALL";
        case JSDRV_ERROR_TOO_BIG: return "TOO_BIG";
        case JSDRV_ERROR_NOT_FOUND: return "NOT_FOUND";
        case JSDRV_ERROR_ALREADY_EXISTS: return "ALREADY_EXISTS";
        case JSDRV_ERROR_PERMISSIONS: return "PERMISSIONS";
        case JSDRV_ERROR_BUSY: return "BUSY";
        case JSDRV_ERROR_UNAVAILABLE: return "UNAVAILABLE";
        case JSDRV_ERROR_IN_USE: return "IN_USE";
        case JSDRV_ERROR_CLOSED: return "CLOSED";
        case JSDRV_ERROR_SEQUENCE: return "SEQUENCE";
        case JSDRV_ERROR_ABORTED: return "ABORTED";
        case JSDRV_ERROR_SYNCHRONIZATION: return "SYNCHRONIZATION";
        // CASE_NAME_END
        default: return "UNKNOWN";
    }
}

const char * jsdrv_error_code_description(int ec) {
    switch (ec) {
        // CASE_DESCRIPTION_START
        case JSDRV_ERROR_SUCCESS: return "Success (no error)";
        case JSDRV_ERROR_UNSPECIFIED: return "Unspecified error";
        case JSDRV_ERROR_NOT_ENOUGH_MEMORY: return "Insufficient memory to complete the operation";
        case JSDRV_ERROR_NOT_SUPPORTED: return "Operation is not supported";
        case JSDRV_ERROR_IO: return "Input/output error";
        case JSDRV_ERROR_PARAMETER_INVALID: return "The parameter value is invalid";
        case JSDRV_ERROR_INVALID_RETURN_CONDITION: return "The function return condition is invalid";
        case JSDRV_ERROR_INVALID_CONTEXT: return "The context is invalid";
        case JSDRV_ERROR_INVALID_MESSAGE_LENGTH: return "The message length in invalid";
        case JSDRV_ERROR_MESSAGE_INTEGRITY: return "The message integrity check failed";
        case JSDRV_ERROR_SYNTAX_ERROR: return "A syntax error was detected";
        case JSDRV_ERROR_TIMED_OUT: return "The operation did not complete in time";
        case JSDRV_ERROR_FULL: return "The target of the operation is full";
        case JSDRV_ERROR_EMPTY: return "The target of the operation is empty";
        case JSDRV_ERROR_TOO_SMALL: return "The target of the operation is too small";
        case JSDRV_ERROR_TOO_BIG: return "The target of the operation is too big";
        case JSDRV_ERROR_NOT_FOUND: return "The requested resource was not found";
        case JSDRV_ERROR_ALREADY_EXISTS: return "The requested resource already exists";
        case JSDRV_ERROR_PERMISSIONS: return "Insufficient permissions to perform the operation.";
        case JSDRV_ERROR_BUSY: return "The requested resource is currently busy.";
        case JSDRV_ERROR_UNAVAILABLE: return "The requested resource is currently unavailable.";
        case JSDRV_ERROR_IN_USE: return "The requested resource is currently in use.";
        case JSDRV_ERROR_CLOSED: return "The requested resource is currently closed.";
        case JSDRV_ERROR_SEQUENCE: return "The requested operation was out of sequence.";
        case JSDRV_ERROR_ABORTED: return "The requested operation was previously aborted.";
        case JSDRV_ERROR_SYNCHRONIZATION: return "The target is not synchronized with the originator.";
        // CASE_DESCRIPTION_END
        default: return "Unknown error";
    }
}
