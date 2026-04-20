/*
 * SPDX-FileCopyrightText: Copyright 2014-2024 Jetperch LLC
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
 * @brief Commonly used C macros for RTOS.
 */

#ifndef MB_CDEF_INC_H__
#define MB_CDEF_INC_H__

#include <stddef.h>  // offsetof

/**
 * @ingroup mb_core
 * @defgroup mb_cmacro_inc C Macros
 *
 * @brief Commonly used C macros for RTOS.
 *
 * @{
 */

/// Indicate state machine code generation section
#define MB_DEF_STATE_MACHINE 0

/// Indicate code generation section
#define MB_GENERATOR 0

/**
 * @brief The unique file identifier for this compilation unit.
 *
 * This must be defined by the build system uniquely for
 * each compilation unit.
 */
#ifndef MB_SOURCE_CODE_FILE_ID
#define MB_SOURCE_CODE_FILE_ID (0)
#endif

/**
 * @brief The unique identifier for each source code line.
 */
#define MB_SOURCE_CODE_ID  ((((uint32_t) (MB_SOURCE_CODE_FILE_ID)) << 16) | (__LINE__ & 0xffff))

/**
 * @def MB_CPP_GUARD_START
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at near the top of the header file after
 * the \#if and imports.
 */

/**
 * @def MB_CPP_GUARD_END
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at the bottom of the header file just before
 * the \#endif.
 */

#if defined(__cplusplus) && !defined(__CDT_PARSER__)
#define MB_CPP_GUARD_START extern "C" {
#define MB_CPP_GUARD_END };
#else
#define MB_CPP_GUARD_START
#define MB_CPP_GUARD_END
#endif

/**
 * @brief Compute the number of elements in an array.
 *
 * @param x The array (NOT a pointer).
 * @return The number of elements in the array.
 */
#define MB_ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )

/**
 * @brief Find the container instance given a member pointer.
 *
 * @param ptr The pointer to an instance member.
 * @param type The type of the container that has member.
 * @param member The name of the member targeted by ptr.
 * @return The pointer to the container.
 */
#define MB_CONTAINER_OF(ptr, type, member) \
( (type *) (((uintptr_t) (ptr)) - offsetof(type, member)) )

/**
 * @brief Perform a compile-time check
 *
 * @param COND The condition which should normally be true.
 * @param MSG The error message which must be a valid C identifier  This
 *      message will be cryptically displayed by the compiler on error.
 */
#define MB_STATIC_ASSERT(COND, MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

/**
 * @brief Declare a packed structure.
 */
#ifdef __GNUC__
#define MB_NORETURN __attribute__((noreturn))
#define MB_STRUCT_PACKED __attribute__((packed))
#define MB_ALIGNED(value) __attribute__((aligned(value)))
#define MB_USED __attribute__((used))
#define MB_FORMAT __attribute__((format))
#define MB_MALLOC __attribute__((malloc))
#define MB_API

#ifndef MB_COMPILER_ALIGN
#define MB_COMPILER_ALIGN(size) __attribute__((aligned(size)))
#endif

#ifndef MB_INLINE_FN
#define MB_INLINE_FN static inline __attribute__((always_inline))
#endif
#else
#define MB_NORETURN
#define MB_STRUCT_PACKED
#define MB_ALIGNED(value)
#define MB_USED
#define MB_FORMAT
#define MB_MALLOC
#define MB_API

#ifndef MB_COMPILER_ALIGN
#define MB_COMPILER_ALIGN
#endif

#ifndef MB_INLINE_FN
#define MB_INLINE_FN static inline
#endif
#endif

/** @} */

#endif /* MB_CDEF_INC_H__ */
