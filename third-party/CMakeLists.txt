

if (NOT CMAKE_CROSSCOMPILING)
    SET(cmocka_dir ${CMAKE_CURRENT_SOURCE_DIR}/cmocka)
    include(CheckCCompilerFlag)
    include(${cmocka_dir}/ConfigureChecks.cmake)
    configure_file(${cmocka_dir}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)
    set(CMOCKA_INCLUDES ${CMAKE_CURRENT_BINARY_DIR} ${cmocka_dir}/include)
    if (CMAKE_COMPILER_IS_GNUCC)
        add_definitions(-Wno-format)
    endif()
    add_library(cmocka STATIC ${cmocka_dir}/src/cmocka.c)
    target_compile_definitions(cmocka PRIVATE -DHAVE_CONFIG_H=1)
    target_include_directories(cmocka PUBLIC ${CMOCKA_INCLUDES})
endif()


add_subdirectory(tinyprintf)
