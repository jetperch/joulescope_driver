# Copyright 2014-2022 Jetperch LLC
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

cmake_minimum_required (VERSION 3.1)
set(PARENT_PROJECT_DIR ${PROJECT_SOURCE_DIR})

if ("${CMAKE_C_COMPILER}" MATCHES "MSVC")
    message(STATUS "Using Microsoft Visual Studio compiler")
    set(JOULESCOPE_DRIVER_COMPILER "MSVC")
else()
    set(JOULESCOPE_DRIVER_COMPILER "")
endif()

project(JOULESCOPE_DRIVER
        VERSION 1.3.17
        LANGUAGES C)
SET(PROJECT_PREFIX JSDRV)
SET(VERSION_STRING "${PROJECT_VERSION}")
SET(VERSION_UNDERSCORE "${PROJECT_VERSION_MAJOR}_${PROJECT_VERSION_MINOR}_${PROJECT_VERSION_PATCH}")

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

if (NOT PARENT_PROJECT_DIR)
    set(JSDRV_TOPLEVEL 1)
    message(STATUS "JSDRV: IS toplevel")
else()
    set(JSDRV_TOPLEVEL 0)
    message(STATUS "JSDRV: is NOT toplevel")
endif()

option(JSDRV_DOCS "Use Doxygen to create the HTML based Host API documentation" OFF)
option(JSDRV_UNIT_TEST "Build the JSDRV unit tests" ON)
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

function (SET_FILENAME _filename)
    get_filename_component(b ${_filename} NAME)
    set_source_files_properties(${_filename} PROPERTIES
        COMPILE_DEFINITIONS "__FILENAME__=\"${b}\"")
endfunction (SET_FILENAME)


if (JSDRV_TOPLEVEL AND CMAKE_COMPILER_IS_GNUCC)
    add_definitions(-Wall -Wextra -Wpedantic -Werror -fPIC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11")
endif()
remove_definitions(-D__cplusplus)
if (JSDRV_TOPLEVEL AND WIN32 AND CMAKE_COMPILER_IS_GNUCC)
    # Ugh, mingw
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-format -std=gnu11")
endif()

add_subdirectory(third-party)

set(JSDRV_SOURCE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    CACHE INTERNAL "JSDRV source path" FORCE
)

set(JSDRV_INCLUDE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${JSDRV_THIRD_PARTY_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/include_private
        CACHE INTERNAL "JSDRV include paths" FORCE
        )

message(STATUS "INCLUDE: ${JSDRV_INCLUDE}")
include_directories(${JSDRV_INCLUDE})
add_subdirectory(src)
add_subdirectory(example)

if(JSDRV_UNIT_TEST AND JSDRV_TOPLEVEL AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(test)
endif()

if(JSDRV_DOCS AND JSDRV_TOPLEVEL)
    add_subdirectory(doc)
endif()
