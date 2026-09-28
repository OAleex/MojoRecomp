cmake_minimum_required(VERSION 3.25)

# This narrow FFmpeg build is adapted from ReXGlue SDK v0.10.0
# (c94f5ebdcb3c9d1a460ca48e04f9758448f8d518), BSD-3-Clause. The source lists
# and codec/parser/bsf overlays intentionally match the Windows x86_64 build that
# produced MojoRecomp's previously archived FFmpeg libraries. See
# launcher/resources/licenses/ReXGlue-LICENSE.txt.
project(MojoRecompPinnedFFmpeg LANGUAGES C)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

file(TO_CMAKE_PATH "${FFMPEG_ROOT}" FFMPEG_ROOT)
if(NOT DEFINED FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/config_windows_x86_64.h")
    message(FATAL_ERROR "FFMPEG_ROOT must point at the pinned wmarti/FFmpeg checkout")
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MojoRecomp's pinned FFmpeg build currently supports Windows x86_64 only")
endif()

set(FFMPEG_OVERLAY "${CMAKE_CURRENT_SOURCE_DIR}/overlay")
set(FFMPEG_COMMON_DEFINES HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
set(FFMPEG_ATOMICS_INCLUDE "${FFMPEG_ROOT}/compat/atomics/win32")

# Keep libavutil as an object library and combine the selected FFmpeg code into
# one replaceable DLL.  The runtime therefore contains no FFmpeg object code,
# while FFmpeg's internal data references remain inside a single module.  This
# is the LGPL-2.1 section 6(b) shared-library boundary used by the release.
add_library(libavutil OBJECT
    ${FFMPEG_ROOT}/libavutil/adler32.c
    ${FFMPEG_ROOT}/libavutil/aes.c
    ${FFMPEG_ROOT}/libavutil/aes_ctr.c
    ${FFMPEG_ROOT}/libavutil/audio_fifo.c
    ${FFMPEG_ROOT}/libavutil/avstring.c
    ${FFMPEG_ROOT}/libavutil/avsscanf.c
    ${FFMPEG_ROOT}/libavutil/base64.c
    ${FFMPEG_ROOT}/libavutil/blowfish.c
    ${FFMPEG_ROOT}/libavutil/bprint.c
    ${FFMPEG_ROOT}/libavutil/buffer.c
    ${FFMPEG_ROOT}/libavutil/cast5.c
    ${FFMPEG_ROOT}/libavutil/camellia.c
    ${FFMPEG_ROOT}/libavutil/channel_layout.c
    ${FFMPEG_ROOT}/libavutil/color_utils.c
    ${FFMPEG_ROOT}/libavutil/cpu.c
    ${FFMPEG_ROOT}/libavutil/crc.c
    ${FFMPEG_ROOT}/libavutil/des.c
    ${FFMPEG_ROOT}/libavutil/dict.c
    ${FFMPEG_ROOT}/libavutil/display.c
    ${FFMPEG_ROOT}/libavutil/dovi_meta.c
    ${FFMPEG_ROOT}/libavutil/downmix_info.c
    ${FFMPEG_ROOT}/libavutil/encryption_info.c
    ${FFMPEG_ROOT}/libavutil/error.c
    ${FFMPEG_ROOT}/libavutil/eval.c
    ${FFMPEG_ROOT}/libavutil/fifo.c
    ${FFMPEG_ROOT}/libavutil/file.c
    ${FFMPEG_ROOT}/libavutil/file_open.c
    ${FFMPEG_ROOT}/libavutil/float_dsp.c
    ${FFMPEG_ROOT}/libavutil/fixed_dsp.c
    ${FFMPEG_ROOT}/libavutil/frame.c
    ${FFMPEG_ROOT}/libavutil/hash.c
    ${FFMPEG_ROOT}/libavutil/hdr_dynamic_metadata.c
    ${FFMPEG_ROOT}/libavutil/hmac.c
    ${FFMPEG_ROOT}/libavutil/hwcontext.c
    ${FFMPEG_ROOT}/libavutil/imgutils.c
    ${FFMPEG_ROOT}/libavutil/integer.c
    ${FFMPEG_ROOT}/libavutil/intmath.c
    ${FFMPEG_ROOT}/libavutil/lfg.c
    ${FFMPEG_ROOT}/libavutil/lls.c
    ${FFMPEG_ROOT}/libavutil/log.c
    ${FFMPEG_ROOT}/libavutil/log2_tab.c
    ${FFMPEG_ROOT}/libavutil/mathematics.c
    ${FFMPEG_ROOT}/libavutil/mastering_display_metadata.c
    ${FFMPEG_ROOT}/libavutil/md5.c
    ${FFMPEG_ROOT}/libavutil/mem.c
    ${FFMPEG_ROOT}/libavutil/murmur3.c
    ${FFMPEG_ROOT}/libavutil/opt.c
    ${FFMPEG_ROOT}/libavutil/parseutils.c
    ${FFMPEG_ROOT}/libavutil/pixdesc.c
    ${FFMPEG_ROOT}/libavutil/pixelutils.c
    ${FFMPEG_ROOT}/libavutil/random_seed.c
    ${FFMPEG_ROOT}/libavutil/rational.c
    ${FFMPEG_ROOT}/libavutil/reverse.c
    ${FFMPEG_ROOT}/libavutil/rc4.c
    ${FFMPEG_ROOT}/libavutil/ripemd.c
    ${FFMPEG_ROOT}/libavutil/samplefmt.c
    ${FFMPEG_ROOT}/libavutil/sha.c
    ${FFMPEG_ROOT}/libavutil/sha512.c
    ${FFMPEG_ROOT}/libavutil/slicethread.c
    ${FFMPEG_ROOT}/libavutil/spherical.c
    ${FFMPEG_ROOT}/libavutil/stereo3d.c
    ${FFMPEG_ROOT}/libavutil/threadmessage.c
    ${FFMPEG_ROOT}/libavutil/time.c
    ${FFMPEG_ROOT}/libavutil/timecode.c
    ${FFMPEG_ROOT}/libavutil/tree.c
    ${FFMPEG_ROOT}/libavutil/twofish.c
    ${FFMPEG_ROOT}/libavutil/utils.c
    ${FFMPEG_ROOT}/libavutil/xga_font_data.c
    ${FFMPEG_ROOT}/libavutil/xtea.c
    ${FFMPEG_ROOT}/libavutil/tea.c
    ${FFMPEG_ROOT}/libavutil/tx.c
    ${FFMPEG_ROOT}/libavutil/tx_float.c
    ${FFMPEG_ROOT}/libavutil/tx_double.c
    ${FFMPEG_ROOT}/libavutil/tx_int32.c
    ${FFMPEG_ROOT}/libavutil/video_enc_params.c
    ${FFMPEG_ROOT}/libavutil/film_grain_params.c
    ${FFMPEG_ROOT}/libavutil/x86/cpu.c
    ${FFMPEG_ROOT}/libavutil/x86/fixed_dsp_init.c
    ${FFMPEG_ROOT}/libavutil/x86/float_dsp_init.c
    ${FFMPEG_ROOT}/libavutil/x86/imgutils_init.c
    ${FFMPEG_ROOT}/libavutil/x86/lls_init.c)

target_include_directories(libavutil PUBLIC "${FFMPEG_ROOT}" PRIVATE "${FFMPEG_ATOMICS_INCLUDE}")
target_compile_definitions(libavutil PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavutil PRIVATE
    -w
    "/clang:-ffile-prefix-map=${FFMPEG_ROOT}=ffmpeg-source"
    "/clang:-fmacro-prefix-map=${FFMPEG_ROOT}=ffmpeg-source")

add_library(libavcodec SHARED
    $<TARGET_OBJECTS:libavutil>
    ${FFMPEG_ROOT}/libavcodec/ac3_parser.c
    ${FFMPEG_ROOT}/libavcodec/adts_parser.c
    ${FFMPEG_ROOT}/libavcodec/allcodecs.c
    ${FFMPEG_ROOT}/libavcodec/avcodec.c
    ${FFMPEG_ROOT}/libavcodec/avdct.c
    ${FFMPEG_ROOT}/libavcodec/avpacket.c
    ${FFMPEG_ROOT}/libavcodec/avpicture.c
    ${FFMPEG_ROOT}/libavcodec/bitstream.c
    ${FFMPEG_ROOT}/libavcodec/bitstream_filter.c
    ${FFMPEG_ROOT}/libavcodec/bitstream_filters.c
    ${FFMPEG_ROOT}/libavcodec/bsf.c
    ${FFMPEG_ROOT}/libavcodec/codec_desc.c
    ${FFMPEG_ROOT}/libavcodec/codec_par.c
    ${FFMPEG_ROOT}/libavcodec/d3d11va.c
    ${FFMPEG_ROOT}/libavcodec/decode.c
    ${FFMPEG_ROOT}/libavcodec/dirac.c
    ${FFMPEG_ROOT}/libavcodec/dv_profile.c
    ${FFMPEG_ROOT}/libavcodec/encode.c
    ${FFMPEG_ROOT}/libavcodec/imgconvert.c
    ${FFMPEG_ROOT}/libavcodec/jni.c
    ${FFMPEG_ROOT}/libavcodec/mathtables.c
    ${FFMPEG_ROOT}/libavcodec/mediacodec.c
    ${FFMPEG_ROOT}/libavcodec/mpeg12framerate.c
    ${FFMPEG_ROOT}/libavcodec/options.c
    ${FFMPEG_ROOT}/libavcodec/parser.c
    ${FFMPEG_ROOT}/libavcodec/parsers.c
    ${FFMPEG_ROOT}/libavcodec/profiles.c
    ${FFMPEG_ROOT}/libavcodec/qsv_api.c
    ${FFMPEG_ROOT}/libavcodec/raw.c
    ${FFMPEG_ROOT}/libavcodec/utils.c
    ${FFMPEG_ROOT}/libavcodec/vorbis_parser.c
    ${FFMPEG_ROOT}/libavcodec/xiph.c
    ${FFMPEG_ROOT}/libavcodec/faandct.c
    ${FFMPEG_ROOT}/libavcodec/faanidct.c
    ${FFMPEG_ROOT}/libavcodec/fdctdsp.c
    ${FFMPEG_ROOT}/libavcodec/jfdctfst.c
    ${FFMPEG_ROOT}/libavcodec/jfdctint.c
    ${FFMPEG_ROOT}/libavcodec/idctdsp.c
    ${FFMPEG_ROOT}/libavcodec/simple_idct.c
    ${FFMPEG_ROOT}/libavcodec/jrevdct.c
    ${FFMPEG_ROOT}/libavcodec/mdct_float.c
    ${FFMPEG_ROOT}/libavcodec/mdct_fixed_32.c
    ${FFMPEG_ROOT}/libavcodec/sinewin.c
    ${FFMPEG_ROOT}/libavcodec/wma_freqs.c
    ${FFMPEG_ROOT}/libavcodec/wmaprodec.c
    ${FFMPEG_ROOT}/libavcodec/wmadec.c
    ${FFMPEG_ROOT}/libavcodec/wma.c
    ${FFMPEG_ROOT}/libavcodec/wma_common.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudio.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodata.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodecheader.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodec_common.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodec_float.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodec_fixed.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodsp.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodsp_data.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodsp_float.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudiodsp_fixed.c
    ${FFMPEG_ROOT}/libavcodec/mpegaudio_parser.c
    ${FFMPEG_ROOT}/libavcodec/dct.c
    ${FFMPEG_ROOT}/libavcodec/dct32_float.c
    ${FFMPEG_ROOT}/libavcodec/dct32_fixed.c
    ${FFMPEG_ROOT}/libavcodec/rdft.c
    ${FFMPEG_ROOT}/libavcodec/aactab.c
    ${FFMPEG_ROOT}/libavcodec/null_bsf.c
    ${FFMPEG_ROOT}/libavcodec/pthread.c
    ${FFMPEG_ROOT}/libavcodec/pthread_slice.c
    ${FFMPEG_ROOT}/libavcodec/pthread_frame.c
    ${FFMPEG_ROOT}/libavcodec/avfft.c
    ${FFMPEG_ROOT}/libavcodec/fft_float.c
    ${FFMPEG_ROOT}/libavcodec/fft_fixed_32.c
    ${FFMPEG_ROOT}/libavcodec/fft_init_table.c
    ${FFMPEG_ROOT}/libavcodec/x86/constants.c
    ${FFMPEG_ROOT}/libavcodec/x86/fdctdsp_init.c
    ${FFMPEG_ROOT}/libavcodec/x86/fft_init.c
    ${FFMPEG_ROOT}/libavcodec/x86/idctdsp_init.c
    ${FFMPEG_ROOT}/libavcodec/x86/fdct.c
    ${FFMPEG_ROOT}/libavcodec/x86/mpegaudiodsp.c
    ${FFMPEG_ROOT}/libavcodec/x86/dct_init.c)

target_include_directories(libavcodec BEFORE PRIVATE
    "${FFMPEG_OVERLAY}")
target_include_directories(libavcodec PUBLIC "${FFMPEG_ROOT}" PRIVATE "${FFMPEG_ATOMICS_INCLUDE}")
target_compile_definitions(libavcodec PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavcodec PRIVATE
    -w
    "/clang:-ffile-prefix-map=${FFMPEG_ROOT}=ffmpeg-source"
    "/clang:-fmacro-prefix-map=${FFMPEG_ROOT}=ffmpeg-source")
target_link_libraries(libavcodec PRIVATE bcrypt)

set_target_properties(libavutil libavcodec PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
set_target_properties(libavcodec PROPERTIES
    RUNTIME_OUTPUT_NAME "mojorecomp-ffmpeg"
    ARCHIVE_OUTPUT_NAME "libavcodec")
