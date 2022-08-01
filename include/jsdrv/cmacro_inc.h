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
 * @brief Commonly used C macros for the Joulescope driver.
 */

#ifndef JSDRV_CMACRO_INC_H__
#define JSDRV_CMACRO_INC_H__

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_cmacro_inc C Macros
 *
 * @brief Commonly used C macros for the Joulescope Driver.
 *
 * @{
 */

/**
 * @def JSDRV_CPP_GUARD_START
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at near the top of the header file after
 * the \#if and imports.
 */

/**
 * @def JSDRV_CPP_GUARD_END
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at the bottom of the header file just before
 * the \#endif.
 */

#if defined(__cplusplus) && !defined(__CDT_PARSER__)
#define JSDRV_CPP_GUARD_START extern "C" {
#define JSDRV_CPP_GUARD_END };
#else
#define JSDRV_CPP_GUARD_START
#define JSDRV_CPP_GUARD_END
#endif

/**
 * @def JSDRV_API
 * @brief All functions that are available from the library are marked with
 *      JSDRV_API.  This platform-specific definition allows DLLs to ber
 *      created properly on Windows.
 */
#if defined(WIN32) && defined(JSDRV_EXPORT)
#define JSDRV_API __declspec(dllexport)
#elif defined(WIN32) && defined(JSDRV_EXPORT)
#define JSDRV_API __declspec(dllimport)
#else
#define JSDRV_API
#endif

/**
 * @brief Declare a packed structure.
 */

#ifdef __GNUC__
#define JSDRV_STRUCT_PACKED __attribute__((packed))
#define JSDRV_USED __attribute__((used))
#define JSDRV_FORMAT __attribute__((format))
#define JSDRV_INLINE_FN __attribute__((always_inline)) static inline
#define JSDRV_PRINTF_FORMAT __attribute__((format (printf, 1, 2)))
#define JSDRV_COMPILER_ALLOC(free_fn) __attribute__((malloc))
//#define JSDRV_COMPILER_ALLOC(free_fn) __attribute__((malloc, malloc(free_fn, 1)))  // gcc 11
#else
#define JSDRV_STRUCT_PACKED
#define JSDRV_USED
#define JSDRV_FORMAT
#define JSDRV_INLINE_FN static inline
#define JSDRV_PRINTF_FORMAT
#define JSDRV_COMPILER_ALLOC(free_fn)
#endif


/** @} */

#endif /* JSDRV_CMACRO_INC_H__ */
