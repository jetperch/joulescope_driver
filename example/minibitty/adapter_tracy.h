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
 * @brief Message communication frame format.
 */

#ifndef MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__
#define MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__

#include "mb/cdef.h"
#include <stdint.h>

MB_CPP_GUARD_START

void * adapter_tracy_initialize(struct jsdrv_context_s * context);
void adapter_tracy_finalize(void * self);
void adapter_tracy_on_trace(void * user_data, const char * topic, const struct jsdrv_union_s * value);

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_EXAMPLE_MINIBITTY_ADAPTER_TRACY_H__ */
