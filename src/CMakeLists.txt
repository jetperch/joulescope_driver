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

if (WIN32)
    set(PLATFORM_SUPPORT_SOURCES
            backend/windows.c
            backend/winusb/msg_queue.c
    )

    set(PLATFORM_SRC
            backend/winusb/backend.c
            backend/winusb/device_change_notifier.c
    )
    set(PLATFORM_LIBS Setupapi Winusb)
    set(PLATFORM_TARGET_LINK_DIRS "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LibUSB REQUIRED libusb-1.0)
    message(STATUS "LibUSB_INCLUDE_DIRS = ${LibUSB_INCLUDE_DIRS}")
    string(REPLACE "/libusb-1.0" "" LibUSB_INCLUDE_DIRS "${LibUSB_INCLUDE_DIRS}")
    include_directories(${LibUSB_INCLUDE_DIRS})
    message(STATUS "LibUSB_INCLUDE_DIRS = ${LibUSB_INCLUDE_DIRS}")
    set(PLATFORM_SUPPORT_SOURCES
            backend/posix.c
            backend/libusb/msg_queue.c
    )

    set(PLATFORM_SRC
            backend/libusb/backend.c
    )
    set(PLATFORM_LIBS pthread m ${LibUSB_LIBRARIES})
    message(STATUS "LibUSB_LIBRARIES = ${LibUSB_LIBRARIES}")
    message(STATUS "LibUSB_LIBDIR = ${LibUSB_LIBDIR}")
    set(PLATFORM_TARGET_LINK_DIRS ${LibUSB_LIBDIR})
endif()

set(SUPPORT_SOURCES
        error_code.c
        cstr.c
        devices.c
        js110_cal.c
        js110_sample_processor.c
        js220_stats.c
        json.c
        log.c
        pubsub.c
        meta.c
        topic.c
        union.c
        version.c
        ${PLATFORM_SUPPORT_SOURCES}
)

set(SOURCES
        #emu.c
        #emulated.c
        js110_usb.c
        js220_usb.c
        js220_params.c
        jsdrv.c
        ${PLATFORM_SRC}
)

foreach(f IN LISTS SUPPORT_SOURCES)
    SET_FILENAME("${f}")
endforeach()

foreach(f IN LISTS SOURCES)
    SET_FILENAME("${f}")
endforeach()

add_library(jsdrv_support_objlib OBJECT ${SUPPORT_SOURCES})
add_dependencies(jsdrv_support_objlib tinyprintf)
target_link_libraries(jsdrv_support_objlib tinyprintf ${PLATFORM_LIBS})
target_link_directories(jsdrv_support_objlib PUBLIC ${PLATFORM_TARGET_LINK_DIRS})

add_library(jsdrv STATIC ${SOURCES})
add_dependencies(jsdrv jsdrv_support_objlib tinyprintf)
target_link_libraries(jsdrv jsdrv_support_objlib tinyprintf ${PLATFORM_LIBS})
target_link_directories(jsdrv PUBLIC ${PLATFORM_TARGET_LINK_DIRS})
