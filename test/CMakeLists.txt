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

include_directories(${CMOCKA_INCLUDE})

function (ADD_CMOCKA_TEST _testName)
    set(TARGET ${_testName})
    SET_FILENAME("${_testName}.c")
    add_executable(${_testName} "${_testName}.c" ${objects})
    add_dependencies(${TARGET} jsdrv tinyprintf cmocka)
    target_link_libraries(${_testName} jsdrv tinyprintf cmocka)
    add_test(${_testName} ${CMAKE_CURRENT_BINARY_DIR}/${_testName})
endfunction (ADD_CMOCKA_TEST)

# For tests that live in a device subdirectory under test/devices/{name}/
function (ADD_CMOCKA_DEVICE_TEST _device _testName)
    set(TARGET ${_testName})
    set(_src "devices/${_device}/${_testName}.c")
    SET_FILENAME("${_src}")
    add_executable(${_testName} "${_src}" ${objects})
    add_dependencies(${TARGET} jsdrv tinyprintf cmocka)
    target_link_libraries(${_testName} jsdrv tinyprintf cmocka)
    add_test(${_testName} ${CMAKE_CURRENT_BINARY_DIR}/${_testName})
endfunction (ADD_CMOCKA_DEVICE_TEST)


ADD_CMOCKA_TEST(buffer_signal_test)

add_executable(buffer_test buffer_test.c ../src/buffer.c)
add_dependencies(buffer_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(buffer_test jsdrv_support_objlib tinyprintf cmocka)
add_test(buffer_test ${CMAKE_CURRENT_BINARY_DIR}/buffer_test)

ADD_CMOCKA_TEST(cstr_test)

add_executable(dbc_test dbc_test.c)
add_dependencies(dbc_test cmocka)
target_link_libraries(dbc_test cmocka)

ADD_CMOCKA_TEST(downsample_test)
ADD_CMOCKA_TEST(error_code_test)
ADD_CMOCKA_DEVICE_TEST(js110 js110_cal_test)
ADD_CMOCKA_DEVICE_TEST(js220 js220_i128_test)
ADD_CMOCKA_DEVICE_TEST(js110 js110_sp_test)
ADD_CMOCKA_DEVICE_TEST(js220 js220_stats_test)
ADD_CMOCKA_TEST(json_test)
ADD_CMOCKA_TEST(log_test)
ADD_CMOCKA_TEST(meta_test)
ADD_CMOCKA_TEST(meta_binary_test)
ADD_CMOCKA_TEST(sample_buffer_f32_test)
ADD_CMOCKA_TEST(statistics_test)
ADD_CMOCKA_TEST(time_test)
ADD_CMOCKA_TEST(time_map_filter_test)
ADD_CMOCKA_TEST(tmap_test)

add_executable(topic_test topic_test.c ../src/topic.c)
add_dependencies(topic_test cmocka)
target_link_libraries(topic_test cmocka)

ADD_CMOCKA_TEST(union_test)
ADD_CMOCKA_TEST(version_test)

ADD_CMOCKA_TEST(os_test)

add_executable(pubsub_test pubsub_test.c)
add_dependencies(pubsub_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(pubsub_test jsdrv_support_objlib tinyprintf cmocka)
add_test(pubsub_test ${CMAKE_CURRENT_BINARY_DIR}/pubsub_test)

add_executable(js320_fwup_test devices/js320/js320_fwup_test.c ../src/devices/js320/js320_fwup.c)
add_dependencies(js320_fwup_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(js320_fwup_test jsdrv_support_objlib tinyprintf cmocka)
add_test(js320_fwup_test ${CMAKE_CURRENT_BINARY_DIR}/js320_fwup_test)

add_executable(js320_fwup_mgr_test devices/js320/js320_fwup_mgr_test.c)
add_dependencies(js320_fwup_mgr_test jsdrv tinyprintf miniz cmocka)
target_link_libraries(js320_fwup_mgr_test jsdrv tinyprintf miniz cmocka)
add_test(js320_fwup_mgr_test ${CMAKE_CURRENT_BINARY_DIR}/js320_fwup_mgr_test)

ADD_CMOCKA_DEVICE_TEST(js320 js320_stats_test)

# js320_drv_test includes the driver source directly and stubs every
# mb_device, jtag, fwup, and msg-alloc dependency, so it only needs the
# support object library (cstr, topic, union, sample_buffer_f32, log, etc).
add_executable(js320_drv_test devices/js320/js320_drv_test.c)
add_dependencies(js320_drv_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(js320_drv_test jsdrv_support_objlib tinyprintf cmocka)
add_test(js320_drv_test ${CMAKE_CURRENT_BINARY_DIR}/js320_drv_test)

add_executable(frontend_test frontend_test.c
        ../src/buffer.c
        ../src/devices.c
        ../src/devices/js110/js110_usb.c
        ../src/devices/js220/js220_usb.c
        ../src/devices/js220/js220_params.c
        ../src/devices/js320/js320_drv.c
        ../src/devices/js320/js320_fwup.c
        ../src/devices/js320/js320_fwup_mgr.c
        ../src/devices/js320/js320_jtag.c
        ../src/devices/js320/js320_stats.c
        ../src/devices/mb_device/mb_device.c
        ../src/jsdrv.c)
set_target_properties(frontend_test PROPERTIES COMPILE_DEFINITIONS "UNITTEST=1;")
add_dependencies(frontend_test jsdrv_support_objlib tinyprintf miniz cmocka)
target_link_libraries(frontend_test jsdrv_support_objlib tinyprintf miniz cmocka)
add_test(frontend_test ${CMAKE_CURRENT_BINARY_DIR}/frontend_test)
