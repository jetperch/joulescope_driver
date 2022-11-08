# Copyright 2014-2021 Jetperch LLC
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

FIND_PACKAGE(Doxygen)
if (NOT DOXYGEN_FOUND)
    message(FATAL_ERROR
            "Doxygen is needed to build the documentation. Please install it")
endif()

find_package(Sphinx REQUIRED)
set(SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/sphinx)
set(SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/sphinx)
file(MAKE_DIRECTORY ${SPHINX_BUILD})

#-- Configure the Template Doxyfile for our specific project
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/doxygen/Doxyfile.in
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile  @ONLY IMMEDIATE)

set(DOXYGEN_INDEX_FILE ${CMAKE_CURRENT_BINARY_DIR}/html/index.html)
set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/doxygen)
file(MAKE_DIRECTORY ${DOXYGEN_OUTPUT_DIR})

# Find all the public headers
file(GLOB_RECURSE JSDRV_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../include/*.h)

#-- Add a custom target to run Doxygen when ever the project is built
add_custom_target(doxygen ALL
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        DEPENDS jsdrv  ${JSDRV_PUBLIC_HEADERS}
        BYPRODUCTS ${DOXYGEN_INDEX_FILE}
        comment "Generating C documentation with Sphinx"
        SOURCES ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html/ DESTINATION doc)

add_custom_target(sphinx ALL
        COMMAND
            ${SPHINX_EXECUTABLE} -b html
            -Dbreathe_projects.jsdrv=${DOXYGEN_OUTPUT_DIR}/xml
            ${SPHINX_SOURCE} ${SPHINX_BUILD}
        DEPENDS doxygen
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        COMMENT "Generating C and python documentation with Sphinx")
