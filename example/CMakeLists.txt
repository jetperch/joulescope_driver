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


add_executable(fuzz fuzz.c)
add_dependencies(fuzz jsdrv)
target_link_libraries(fuzz jsdrv)
