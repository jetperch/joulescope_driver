/*
 * Copyright 2026 Jetperch LLC
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
 * @brief Cross-platform atomic operations.
 */

#ifndef JSDRV_OS_ATOMIC_H_
#define JSDRV_OS_ATOMIC_H_

#include <stdint.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_os_atomic Atomic operations
 *
 * @brief Provide cross-platform atomic integer operations.
 *
 * Three-tier implementation:
 * 1. C11 stdatomic.h (GCC, Clang, MSVC >= VS2022 17.5)
 * 2. Windows Interlocked intrinsics
 * 3. GCC/Clang __sync builtins
 *
 * @{
 */

// Prefer MSVC Interlocked on Windows for broadest compatibility
// (stdatomic.h requires MSVC >= VS2022 17.5 with /std:c17).
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
    && !defined(__STDC_NO_ATOMICS__) && !defined(_WIN32)
  #include <stdatomic.h>
  typedef atomic_int_fast32_t jsdrv_os_atomic_t;

  static inline int_fast32_t jsdrv_os_atomic_inc_(jsdrv_os_atomic_t * p) {
      return atomic_fetch_add(p, 1) + 1;
  }
  static inline int_fast32_t jsdrv_os_atomic_dec_(jsdrv_os_atomic_t * p) {
      return atomic_fetch_sub(p, 1) - 1;
  }
  #define JSDRV_OS_ATOMIC_INC(p)      jsdrv_os_atomic_inc_(p)
  #define JSDRV_OS_ATOMIC_DEC(p)      jsdrv_os_atomic_dec_(p)
  #define JSDRV_OS_ATOMIC_SET(p, v)   atomic_store((p), (v))
  #define JSDRV_OS_ATOMIC_GET(p)      atomic_load((p))

#elif defined(_WIN32)
  #include <windows.h>
  typedef volatile LONG jsdrv_os_atomic_t;

  #define JSDRV_OS_ATOMIC_INC(p)      InterlockedIncrement((p))
  #define JSDRV_OS_ATOMIC_DEC(p)      InterlockedDecrement((p))
  #define JSDRV_OS_ATOMIC_SET(p, v)   InterlockedExchange((p), (v))
  #define JSDRV_OS_ATOMIC_GET(p)      InterlockedCompareExchange((p), 0, 0)

#elif defined(__GNUC__) || defined(__clang__)
  // Pre-C11 fallback using GCC/Clang builtins.
  // Note: __sync_lock_test_and_set has acquire (not full)
  // barrier semantics, which is sufficient for x86 and ARM.
  typedef volatile int32_t jsdrv_os_atomic_t;

  #define JSDRV_OS_ATOMIC_INC(p)      (__sync_add_and_fetch((p), 1))
  #define JSDRV_OS_ATOMIC_DEC(p)      (__sync_sub_and_fetch((p), 1))
  #define JSDRV_OS_ATOMIC_SET(p, v)   (__sync_lock_test_and_set((p), (v)))
  #define JSDRV_OS_ATOMIC_GET(p)      (__sync_add_and_fetch((p), 0))

#else
  #error "No atomic implementation available for this compiler"
#endif

/** @} */

#endif  /* JSDRV_OS_ATOMIC_H_ */
