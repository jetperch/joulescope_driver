# CMake build script for libusb
# Copyright © 2022 Matt Liberty <matt.liberty@jetperch.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

cmake_minimum_required(VERSION 3.22)
project(LIBUSB
        VERSION 1.0.26
        LANGUAGES C)

set(LIBUSB_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/libusb)

if (WIN32)
    message(STATUS "LIBUSB WIN32")
    set(PLATFORM_SRC
            libusb/os/events_windows.c
            libusb/os/threads_windows.c
            libusb/os/windows_common.c
            libusb/os/windows_winusb.c
    )
    set(PLATFORM_LIBS Setupapi Winusb)
elseif (APPLE)
    message(STATUS "LIBUSB APPLE MACOS")
    set(PLATFORM_SRC
            libusb/os/darwin_usb.c
            libusb/os/events_posix.c
            libusb/os/threads_posix.c
    )
    set(LIBUSB_INCLUDE ${LIBUSB_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/include/macos)
    set(PLATFORM_LIBS "objc -Wl,-framework,IOKit -Wl,-framework,CoreFoundation -Wl,-framework,Security")
elseif (UNIX)
    message(STATUS "LIBUSB LINUX")
    set(PLATFORM_SRC
            libusb/os/linux_udev.c
            libusb/os/linux_usbfs.c
            libusb/os/events_posix.c
            libusb/os/threads_posix.c
            )
    set(LIBUSB_INCLUDE "${LIBUSB_INCLUDE}" ${CMAKE_CURRENT_SOURCE_DIR}/include/linux)
    set(PLATFORM_LIBS udev)
else()
    message(ERROR "LIBUSB unsupported platform")
endif()

set(SOURCES
        libusb/core.c
        libusb/descriptor.c
        libusb/hotplug.c
        libusb/io.c
        libusb/strerror.c
        libusb/sync.c
)

message(STATUS "LIBUSB_INCLUDE = ${LIBUSB_INCLUDE}")
include_directories(${LIBUSB_INCLUDE})
add_library(libusb OBJECT ${PLATFORM_SRC} ${SOURCES})
target_include_directories(libusb PUBLIC ${LIBUSB_INCLUDE})
target_link_libraries(libusb PUBLIC ${PLATFORM_LIBS})
