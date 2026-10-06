/*
 * SPDX-FileCopyrightText: Copyright 2025 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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
 * @brief Message communication frame format.
 */

#ifndef MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__
#define MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__

#include "mb/cdef.h"
#include <stdint.h>

MB_CPP_GUARD_START

struct mbgen_s;

/**
 * @brief Initialize the Tracy forwarding client.
 *
 * @param context The jsdrv context.
 * @param mbgen The optional loaded mbgen.bin metadata for name resolution.
 *      NULL uses raw "task.N" / "isr.N" names.  Caller retains ownership
 *      and must keep it valid until adapter_tracy_finalize().
 * @param verbose_name When true, display "usbd (task.2)" instead of "usbd".
 * @return The instance for adapter_tracy_on_trace() and adapter_tracy_finalize().
 */
void * adapter_tracy_initialize(struct jsdrv_context_s * context,
                                const struct mbgen_s * mbgen,
                                int verbose_name);
void adapter_tracy_finalize(void * self);
void adapter_tracy_on_trace(void * user_data, const char * topic, const struct jsdrv_union_s * value);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__ */
