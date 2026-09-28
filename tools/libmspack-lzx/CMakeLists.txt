cmake_minimum_required(VERSION 3.25)

project(MojoRecompLibmspackLzx LANGUAGES C)

if(NOT DEFINED LIBMSPACK_ROOT OR
   NOT EXISTS "${LIBMSPACK_ROOT}/libmspack/mspack/lzxd.c")
    message(FATAL_ERROR
        "LIBMSPACK_ROOT must point at the pinned libmspack repository")
endif()

set(LIBMSPACK_MSPACK_ROOT "${LIBMSPACK_ROOT}/libmspack/mspack")
add_library(mojorecomp_lzx SHARED
    "${LIBMSPACK_MSPACK_ROOT}/lzxd.c")
target_include_directories(mojorecomp_lzx PRIVATE
    "${LIBMSPACK_MSPACK_ROOT}")
set_target_properties(mojorecomp_lzx PROPERTIES
    OUTPUT_NAME "mojorecomp-lzx"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    C_VISIBILITY_PRESET hidden)
