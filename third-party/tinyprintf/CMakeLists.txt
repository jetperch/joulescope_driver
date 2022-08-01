
add_library(tinyprintf OBJECT tinyprintf.c)
target_include_directories(tinyprintf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
