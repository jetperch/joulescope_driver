add_library(miniz STATIC miniz.c)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
