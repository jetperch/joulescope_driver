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
 * @brief JSDRV library version.
 */

#ifndef JSDRV_VERSION_H_
#define JSDRV_VERSION_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_version Version
 *
 * @brief Joulescope driver version.
 *
 * @{
 */

// Use version_update.py to update.

/**
 * @brief The Joulescope driver major version.
 *
 * Changes in the major version indicate that this release
 * breaks backwards compatibility.
 */
#define JSDRV_VERSION_MAJOR 1

/**
 * @brief The Joulescope driver minor version.
 *
 * Changes in the minor version indicate that new features were
 * added.  Any changes that break backwards compatibility should
 * not adversely affect performance.
 */
#define JSDRV_VERSION_MINOR 0

/**
 * @brief The Joulescope driver patch version.
 *
 * Changes in the patch version indicate bug fixes and improvements.
 */
#define JSDRV_VERSION_PATCH 3

/**
 * \brief The maximum version string length.
 *
 * The actual length is 14 bytes (MMM.mmm.ppppp\\x00), but round up
 * to simplify packing.
 */
#define JSDRV_VERSION_STR_LENGTH_MAX  (16)

/**
 * \brief Macro to encode version to uint32_t
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The 32-bit encoded version number.
 */
#define JSDRV_VERSION_ENCODE_U32(major, minor, patch) \
    ( (( ((uint32_t) (major)) &   0xff) << 24) | \
      (( ((uint32_t) (minor)) &   0xff) << 16) | \
      (( ((uint32_t) (patch)) & 0xffff) <<  0) )

/// Decode the major version from a U32 encoded version.
#define JSDRV_VERSION_DECODE_U32_MAJOR(ver_u32_)   ((uint8_t) ((ver_u32_ >> 24) & 0xff))
/// Decode the minor version from a U32 encoded version.
#define JSDRV_VERSION_DECODE_U32_MINOR(ver_u32_)   ((uint8_t) ((ver_u32_ >> 16) & 0xff))
/// Decode the patch version from a U32 encoded version.
#define JSDRV_VERSION_DECODE_U32_PATCH(ver_u32_)   ((uint16_t) ((ver_u32_ >> 0) & 0xffff))

/**
 * \brief Internal macro to convert argument to string.
 *
 * \param x The argument to convert to a string.
 * \return The string version of x.
 */
#define JSDRV_VERSION__STR(x) #x

/**
 * \brief Macro to create the version string separated by "." characters.
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The firmware string.
 */
#define JSDRV_VERSION_ENCODE_STR(major, minor, patch) \
        JSDRV_VERSION__STR(major) "." JSDRV_VERSION__STR(minor) "." JSDRV_VERSION__STR(patch)

/// The JSDRV version as uint32_t
#define JSDRV_VERSION_U32 JSDRV_VERSION_ENCODE_U32(JSDRV_VERSION_MAJOR, JSDRV_VERSION_MINOR, JSDRV_VERSION_PATCH)

/// The JSDRV version as "major.minor.patch" string
#define JSDRV_VERSION_STR JSDRV_VERSION_ENCODE_STR(JSDRV_VERSION_MAJOR, JSDRV_VERSION_MINOR, JSDRV_VERSION_PATCH)

/**
 * \brief Convert a u32 encoded version as a string.
 *
 * \param[in] u32 The u32 encoded version.
 * \param[out] str The output string, which should have at least 14
 *      bytes available to avoid truncation.
 * \param[in] size The number of bytes available in str.
 */
void jsdrv_version_u32_to_str(uint32_t u32, char * str, size_t size);

/** @} */

#endif /* JSDRV_VERSION_H_ */
