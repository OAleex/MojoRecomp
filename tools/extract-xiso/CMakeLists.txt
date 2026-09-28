cmake_minimum_required(VERSION 3.25)

project(MojoRecompExtractXiso LANGUAGES C)

set(EXTRACT_XISO_ROOT "" CACHE PATH "Pinned extract-xiso source checkout")
if(NOT EXTRACT_XISO_ROOT OR NOT EXISTS "${EXTRACT_XISO_ROOT}/extract-xiso.c")
    message(FATAL_ERROR "EXTRACT_XISO_ROOT must point at the pinned extract-xiso source checkout")
endif()

add_executable(extract-xiso
    "${EXTRACT_XISO_ROOT}/extract-xiso.c")

set_target_properties(extract-xiso PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES)

target_compile_definitions(extract-xiso PRIVATE
    WIN32
    NDEBUG
    _CONSOLE
    NO_FTP
    _CRT_SECURE_NO_WARNINGS)

if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(extract-xiso PRIVATE /O2)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(extract-xiso PRIVATE
        /clang:-std=gnu89
        /clang:-Wno-deprecated-declarations
        /clang:-Wno-deprecated-non-prototype)
endif()

target_link_libraries(extract-xiso PRIVATE ws2_32)
