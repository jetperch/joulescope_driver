/*
* Copyright 2025 Jetperch LLC
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
 * @brief Adapter header.
 */

#ifndef MB_EXAMPLE_MINIBITTY_ADAPTER_H__
#define MB_EXAMPLE_MINIBITTY_ADAPTER_H__

#include "mb/cdef.h"
#include <stdint.h>

MB_CPP_GUARD_START

#define MB_TRACE_SOF  (0xC3U)


/// The RTOS object types.
enum mb_object_type_e {
    MB_OBJ_ISR = 0,         ///< An interrupt service routine, for trace only.
    MB_OBJ_CONTEXT = 1,     ///< The main RTOS context object.
    MB_OBJ_TASK = 2,        ///< A task.
    MB_OBJ_TIMER = 3,       ///< A timer deadline.
    MB_OBJ_MSG = 4,         ///< A message.
    MB_OBJ_RSV5 = 5,        ///< Reserved for now
    MB_OBJ_HEAP = 6,        ///< The heap
    MB_OBJ_TRACE = 7,       ///< The trace for OS trace, value trace, & logging.
    MB_OBJ_FSM = 8,         ///< A finite state machine instance.
};

enum mb_trace_type_e {
    MB_TRACE_TYPE_INVALID = 0,
    MB_TRACE_TYPE_READY = 1,
    MB_TRACE_TYPE_ENTER = 2,
    MB_TRACE_TYPE_EXIT = 3,  // optional duration if enter is omitted
    MB_TRACE_TYPE_ALLOC = 4,
    MB_TRACE_TYPE_FREE = 5,
    MB_TRACE_TYPE_RSV6 = 6,
    MB_TRACE_TYPE_RSV7 = 7,
    MB_TRACE_TYPE_TIMESYNC = 8,
    MB_TRACE_TYPE_TIMEMAP = 9,
    MB_TRACE_TYPE_FAULT = 10,
    MB_TRACE_TYPE_VALUE = 11,
    MB_TRACE_TYPE_LOG = 12,
    MB_TRACE_TYPE_RSV13 = 13,
    MB_TRACE_TYPE_RSV14 = 14,
    MB_TRACE_TYPE_OVERFLOW = 15,
};

extern const char * MB_OBJ_NAME[16];

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_EXAMPLE_MINIBITTY_ADAPTER_H__ */
