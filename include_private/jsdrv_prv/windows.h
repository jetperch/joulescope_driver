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
 * @brief Joulescope host driver backend API.
 */

#ifndef JSDRV_PRV_BACKEND_WINDOWS_H_
#define JSDRV_PRV_BACKEND_WINDOWS_H_

#include "jsdrv/cmacro_inc.h"
#include "jsdrv_prv/log.h"
#include <windows.h>

JSDRV_CPP_GUARD_START

BOOL GetErrorMessage(DWORD dwErrorCode, char * pBuffer, DWORD cchBufferLength);

#define WINDOWS_LOGE(format, ...) do { \
    char error_msg_[64]; \
    DWORD error_ = GetLastError(); \
    GetErrorMessage(error_, error_msg_, sizeof(error_msg_)); \
    JSDRV_LOGE(format ": %d: %s", __VA_ARGS__, error_, error_msg_); \
} while (0)

JSDRV_CPP_GUARD_END

#endif /* JSDRV_PRV_BACKEND_WINDOWS_H_ */
