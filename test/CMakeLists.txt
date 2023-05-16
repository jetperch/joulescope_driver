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

include_directories(${CMOCKA_INCLUDE})

function (ADD_CMOCKA_TEST _testName)
    set(TARGET ${_testName})
    SET_FILENAME("${_testName}.c")
    add_executable(${_testName} "${_testName}.c" ${objects})
    add_dependencies(${TARGET} jsdrv_objlib jsdrv_support_objlib tinyprintf cmocka ${PLATFORM_DEPENDENCIES})
    target_link_libraries(${_testName} jsdrv_objlib jsdrv_support_objlib tinyprintf cmocka ${PLATFORM_LIBS})
    add_test(${_testName} ${CMAKE_CURRENT_BINARY_DIR}/${_testName})
endfunction (ADD_CMOCKA_TEST)


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
ADD_CMOCKA_TEST(js110_cal_test)
ADD_CMOCKA_TEST(js220_i128_test)
ADD_CMOCKA_TEST(js110_sp_test)
ADD_CMOCKA_TEST(js220_stats_test)
ADD_CMOCKA_TEST(json_test)
ADD_CMOCKA_TEST(log_test)
ADD_CMOCKA_TEST(meta_test)
ADD_CMOCKA_TEST(sample_buffer_f32_test)
ADD_CMOCKA_TEST(statistics_test)
ADD_CMOCKA_TEST(time_test)

add_executable(topic_test topic_test.c ../src/topic.c)
add_dependencies(topic_test cmocka)
target_link_libraries(topic_test cmocka)

ADD_CMOCKA_TEST(union_test)
ADD_CMOCKA_TEST(version_test)

add_executable(pubsub_test pubsub_test.c)
add_dependencies(pubsub_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(pubsub_test jsdrv_support_objlib tinyprintf cmocka)
add_test(pubsub_test ${CMAKE_CURRENT_BINARY_DIR}/pubsub_test)

add_executable(frontend_test frontend_test.c
        ../src/buffer.c
        ../src/js110_usb.c
        ../src/js220_usb.c
        ../src/js220_params.c
        ../src/jsdrv.c)
set_target_properties(frontend_test PROPERTIES COMPILE_DEFINITIONS "UNITTEST=1;")
add_dependencies(frontend_test jsdrv_support_objlib tinyprintf cmocka)
target_link_libraries(frontend_test jsdrv_support_objlib tinyprintf cmocka)
add_test(frontend_test ${CMAKE_CURRENT_BINARY_DIR}/frontend_test)

SET_FILENAME("jsdrv_util.c")
add_executable(jsdrv_util
        jsdrv_util.c
        jsdrv_util/demo.c
        jsdrv_util/dev.c
        jsdrv_util/hotplug.c
        jsdrv_util/info.c
        jsdrv_util/mem_erase.c
        jsdrv_util/mem_read.c
        jsdrv_util/mem_write.c
        jsdrv_util/reset.c
        jsdrv_util/scan.c
        jsdrv_util/set.c
        jsdrv_util/statistics.c
        jsdrv_util/stream_buffer.c
        jsdrv_util/threads.c
        jsdrv_util/version.c
        )
add_dependencies(jsdrv_util jsdrv)
target_link_libraries(jsdrv_util jsdrv)
