/*
 * Copyright 2014-2025 Jetperch LLC
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
 * @brief MiniBitty version.
 */

#ifndef MB_VERSION_H_
#define MB_VERSION_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup mb
 * @defgroup mb_version Version
 *
 * @brief MiniBitty version.
 *
 * @{
 */

// DO NOT EDIT DIRECTLY: Edit CHANGELOG.md and then run bin/version_update.py.
#define MB_VERSION_MAJOR 0
#define MB_VERSION_MINOR 0
#define MB_VERSION_PATCH 1

/**
 * \brief The maximum version string length.
 *
 * The actual length is 14 bytes (MMM.mmm.ppppp\x0), but round up
 * to simplify packing.
 */
#define MB_VERSION_STR_LENGTH_MAX  (16U)

/**
 * \brief Macro to encode version to uint32_t
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The 32-bit encoded version number.
 */
#define MB_VERSION_ENCODE_U32(major, minor, patch) \
    ( (( ((uint32_t) (major)) &   0xff) << 24) | \
      (( ((uint32_t) (minor)) &   0xff) << 16) | \
      (( ((uint32_t) (patch)) & 0xffff) <<  0) )

/// Decode the major version from a U32 encoded version.
#define MB_VERSION_DECODE_U32_MAJOR(ver_u32_)   ((uint8_t) ((ver_u32_ >> 24) & 0xff))
/// Decode the minor version from a U32 encoded version.
#define MB_VERSION_DECODE_U32_MINOR(ver_u32_)   ((uint8_t) ((ver_u32_ >> 16) & 0xff))
/// Decode the patch version from a U32 encoded version.
#define MB_VERSION_DECODE_U32_PATCH(ver_u32_)   ((uint16_t) ((ver_u32_ >> 0) & 0xffff))

/**
 * \brief Macro to encode version to uint16_t (no patch)
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \returns The 16-bit encoded version number.
 */
#define MB_VERSION_ENCODE_U16(major, minor) \
    ( (( ((uint16_t) (major)) &   0xff) << 8) | \
      (( ((uint16_t) (minor)) &   0xff) << 0) )

/// Decode the major version from a U16 encoded version.
#define MB_VERSION_DECODE_U16_MAJOR(ver_u16_)   ((uint8_t) ((ver_u16_ >> 8) & 0xff))
/// Decode the minor version from a U32 encoded version.
#define MB_VERSION_DECODE_U16_MINOR(ver_u16_)   ((uint8_t) ((ver_u16_ >> 0) & 0xff))

/**
 * \brief Internal macro to convert argument to string.
 *
 * \param x The argument to convert to a string.
 * \return The string version of x.
 */
#define MB_VERSION__STR(x) #x

/**
 * \brief Macro to create the version string separated by "." characters.
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The firmware string.
 */
#define MB_VERSION_ENCODE_STR(major, minor, patch) \
        MB_VERSION__STR(major) "." MB_VERSION__STR(minor) "." MB_VERSION__STR(patch)

/// The current version as uint32_t
#define MB_VERSION_U32 MB_VERSION_ENCODE_U32(MB_VERSION_MAJOR, MB_VERSION_MINOR, MB_VERSION_PATCH)

/// The current version as uint16_t
#define MB_VERSION_U16 MB_VERSION_ENCODE_U16(MB_VERSION_MAJOR, MB_VERSION_MINOR)

/// The current version as "major.minor.patch" string
#define MB_VERSION_STR MB_VERSION_ENCODE_STR(MB_VERSION_MAJOR, MB_VERSION_MINOR, MB_VERSION_PATCH)

/** @} */

#endif /* MB_VERSION_H_ */
