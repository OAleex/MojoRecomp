cmake_minimum_required(VERSION 3.25)

project(MojoRecompRuntime LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MOJORECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(WIN32)
    enable_language(RC)
    file(TO_CMAKE_PATH
        "${MOJORECOMP_ROOT}/launcher/src-tauri/icons/icon.ico"
        MOJORECOMP_RUNTIME_ICON)
    configure_file(resources/mojorecomp_runtime.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/mojorecomp_runtime.rc" @ONLY)
endif()
set(MOJORECOMP_TOMLPLUSPLUS_ROOT
    "${MOJORECOMP_ROOT}/thirdparty/work/XenonRecomp/thirdparty/tomlplusplus/include")

set(MOJORECOMP_VERSION_FILE "${MOJORECOMP_ROOT}/version.toml")
if(NOT EXISTS "${MOJORECOMP_VERSION_FILE}")
    message(FATAL_ERROR "Missing suite version metadata: ${MOJORECOMP_VERSION_FILE}")
endif()
file(READ "${MOJORECOMP_VERSION_FILE}" MOJORECOMP_VERSION_TEXT)
function(mojorecomp_read_version KEY OUTPUT)
    string(REGEX MATCH "${KEY}[ \t]*=[ \t]*\"([^\"]+)\""
           _VERSION_MATCH "${MOJORECOMP_VERSION_TEXT}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "Missing '${KEY}' in ${MOJORECOMP_VERSION_FILE}")
    endif()
    set(${OUTPUT} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
mojorecomp_read_version("suite" MOJORECOMP_SUITE_VERSION)
mojorecomp_read_version("launcher" MOJORECOMP_LAUNCHER_VERSION)
mojorecomp_read_version("cot_runtime" MOJORECOMP_COT_RUNTIME_VERSION)
mojorecomp_read_version("mom_runtime" MOJORECOMP_MOM_RUNTIME_VERSION)
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${MOJORECOMP_ROOT}"
    RESULT_VARIABLE MOJORECOMP_GIT_RESULT
    OUTPUT_VARIABLE MOJORECOMP_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(MOJORECOMP_GIT_RESULT EQUAL 0 AND MOJORECOMP_GIT_SHA)
    set(MOJORECOMP_BUILD_ID "git-${MOJORECOMP_GIT_SHA}")
else()
    set(MOJORECOMP_BUILD_ID "unknown")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(version_generated.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mojorecomp_version.h" @ONLY)

# The pinned XenonUtils build uses the static MSVC runtime. Keep the translated
# image and all MojoRecomp executables on the same CRT so they can link directly
# without /FAILIFMISMATCH RuntimeLibrary conflicts.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The runtime intentionally uses C++ exceptions for guest-thread exits, probe
# failures and several existing host error paths. clang-cl does not enable
# exception handling merely because it targets the MSVC ABI, so make this an
# explicit project requirement instead of relying on an inherited cache flag.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_compile_options(/EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Keep optimized configurations optimized even when a local/reconfigured
    # CMake cache has empty CMAKE_CXX_FLAGS_<CONFIG> entries.  build-smoke was
    # accidentally regenerated that way and produced a nominal "Release"
    # runtime with no /O2 at all, which is catastrophic for the CPU-heavy PM4,
    # texture-coherency and vertex-preparation hot paths.
    add_compile_options(
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:Release>:/Ob2>"
        "$<$<CONFIG:Release>:/DNDEBUG>"
        "$<$<CONFIG:RelWithDebInfo>:/O2>"
        "$<$<CONFIG:RelWithDebInfo>:/Ob1>"
        "$<$<CONFIG:RelWithDebInfo>:/DNDEBUG>"
        "$<$<CONFIG:MinSizeRel>:/O1>"
        "$<$<CONFIG:MinSizeRel>:/Ob1>"
        "$<$<CONFIG:MinSizeRel>:/DNDEBUG>")
endif()

set(MOJORECOMP_LTO OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MOJORECOMP_LTO ON)
endif()

function(mojorecomp_enable_lto TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR
            "Unknown MojoRecomp target: ${TARGET_NAME}")
    endif()

    get_target_property(MOJORECOMP_OPT_TARGET_TYPE ${TARGET_NAME} TYPE)

    if(MOJORECOMP_LTO)
        target_compile_options(${TARGET_NAME} PRIVATE
            "$<$<CONFIG:Release>:-flto=thin>")
        if(NOT MOJORECOMP_OPT_TARGET_TYPE STREQUAL "STATIC_LIBRARY" AND
           NOT MOJORECOMP_OPT_TARGET_TYPE STREQUAL "OBJECT_LIBRARY")
            target_link_options(${TARGET_NAME} PRIVATE
                "$<$<CONFIG:Release>:-flto=thin>")
        endif()
    endif()

endfunction()

if(BUILD_TESTING)
    add_executable(MojoRecompStutterProfilerTest
        tests/stutter_profiler_test.cpp
        host/stutter_profiler.cpp)
    add_test(NAME StutterProfiler COMMAND MojoRecompStutterProfilerTest)
    set_tests_properties(StutterProfiler PROPERTIES TIMEOUT 10 LABELS "host;performance")
    add_executable(MojoRecompFrameRatePolicyTest tests/frame_rate_policy_test.cpp)
    add_test(NAME FrameRatePolicy COMMAND MojoRecompFrameRatePolicyTest)
    set_tests_properties(FrameRatePolicy PROPERTIES TIMEOUT 10 LABELS "host;timing")
    add_executable(MojoRecompTimingTest tests/timing_test.cpp)
    add_test(NAME HostTiming COMMAND MojoRecompTimingTest)
    set_tests_properties(HostTiming PROPERTIES TIMEOUT 10 LABELS "host;timing")
    add_executable(MojoRecompPresentationMetricsTest
        tests/presentation_metrics_test.cpp
        host/presentation_metrics.cpp)
    add_test(NAME PresentationMetrics COMMAND MojoRecompPresentationMetricsTest)
    set_tests_properties(PresentationMetrics PROPERTIES TIMEOUT 10 LABELS "host;timing;performance")
    add_executable(MojoRecompGpuExecutorTest
        tests/gpu_executor_test.cpp
        gpu/gpu_executor.cpp
        gpu/guest_memory_snapshot.cpp)
    add_test(NAME GpuExecutor COMMAND MojoRecompGpuExecutorTest)
    set_tests_properties(GpuExecutor PROPERTIES TIMEOUT 10 LABELS "gpu;performance")
    add_executable(MojoRecompGuestMemorySnapshotTest
        tests/guest_memory_snapshot_test.cpp
        gpu/guest_memory_snapshot.cpp)
    if(WIN32)
        target_link_libraries(MojoRecompGuestMemorySnapshotTest PRIVATE onecore)
    endif()
    add_test(NAME GuestMemorySnapshot COMMAND MojoRecompGuestMemorySnapshotTest)
    set_tests_properties(GuestMemorySnapshot PROPERTIES TIMEOUT 10 LABELS "gpu;memory")
    add_executable(MojoRecompShaderMetadataTest
        tests/shader_metadata_test.cpp
        gpu/shader_metadata.cpp)
    add_test(NAME ShaderMetadata COMMAND MojoRecompShaderMetadataTest)
    set_tests_properties(ShaderMetadata PROPERTIES TIMEOUT 10 LABELS "gpu;shader")
    add_executable(MojoRecompTextureAbiTest tests/texture_abi_test.cpp)
    add_test(NAME TextureAbi COMMAND MojoRecompTextureAbiTest)
    add_executable(MojoRecompDebugOverlayTest tests/debug_overlay_test.cpp)
    add_test(NAME DebugOverlay COMMAND MojoRecompDebugOverlayTest)
    add_executable(MojoRecompInputMergeTest tests/input_merge_test.cpp)
    add_test(NAME InputMerge COMMAND MojoRecompInputMergeTest)
    add_executable(MojoRecompPm4ExtentProbe tests/pm4_extent_probe.cpp gpu/pm4.cpp)
    add_test(NAME Pm4ExtentProbe COMMAND MojoRecompPm4ExtentProbe)
    set_tests_properties(Pm4ExtentProbe PROPERTIES TIMEOUT 10 LABELS "gpu")
    add_executable(MojoRecompVfsProbe tests/vfs_probe.cpp kernel/vfs.cpp kernel/klog.cpp)
    add_test(NAME VfsProbe COMMAND MojoRecompVfsProbe)
    set_tests_properties(VfsProbe PROPERTIES TIMEOUT 10 LABELS "kernel")
    add_executable(MojoRecompRuntimeConfigTest
        tests/runtime_config_test.cpp
        config/runtime_config.cpp)
    target_include_directories(MojoRecompRuntimeConfigTest PRIVATE
        "${MOJORECOMP_TOMLPLUSPLUS_ROOT}")
    add_test(NAME RuntimeConfig COMMAND MojoRecompRuntimeConfigTest)
    set_tests_properties(RuntimeConfig PROPERTIES TIMEOUT 10 LABELS "config")
    add_executable(MojoRecompRenderPolicyTest tests/render_policy_test.cpp)
    add_test(NAME RenderPolicy COMMAND MojoRecompRenderPolicyTest)
    set_tests_properties(RenderPolicy PROPERTIES TIMEOUT 10 LABELS "gpu")
    add_executable(MojoRecompDialogueCatalogTest
        tests/dialogue_catalog_test.cpp
        subtitles/dialogue_catalog.cpp)
    add_test(NAME DialogueCatalog COMMAND MojoRecompDialogueCatalogTest)
    set_tests_properties(DialogueCatalog PROPERTIES TIMEOUT 10 LABELS "subtitles")
    add_executable(MojoRecompRcfDiagnosticsTest
        tests/rcf_diagnostics_test.cpp
        kernel/rcf_diagnostics.cpp)
    add_test(NAME RcfDiagnostics COMMAND MojoRecompRcfDiagnosticsTest)
    set_tests_properties(RcfDiagnostics PROPERTIES TIMEOUT 10 LABELS "kernel;diagnostics")
endif()

set(MOJORECOMP_VULKAN_HEADERS "${MOJORECOMP_ROOT}/thirdparty/Vulkan-Headers/include")
set(MOJORECOMP_SDL3_SOURCE "${MOJORECOMP_ROOT}/thirdparty/SDL")
set(MOJORECOMP_SDL3_BUILD "${MOJORECOMP_ROOT}/thirdparty/build/SDL")
set(PPC_DIR "${MOJORECOMP_ROOT}/ppc" CACHE PATH "XenonRecomp output directory")
set(XENON_ROOT "${MOJORECOMP_ROOT}/thirdparty/work/XenonRecomp" CACHE PATH
    "MojoRecomp-patched XenonRecomp work tree")
set(XENOS_ROOT "${MOJORECOMP_ROOT}/thirdparty/work/XenosRecomp" CACHE PATH
    "MojoRecomp-patched XenosRecomp work tree")

if(WIN32 AND
   EXISTS "${MOJORECOMP_SDL3_SOURCE}/include/SDL3/SDL.h" AND
   EXISTS "${MOJORECOMP_SDL3_BUILD}/SDL3-static.lib")
    add_library(mojorecomp_sdl3 STATIC IMPORTED GLOBAL)
    set_target_properties(mojorecomp_sdl3 PROPERTIES
        IMPORTED_LOCATION "${MOJORECOMP_SDL3_BUILD}/SDL3-static.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${MOJORECOMP_SDL3_SOURCE}/include")
    target_link_libraries(mojorecomp_sdl3 INTERFACE
        kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid
        advapi32 setupapi shell32 hid dinput8)
    set(MOJORECOMP_HAS_SDL3 ON)
endif()

set(XENON_BUILD "${MOJORECOMP_ROOT}/thirdparty/build/XenonRecomp")
set(XENON_FMT "${XENON_BUILD}/thirdparty/fmt/fmt.lib")
set(XENON_XXHASH "${XENON_BUILD}/thirdparty/xxHash/cmake_unofficial/xxhash.lib")
set(CLANG_BUILTINS
    "${MOJORECOMP_ROOT}/thirdparty/llvm22/lib/clang/22/lib/windows/clang_rt.builtins-x86_64.lib")

set(MOJORECOMP_FFMPEG_ROOT "" CACHE PATH "Optional FFmpeg source/include root for XMA2 decoding")
set(MOJORECOMP_FFMPEG_LIBDIR "" CACHE PATH "Optional directory containing libavcodec.lib and mojorecomp-ffmpeg.dll")

# The previous ReXGlue bring-up proved this FFmpeg/XMA implementation. setup.bat
# now rebuilds the same narrow source set from the pinned public FFmpeg fork/commit
# through tools/ffmpeg-rexglue, removing the old opaque archived-library dependency.
set(MOJORECOMP_REXGLUE_FFMPEG_ROOT
    "${MOJORECOMP_ROOT}/thirdparty/FFmpeg")
set(MOJORECOMP_REXGLUE_FFMPEG_LIBDIR
    "${MOJORECOMP_ROOT}/thirdparty/build/FFmpeg")
if(NOT MOJORECOMP_FFMPEG_ROOT AND
   EXISTS "${MOJORECOMP_REXGLUE_FFMPEG_ROOT}/libavcodec/avcodec.h")
    set(MOJORECOMP_FFMPEG_ROOT "${MOJORECOMP_REXGLUE_FFMPEG_ROOT}")
endif()
if(NOT MOJORECOMP_FFMPEG_LIBDIR AND
   EXISTS "${MOJORECOMP_REXGLUE_FFMPEG_LIBDIR}/libavcodec.lib" AND
   EXISTS "${MOJORECOMP_REXGLUE_FFMPEG_LIBDIR}/mojorecomp-ffmpeg.dll")
    set(MOJORECOMP_FFMPEG_LIBDIR "${MOJORECOMP_REXGLUE_FFMPEG_LIBDIR}")
endif()

set(MOJORECOMP_HAS_FFMPEG OFF)
if(MOJORECOMP_FFMPEG_ROOT AND MOJORECOMP_FFMPEG_LIBDIR AND
   EXISTS "${MOJORECOMP_FFMPEG_ROOT}/libavcodec/avcodec.h" AND
   EXISTS "${MOJORECOMP_FFMPEG_LIBDIR}/libavcodec.lib" AND
   EXISTS "${MOJORECOMP_FFMPEG_LIBDIR}/mojorecomp-ffmpeg.dll")
    set(MOJORECOMP_HAS_FFMPEG ON)
endif()

# The runtime only needs XenonUtils' MIT-licensed image/XEX loader. Build that
# narrow source set here and keep libmspack's LGPL LZX implementation behind a
# replaceable DLL boundary instead of importing the upstream static archive,
# which folds lzxd.c into the runtime executable.
set(MOJORECOMP_LIBMSPACK_SOURCE
    "${XENON_ROOT}/thirdparty/libmspack")
set(LIBMSPACK_ROOT "${MOJORECOMP_LIBMSPACK_SOURCE}")
add_subdirectory(
    "${MOJORECOMP_ROOT}/tools/libmspack-lzx"
    "${CMAKE_CURRENT_BINARY_DIR}/libmspack-lzx")
set(MOJORECOMP_LIBMSPACK_ROOT
    "${MOJORECOMP_LIBMSPACK_SOURCE}/libmspack/mspack")

add_library(mojorecomp_xex_image STATIC
    "${XENON_ROOT}/XenonUtils/image.cpp"
    "${XENON_ROOT}/XenonUtils/xex.cpp"
    "${XENON_ROOT}/XenonUtils/xex_patcher.cpp"
    "${XENON_ROOT}/XenonUtils/memory_mapped_file.cpp"
    "${XENON_ROOT}/thirdparty/tiny-AES-c/aes.c")
target_compile_definitions(mojorecomp_xex_image PRIVATE NOMINMAX)
target_include_directories(mojorecomp_xex_image
    PUBLIC "${XENON_ROOT}/XenonUtils"
    PRIVATE
        "${MOJORECOMP_LIBMSPACK_ROOT}"
        "${XENON_ROOT}/thirdparty/tiny-AES-c"
        "${XENON_ROOT}/thirdparty/TinySHA1")
target_link_libraries(mojorecomp_xex_image PUBLIC mojorecomp_lzx)

if(NOT EXISTS "${PPC_DIR}/ppc_func_mapping.cpp")
    message(FATAL_ERROR
        "No XenonRecomp image in ${PPC_DIR}. Run tools/recompile.bat first.")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${MOJORECOMP_ROOT}/tools/gen_link_stubs.py"
    WORKING_DIRECTORY "${MOJORECOMP_ROOT}"
    RESULT_VARIABLE STUB_RESULT)
if(NOT STUB_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate phase-0 import stubs")
endif()

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/*.cpp")
list(LENGTH PPC_SOURCES PPC_TU_COUNT)
message(STATUS "Crash of the Titans translated image: ${PPC_TU_COUNT} C++ files")

if(MOJORECOMP_LTO)
    add_library(mojorecomp_ppc OBJECT ${PPC_SOURCES})
else()
    add_library(mojorecomp_ppc STATIC ${PPC_SOURCES})
endif()
mojorecomp_enable_lto(mojorecomp_ppc)
target_include_directories(mojorecomp_ppc PUBLIC
    "${PPC_DIR}"
    "${XENON_ROOT}/thirdparty/simde")
target_compile_options(mojorecomp_ppc PRIVATE
    -msse4.1 -mavx
    -Wno-unused-variable -Wno-unused-but-set-variable)

# XenonRecomp emits `mftb` as __rdtsc(). Force-include the MojoRecomp guest
# timebase over translated sources only so the title sees the Xbox 360's fixed
# 49.875 MHz clock instead of the host's multi-GHz TSC.
if(MSVC)
    target_compile_options(mojorecomp_ppc PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/cpu/timebase.h")
else()
    target_compile_options(mojorecomp_ppc PRIVATE
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/cpu/timebase.h")
endif()

# Xenos shader translation is linked in-process.  The title hands us raw shader
# microcode through PM4; this keeps translation out of an external-process path and
# gives the renderer a direct microcode -> HLSL -> SPIR-V seam.
if(NOT EXISTS "${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp")
    message(FATAL_ERROR "Pinned XenosRecomp checkout is missing shader_recompiler.cpp")
endif()

file(READ "${XENOS_ROOT}/XenosRecomp/shader_common.h" MOJO_SHADER_COMMON_SOURCE)
file(READ "${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp" MOJO_RECOMPILER_SOURCE)
if(NOT MOJO_RECOMPILER_SOURCE MATCHES "MOJORECOMP_XENOS_BOOL_EXEC")
    message(FATAL_ERROR "MojoRecomp Xenos runtime patch is incomplete (boolean-exec support missing). Run tools/apply-xenos-patches.bat for this XENOS_ROOT.")
endif()
foreach(MOJO_SHADER_COMMON_MARKER
        "g_TextureSampleScale"
        "g_PosScale"
        "g_PosDepthTransform"
        "g_LoopCount"
        "MojoQuantizeFloat24Depth")
    if(NOT MOJO_SHADER_COMMON_SOURCE MATCHES "${MOJO_SHADER_COMMON_MARKER}")
        message(FATAL_ERROR "MojoRecomp Xenos runtime patch is incomplete (${MOJO_SHADER_COMMON_MARKER} missing). Run tools/apply-xenos-patches.bat for this XENOS_ROOT.")
    endif()
endforeach()
foreach(MOJO_RECOMPILER_MARKER
        "g_TextureSampleScale"
        "g_PosScale"
        "g_PosDepthTransform"
        "g_LoopCount"
        "SPEC_CONSTANT_ALPHA_FUNC_SHIFT")
    if(NOT MOJO_RECOMPILER_SOURCE MATCHES "${MOJO_RECOMPILER_MARKER}")
        message(FATAL_ERROR "MojoRecomp Xenos runtime patch is incomplete (${MOJO_RECOMPILER_MARKER} integration missing). Run tools/apply-xenos-patches.bat for this XENOS_ROOT.")
    endif()
endforeach()
file(READ "${XENOS_ROOT}/XenosRecomp/shader_common.h" MOJO_SHADER_COMMON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MOJO_SHADER_COMMON_BYTES
       "${MOJO_SHADER_COMMON_HEX}")
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/mojorecomp_shader_common_embedded.h"
    CONTENT "// Generated from the pinned XenosRecomp shader_common.h.\n#pragma once\ninline constexpr unsigned char g_mojoShaderCommonH[] = {${MOJO_SHADER_COMMON_BYTES}};\ninline constexpr unsigned long g_mojoShaderCommonHSize = sizeof(g_mojoShaderCommonH);\n"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${XENOS_ROOT}/XenosRecomp/shader_common.h")

if(MOJORECOMP_LTO)
    add_library(mojorecomp_shader_xlate OBJECT
        gpu/shader_translator.cpp
        host/host_paths.cpp
        "${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp")
else()
    add_library(mojorecomp_shader_xlate STATIC
        gpu/shader_translator.cpp
        host/host_paths.cpp
        "${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp")
endif()
mojorecomp_enable_lto(mojorecomp_shader_xlate)
target_compile_definitions(mojorecomp_shader_xlate PRIVATE MOJORECOMP_XENOS_BOOL_EXEC)
target_include_directories(mojorecomp_shader_xlate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${XENOS_ROOT}/XenosRecomp"
    "${XENOS_ROOT}/thirdparty/dxc-bin/inc"
    "${XENON_ROOT}/thirdparty/fmt/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mojorecomp_shader_xlate PRIVATE "${XENON_FMT}")
if(WIN32)
    target_compile_definitions(mojorecomp_shader_xlate PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    set_source_files_properties("${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp"
        PROPERTIES COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/gpu/xenos_pch.h;-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic")
else()
    set_source_files_properties("${XENOS_ROOT}/XenosRecomp/shader_recompiler.cpp"
        PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/gpu/xenos_pch.h;-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic")
endif()

set(MOJORECOMP_KERNEL_SOURCES
    config/runtime_config.cpp
    debug_mode.cpp
    gpu/hardware_probe.cpp
    kernel/memory.cpp
    kernel/heap.cpp
    kernel/klog.cpp
    kernel/imports.cpp
    kernel/rcf_diagnostics.cpp
    kernel/unimplemented.cpp
    kernel/xex_loader.cpp
    kernel/vfs.cpp
    gpu/pm4.cpp
    gpu/renderer_probe.cpp
    gpu/draw_snapshot.cpp
    gpu/gpu_executor.cpp
    gpu/guest_memory_snapshot.cpp
    gpu/shader_cache.cpp
    gpu/shader_metadata.cpp
    gpu/vk_presenter.cpp
    gpu/vd.cpp
    host/input.cpp
    host/presentation_metrics.cpp
    host/stutter_profiler.cpp
    host/timing.cpp
    host/system_font_atlas.cpp
    host/window.cpp
    audio/xaudio.cpp
    audio/xma_decoder.cpp
    subtitles/dialogue_catalog.cpp
    subtitles/subtitle_runtime.cpp
    cpu/timebase.cpp
    cpu/guest_thread.cpp
    cpu/guest_fiber.cpp
    runtime_state.cpp
    title_resources.cpp
    title_hooks.cpp)

if(BUILD_TESTING AND WIN32)
    add_executable(MojoRecompWindowModeTest
        tests/window_mode_test.cpp
        host/window.cpp)
    target_link_libraries(MojoRecompWindowModeTest PRIVATE user32)
    add_test(NAME WindowMode COMMAND MojoRecompWindowModeTest)
    set_tests_properties(WindowMode PROPERTIES TIMEOUT 15 LABELS "host;window")

    add_executable(MojoRecompShaderAspectTransformTest
        tests/shader_aspect_transform_test.cpp)
    target_link_libraries(MojoRecompShaderAspectTransformTest PRIVATE mojorecomp_shader_xlate)
    add_test(NAME ShaderAspectTransform COMMAND MojoRecompShaderAspectTransformTest)
    set_tests_properties(ShaderAspectTransform PROPERTIES TIMEOUT 15 LABELS "shader;cpu")

    add_test(NAME RuntimeSingleInstance
        COMMAND MojoRecompBootProbe --single-instance-self-test)
    set_tests_properties(RuntimeSingleInstance PROPERTIES TIMEOUT 15 LABELS "host;process")

    add_test(NAME RuntimeCrashLogPath
        COMMAND MojoRecompBootProbe --crash-log-path-self-test)
    set_tests_properties(RuntimeCrashLogPath PROPERTIES TIMEOUT 15 LABELS "host;logging")

    add_test(NAME RuntimeCrashDump
        COMMAND MojoRecompBootProbe --crash-dump-self-test)
    set_tests_properties(RuntimeCrashDump PROPERTIES TIMEOUT 20 LABELS "host;logging;crash")

    add_test(NAME RuntimeWindowResize
        COMMAND MojoRecompBootProbe --window-resize-self-test)
    set_tests_properties(RuntimeWindowResize PROPERTIES TIMEOUT 20 LABELS "host;window;gpu")

    add_test(NAME RuntimeWindowClose
        COMMAND MojoRecompBootProbe --window-close-self-test)
    set_tests_properties(RuntimeWindowClose PROPERTIES TIMEOUT 10 LABELS "host;window;process")

    if(MOJORECOMP_HAS_SDL3)
        # Manual hardware probe only (not registered with CTest): useful for
        # checking native PlayStation/Xbox mappings without launching the game.
        add_executable(MojoRecompInputProbe EXCLUDE_FROM_ALL tests/input_probe.cpp)
        target_link_libraries(MojoRecompInputProbe PRIVATE
            mojorecomp_sdl3
            ucrt
            oldnames
            legacy_stdio_definitions)
        target_link_options(MojoRecompInputProbe PRIVATE "/NODEFAULTLIB:libucrt")
    endif()

    set(TEXTURE_PROBE_VS "${CMAKE_CURRENT_BINARY_DIR}/texture_probe_vs.spv")
    add_custom_command(OUTPUT "${TEXTURE_PROBE_VS}"
        COMMAND "${XENOS_ROOT}/thirdparty/dxc-bin/bin/x64/dxc.exe"
            -spirv -T vs_6_0 -E main -Fo "${TEXTURE_PROBE_VS}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_probe_vs.hlsl"
        DEPENDS tests/texture_probe_vs.hlsl VERBATIM)
    add_executable(MojoRecompTextureGpuProbe tests/texture_gpu_probe.cpp
        host/crash_reporter_stub.cpp
        gpu/shader_cache.cpp
        gpu/shader_metadata.cpp
        gpu/guest_memory_snapshot.cpp
        gpu/pm4.cpp
        kernel/klog.cpp
        host/presentation_metrics.cpp
        host/stutter_profiler.cpp
        host/window.cpp
        host/system_font_atlas.cpp
        config/runtime_config.cpp
        "${TEXTURE_PROBE_VS}")
    set(RECTANGLE_PROBE_VS "${CMAKE_CURRENT_BINARY_DIR}/rectangle_probe_vs.spv")
    add_custom_command(OUTPUT "${RECTANGLE_PROBE_VS}"
        COMMAND "${XENOS_ROOT}/thirdparty/dxc-bin/bin/x64/dxc.exe"
            -spirv -fspv-target-env=vulkan1.2 -T vs_6_0 -E main
            -I "${XENOS_ROOT}/XenosRecomp" -Fo "${RECTANGLE_PROBE_VS}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/rectangle_probe_vs.hlsl"
        DEPENDS tests/rectangle_probe_vs.hlsl "${XENOS_ROOT}/XenosRecomp/shader_common.h" VERBATIM)
    target_sources(MojoRecompTextureGpuProbe PRIVATE "${RECTANGLE_PROBE_VS}")
    target_include_directories(MojoRecompTextureGpuProbe PRIVATE
        "${MOJORECOMP_VULKAN_HEADERS}"
        "${MOJORECOMP_TOMLPLUSPLUS_ROOT}")
    target_link_libraries(MojoRecompTextureGpuProbe PRIVATE mojorecomp_shader_xlate)
    add_test(NAME ResolveVertexCoordinatesProbe COMMAND MojoRecompTextureGpuProbe --resolve-rect-cpu)
    set_tests_properties(ResolveVertexCoordinatesProbe PROPERTIES TIMEOUT 15 LABELS "cpu")
    add_test(NAME TextureGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}")
    set_tests_properties(TextureGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME AuthoredMipGpuProbe COMMAND MojoRecompTextureGpuProbe --authored-mips)
    set_tests_properties(AuthoredMipGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME FxaaOrientationGpuProbe COMMAND MojoRecompTextureGpuProbe --fxaa-orientation)
    set_tests_properties(FxaaOrientationGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu;postprocess")
    add_test(NAME RectangleGpuProbe COMMAND MojoRecompTextureGpuProbe "${RECTANGLE_PROBE_VS}" --rectangle)
    set_tests_properties(RectangleGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME ResolveSourceGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --resolve-source)
    add_test(NAME ResolveSwapGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --resolve-swap)
    add_test(NAME ResolveSwapRgbaGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --resolve-swap-rgba)
    set_tests_properties(ResolveSwapGpuProbe ResolveSwapRgbaGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    set_tests_properties(ResolveSourceGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME TiledResolveGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --tiled-resolve)
    set_tests_properties(TiledResolveGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME ConditionalExecGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --conditional-exec)
    set_tests_properties(ConditionalExecGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
    add_test(NAME ConditionalExecNotGpuProbe COMMAND MojoRecompTextureGpuProbe "${TEXTURE_PROBE_VS}" --conditional-exec-not)
    set_tests_properties(ConditionalExecNotGpuProbe PROPERTIES TIMEOUT 45 LABELS "gpu")
endif()

add_executable(MojoRecompSmoke
    smoke_main.cpp
    host/crash_reporter_stub.cpp
    ${MOJORECOMP_KERNEL_SOURCES}
    generated_import_stubs.cpp)
mojorecomp_enable_lto(MojoRecompSmoke)
target_include_directories(MojoRecompSmoke PRIVATE
    "${PPC_DIR}"
    "${MOJORECOMP_VULKAN_HEADERS}"
    "${MOJORECOMP_TOMLPLUSPLUS_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${XENON_ROOT}/XenonUtils"
    "${XENON_ROOT}/thirdparty/simde")
target_link_libraries(MojoRecompSmoke PRIVATE mojorecomp_ppc mojorecomp_shader_xlate)
if(WIN32)
    # memory.cpp uses the Windows 10 virtual-memory APIs exported through
    # onecore.lib. SDL3-static was built against the dynamic UCRT, while this
    # project deliberately compiles with /MT for the pinned Xenon libraries;
    # use the same import-UCRT bridge already proven by MojoRecompBootProbe.
    target_link_libraries(MojoRecompSmoke PRIVATE
        onecore
        gdi32
        ws2_32
        winmm
        ucrt
        oldnames
        legacy_stdio_definitions)
    target_link_options(MojoRecompSmoke PRIVATE "/NODEFAULTLIB:libucrt")
    if(MOJORECOMP_HAS_SDL3)
        target_link_libraries(MojoRecompSmoke PRIVATE mojorecomp_sdl3)
        target_compile_definitions(MojoRecompSmoke PRIVATE MOJORECOMP_HAS_SDL3=1)
    endif()
endif()

get_target_property(MOJORECOMP_PPC_TARGET_TYPE mojorecomp_ppc TYPE)
if(MOJORECOMP_PPC_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    if(MSVC)
        target_link_options(MojoRecompSmoke PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:mojorecomp_ppc>")
    else()
        target_link_options(MojoRecompSmoke PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:mojorecomp_ppc>" "-Wl,--no-whole-archive")
    endif()
endif()

if(WIN32 AND EXISTS "${CLANG_BUILTINS}")
    target_link_libraries(MojoRecompSmoke PRIVATE "${CLANG_BUILTINS}")
endif()

if(WIN32)
    add_executable(MojoRecompBootProbe
        boot_probe.cpp
        host/crash_reporter_windows.cpp
        tests/fiber_probe.cpp
        tests/event_probe.cpp
        ${MOJORECOMP_KERNEL_SOURCES}
        generated_import_stubs.cpp)
    mojorecomp_enable_lto(MojoRecompBootProbe)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(MojoRecompBootProbe PRIVATE
            "$<$<CONFIG:Release>:/Zi>")
        target_link_options(MojoRecompBootProbe PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
            "$<$<CONFIG:Release>:/PDBALTPATH:cot-runtime.pdb>"
            "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:/OPT:ICF>")
        set_target_properties(MojoRecompBootProbe PROPERTIES
            PDB_NAME "cot-runtime"
            PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    target_sources(MojoRecompBootProbe PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/mojorecomp_runtime.rc")
    target_include_directories(MojoRecompBootProbe PRIVATE
        "${PPC_DIR}"
        "${MOJORECOMP_VULKAN_HEADERS}"
        "${MOJORECOMP_TOMLPLUSPLUS_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${XENON_ROOT}/XenonUtils"
        "${XENON_ROOT}/thirdparty/simde"
        "${XENON_ROOT}/thirdparty/fmt/include"
        "${XENON_ROOT}/thirdparty/xxHash")
    if(MOJORECOMP_HAS_FFMPEG)
        target_include_directories(MojoRecompBootProbe PRIVATE "${MOJORECOMP_FFMPEG_ROOT}")
        target_compile_definitions(MojoRecompBootProbe PRIVATE MOJORECOMP_HAS_FFMPEG=1)
    endif()
    if(MOJORECOMP_HAS_SDL3)
        target_link_libraries(MojoRecompBootProbe PRIVATE mojorecomp_sdl3)
        target_compile_definitions(MojoRecompBootProbe PRIVATE MOJORECOMP_HAS_SDL3=1)
    endif()
    target_link_libraries(MojoRecompBootProbe PRIVATE
        mojorecomp_ppc
        mojorecomp_shader_xlate
        mojorecomp_xex_image
        "${XENON_FMT}"
        "${XENON_XXHASH}"
        "${CLANG_BUILTINS}"
        onecore
        gdi32
        ws2_32
        winmm
        dbghelp)
    if(MOJORECOMP_HAS_FFMPEG)
        target_link_libraries(MojoRecompBootProbe PRIVATE
            "${MOJORECOMP_FFMPEG_LIBDIR}/libavcodec.lib"
            ucrt
            oldnames
            legacy_stdio_definitions)
        # The imported FFmpeg libraries were built against the dynamic UCRT.
        # This target otherwise receives the static libucrt from clang-cl's
        # /MT runtime selection, which duplicates symbols once ucrt.lib is added.
        target_link_options(MojoRecompBootProbe PRIVATE "/NODEFAULTLIB:libucrt")
        add_custom_command(TARGET MojoRecompBootProbe POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MOJORECOMP_FFMPEG_LIBDIR}/mojorecomp-ffmpeg.dll"
                "$<TARGET_FILE_DIR:MojoRecompBootProbe>/mojorecomp-ffmpeg.dll"
            VERBATIM)
    endif()
    if(MOJORECOMP_PPC_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        target_link_options(MojoRecompBootProbe PRIVATE
            "/WHOLEARCHIVE:$<TARGET_FILE:mojorecomp_ppc>" "/MAP")
    else()
        target_link_options(MojoRecompBootProbe PRIVATE "/MAP")
    endif()
    add_custom_command(TARGET MojoRecompBootProbe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MojoRecompBootProbe>"
            "$<TARGET_FILE_DIR:MojoRecompBootProbe>/cot-runtime.exe"
        COMMENT "Creating public COT runtime alias cot-runtime.exe")
    if(BUILD_TESTING)
        add_test(NAME FiberProbe COMMAND MojoRecompBootProbe --fiber-test)
        set_tests_properties(FiberProbe PROPERTIES TIMEOUT 15 LABELS "cpu")
        set_tests_properties(FiberProbe PROPERTIES ENVIRONMENT "MOJORECOMP_NATIVE_FIBERS=1")
        add_test(NAME EventProbe COMMAND MojoRecompBootProbe --event-test)
        set_tests_properties(EventProbe PROPERTIES TIMEOUT 15 LABELS "kernel")
    endif()
endif()
