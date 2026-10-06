# SPDX-FileCopyrightText: Copyright 2022-2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.



if (NOT CMAKE_CROSSCOMPILING)
    SET(cmocka_dir ${CMAKE_CURRENT_SOURCE_DIR}/cmocka)
    include(CheckCCompilerFlag)
    include(${cmocka_dir}/ConfigureChecks.cmake)
    configure_file(${cmocka_dir}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)
    set(CMOCKA_INCLUDES ${CMAKE_CURRENT_BINARY_DIR} ${cmocka_dir}/include)
    if (CMAKE_COMPILER_IS_GNUCC)
        add_definitions(-Wno-format)
    endif()
    add_library(cmocka STATIC ${cmocka_dir}/src/cmocka.c)
    target_compile_definitions(cmocka PRIVATE -DHAVE_CONFIG_H=1)
    target_include_directories(cmocka PUBLIC ${CMOCKA_INCLUDES})
endif()


add_subdirectory(miniz)
add_subdirectory(tinyprintf)
if (WIN32)
    # use joulescope_driver support, not libusb
else()
    # use libusb on macOS (Darwin) and Linux
    add_subdirectory(libusb)
endif()
