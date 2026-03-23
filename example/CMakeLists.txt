# Copyright 2014-2023 Jetperch LLC
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

add_executable(jsdrv_exe
        jsdrv/jsdrv.c
        jsdrv/capture.c
        jsdrv/demo.c
        jsdrv/dev.c
        jsdrv/hotplug.c
        jsdrv/info.c
        jsdrv/mem_erase.c
        jsdrv/mem_read.c
        jsdrv/mem_write.c
        jsdrv/reset.c
        jsdrv/scan.c
        jsdrv/set.c
        jsdrv/statistics.c
        jsdrv/stream_buffer.c
        jsdrv/threads.c
        jsdrv/version.c
)
add_dependencies(jsdrv_exe jsdrv)
target_link_libraries(jsdrv_exe jsdrv)
set_target_properties(jsdrv_exe PROPERTIES OUTPUT_NAME jsdrv)

add_definitions(
        -DTRACY_ENABLE=1
        -DTRACY_ON_DEMAND=1
        -DTRACY_NO_CALLSTACK=1
        -DTRACY_NO_CODE_TRANSFER=1
        -DTRACY_NO_CONTEXT_SWITCH=1
        -DTRACY_NO_EXIT=1
        -DTRACY_NO_SAMPLING=1
        -DTRACY_NO_VERIFY=1
        -DTRACY_NO_VSYNC_CAPTURE=1
        -DTRACY_NO_FRAME_IMAGE=1
        -DTRACY_NO_SYSTEM_TRACING=1
        -DTRACY_DELAYED_INIT=out.dat
        -DTRACY_MANUAL_LIFETIME=1
        -DTRACY_NO_CRASH_HANDLER=1
        -DTRACY_VERBOSE=1
)
add_executable(minibitty_exe
        minibitty/main.cpp
        minibitty/adapter.c
        minibitty/firmware.c
        minibitty/fpga_mem.c
        minibitty/hotplug.c
        minibitty/info.c
        minibitty/pubsub_info.c
        minibitty/pubsub_sniffer.c
        minibitty/pubsub_test.c
        minibitty/loopback.c
        minibitty/mem.c
        minibitty/stream.c
        minibitty/throughput.c
        minibitty/version.c
        minibitty/adapter_tracy.cpp
        minibitty/tracy/common/tracy_lz4.cpp
        minibitty/tracy/common/TracySocket.cpp
)
target_compile_features(minibitty_exe PUBLIC cxx_std_11)
#set_target_properties(minibitty_exe PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
add_dependencies(minibitty_exe jsdrv)
target_link_libraries(minibitty_exe jsdrv ws2_32 dbghelp advapi32 user32)
set_target_properties(minibitty_exe PROPERTIES OUTPUT_NAME minibitty)

add_executable(fuzz fuzz.c)
add_dependencies(fuzz jsdrv)
target_link_libraries(fuzz jsdrv)
