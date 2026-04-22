# Copyright 2014-2025 Jetperch LLC
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
    set(PLATFORM_DEPENDENCIES "")
    set(PLATFORM_LIBS Setupapi Winusb winmm)
    set(PLATFORM_TARGET_LINK_DIRS "")
    if (BUILD_SHARED_LIBS)
        add_definitions(-DJSDRV_EXPORT=1)
    endif()
else()
    set(PLATFORM_SUPPORT_SOURCES
            backend/posix.c
            backend/libusb/msg_queue.c
    )

    set(PLATFORM_SRC
            backend/libusb/backend.c
    )
    set(PLATFORM_DEPENDENCIES libusb)
    set(PLATFORM_LIBS pthread m libusb)
    set(PLATFORM_TARGET_LINK_DIRS ${LibUSB_LIBDIR})
endif()

set(SUPPORT_SOURCES
        buffer_signal.c
        error_code.c
        calibration_hash.c
        cstr.c
        downsample.c
        devices/js110/js110_cal.c
        devices/js220/js220_i128.c
        devices/js110/js110_sample_processor.c
        devices/js110/js110_stats.c
        devices/js220/js220_stats.c
        json.c
        log.c
        pubsub.c
        meta.c
        meta_binary.c
        sample_buffer_f32.c
        statistics.c
        time.c
        time_map_filter.c
        tmap.c
        topic.c
        union.c
        version.c
        ${PLATFORM_SUPPORT_SOURCES}
)

set(SOURCES
        buffer.c
        devices.c
        #emu.c
        #emulated.c
        devices/js110/js110_usb.c
        devices/js220/js220_usb.c
        devices/js220/js220_params.c
        devices/js320/firmware.c
        devices/js320/js320_drv.c
        devices/js320/js320_fwup.c
        devices/js320/js320_fwup_mgr.c
        devices/js320/js320_jtag.c
        devices/js320/js320_stats.c
        devices/mb_device/mb_device.c
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
add_dependencies(jsdrv_support_objlib tinyprintf ${PLATFORM_DEPENDENCIES})
target_link_libraries(jsdrv_support_objlib tinyprintf ${PLATFORM_LIBS})
target_link_directories(jsdrv_support_objlib PUBLIC ${PLATFORM_TARGET_LINK_DIRS})

add_library(jsdrv ${SOURCES})
add_dependencies(jsdrv jsdrv_support_objlib tinyprintf miniz ${PLATFORM_DEPENDENCIES})
target_link_libraries(jsdrv PRIVATE jsdrv_support_objlib tinyprintf miniz ${PLATFORM_LIBS})
target_link_directories(jsdrv PUBLIC ${PLATFORM_TARGET_LINK_DIRS})
target_include_directories(jsdrv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
