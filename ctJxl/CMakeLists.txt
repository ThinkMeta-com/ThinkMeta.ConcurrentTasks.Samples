# ctJxl on Linux (clang, Ninja): libjxl 0.13.0 through its own CMake, with the eleven encoder files of ctJxl.Encoder in
# place of its own, the runner and the task graph (ctJxl.Tasks) and the program. Windows builds use the .vcxproj files
# of this folder and of jxl/libjxl-0.13.0/msvc; this file never takes part in them. On its own:
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#
# (or included by the CMakeLists.txt of the samples of the API repository, after Samples.Shared). NASM builds the SIMD
# code of libjpeg-turbo, as in the Windows build.
#
# Target: ctJxl (the program, in bin/ of the build folder). Like cjxl, ctJxl chooses its Highway target (SSE2 to AVX2)
# at run time and runs on any x64 CPU: there is no -march here, unlike the Windows build (/arch:AVX2), so it needs no
# CPU check. The unit tests (ctJxl.Tests) are Windows only.
#
# The input decoders are built here from fixed file lists, as the projects in jxl/libjxl-0.13.0/msvc build them: zlib and
# libpng from jxl/libjxl-0.13.0/third_party, giflib 5.2.2 and libjpeg-turbo 3.2.0. libjxl's own bundling of libpng
# (JPEGXL_BUNDLE_LIBPNG) stays off, as it writes into the source tree; libjxl finds the four through find_package,
# pointed here at these targets and at one folder with their public headers. All code is compiled without contraction
# of a*b+c into FMA (-ffp-contract=off), as the oracle (jxl/cjxl-oracle, bin/linux-x64/cjxl).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ctJxl LANGUAGES C CXX ASM_NASM)
    include(${CMAKE_CURRENT_LIST_DIR}/../Shared/Standalone.cmake)
else()
    enable_language(C)
    enable_language(ASM_NASM)
endif()

set(CT_JXL_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CT_JXL_APP_DIR ${CT_JXL_DIR}/src/App)
set(CT_JXL_ENCODER_DIR ${CT_JXL_DIR}/src/Encoder)
set(CT_JXL_TASKS_DIR ${CT_JXL_DIR}/src/Tasks)
set(CT_JXL_LIBJXL_DIR ${CT_JXL_DIR}/jxl/libjxl-0.13.0)
set(CT_JXL_GIFLIB_DIR ${CT_JXL_DIR}/jxl/giflib-5.2.2)
set(CT_JXL_LIBJPEG_DIR ${CT_JXL_DIR}/jxl/libjpeg-turbo-3.2.0)
cmake_path(NORMAL_PATH CT_JXL_LIBJPEG_DIR)

# --- vendor code: no -Wall/-Wextra of the samples, only -ffp-contract=off (restored after libjxl) ---

get_directory_property(ct_jxl_compile_options COMPILE_OPTIONS)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>")

# the public headers of the four decoders in one folder, for libjxl's find_package calls
set(CT_JXL_DEPS_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/deps/include)
set(CT_JXL_ZLIB_DIR ${CT_JXL_LIBJXL_DIR}/third_party/zlib)
set(CT_JXL_LIBPNG_DIR ${CT_JXL_LIBJXL_DIR}/third_party/libpng)
file(COPY ${CT_JXL_ZLIB_DIR}/zlib.h ${CT_JXL_ZLIB_DIR}/zconf.h
          ${CT_JXL_LIBPNG_DIR}/png.h ${CT_JXL_LIBPNG_DIR}/pngconf.h
          ${CT_JXL_GIFLIB_DIR}/gif_lib.h
          ${CT_JXL_LIBJPEG_DIR}/src/jpeglib.h ${CT_JXL_LIBJPEG_DIR}/src/jmorecfg.h ${CT_JXL_LIBJPEG_DIR}/src/jerror.h
     DESTINATION ${CT_JXL_DEPS_INCLUDE})
# libpng's configuration without awk, as in libpng's own CMake and jxl/libjxl-0.13.0/msvc/include-gen/libpng
configure_file(${CT_JXL_LIBPNG_DIR}/scripts/pnglibconf.h.prebuilt ${CT_JXL_DEPS_INCLUDE}/pnglibconf.h COPYONLY)

# zlib (jxl/libjxl-0.13.0/msvc/libjxl-zlib.vcxproj)
add_library(ctjxl_zlib STATIC
    ${CT_JXL_ZLIB_DIR}/adler32.c ${CT_JXL_ZLIB_DIR}/compress.c ${CT_JXL_ZLIB_DIR}/crc32.c ${CT_JXL_ZLIB_DIR}/deflate.c
    ${CT_JXL_ZLIB_DIR}/gzclose.c ${CT_JXL_ZLIB_DIR}/gzlib.c ${CT_JXL_ZLIB_DIR}/gzread.c ${CT_JXL_ZLIB_DIR}/gzwrite.c
    ${CT_JXL_ZLIB_DIR}/inflate.c ${CT_JXL_ZLIB_DIR}/infback.c ${CT_JXL_ZLIB_DIR}/inftrees.c ${CT_JXL_ZLIB_DIR}/inffast.c
    ${CT_JXL_ZLIB_DIR}/trees.c ${CT_JXL_ZLIB_DIR}/uncompr.c ${CT_JXL_ZLIB_DIR}/zutil.c)
target_include_directories(ctjxl_zlib PUBLIC ${CT_JXL_DEPS_INCLUDE})
target_compile_definitions(ctjxl_zlib PUBLIC HAVE_UNISTD_H)

# libpng with its SSE2 filters (jxl/libjxl-0.13.0/msvc/libjxl-png.vcxproj)
add_library(ctjxl_png STATIC
    ${CT_JXL_LIBPNG_DIR}/png.c ${CT_JXL_LIBPNG_DIR}/pngerror.c ${CT_JXL_LIBPNG_DIR}/pngget.c
    ${CT_JXL_LIBPNG_DIR}/pngmem.c ${CT_JXL_LIBPNG_DIR}/pngpread.c ${CT_JXL_LIBPNG_DIR}/pngread.c
    ${CT_JXL_LIBPNG_DIR}/pngrio.c ${CT_JXL_LIBPNG_DIR}/pngrtran.c ${CT_JXL_LIBPNG_DIR}/pngrutil.c
    ${CT_JXL_LIBPNG_DIR}/pngset.c ${CT_JXL_LIBPNG_DIR}/pngtrans.c ${CT_JXL_LIBPNG_DIR}/pngwio.c
    ${CT_JXL_LIBPNG_DIR}/pngwrite.c ${CT_JXL_LIBPNG_DIR}/pngwtran.c ${CT_JXL_LIBPNG_DIR}/pngwutil.c
    ${CT_JXL_LIBPNG_DIR}/intel/intel_init.c ${CT_JXL_LIBPNG_DIR}/intel/filter_sse2_intrinsics.c)
target_compile_definitions(ctjxl_png PRIVATE PNG_INTEL_SSE_OPT=1)
target_link_libraries(ctjxl_png PUBLIC ctjxl_zlib)

# giflib (jxl/giflib-5.2.2/giflib.vcxproj)
add_library(ctjxl_gif STATIC
    ${CT_JXL_GIFLIB_DIR}/dgif_lib.c ${CT_JXL_GIFLIB_DIR}/egif_lib.c ${CT_JXL_GIFLIB_DIR}/gifalloc.c
    ${CT_JXL_GIFLIB_DIR}/gif_err.c ${CT_JXL_GIFLIB_DIR}/gif_font.c ${CT_JXL_GIFLIB_DIR}/gif_hash.c
    ${CT_JXL_GIFLIB_DIR}/openbsd-reallocarray.c)
target_include_directories(ctjxl_gif PUBLIC ${CT_JXL_DEPS_INCLUDE})

# libjpeg-turbo with its x86-64 SIMD code (jxl/libjxl-0.13.0/msvc/libjxl-jpeg-turbo.vcxproj, the file set of the upstream
# target jpeg-static); its configuration headers from its own templates, with the values of its CMake on Linux
block(SCOPE_FOR VARIABLES)
    set(JPEG_LIB_VERSION 62)
    set(VERSION 3.2.0)
    set(LIBJPEG_TURBO_VERSION_NUMBER 3002000)
    set(C_ARITH_CODING_SUPPORTED 1)
    set(D_ARITH_CODING_SUPPORTED 1)
    set(WITH_SIMD 1)
    set(BUILD 20261007) # the build number of libjpeg-turbo-3.2.0/msvc/jconfigint.h
    set(HIDDEN "__attribute__((visibility(\"hidden\")))")
    set(INLINE "__inline__ __attribute__((always_inline))")
    set(THREAD_LOCAL __thread)
    set(CMAKE_PROJECT_NAME libjpeg-turbo)
    set(SIZE_T 8)
    set(HAVE_BUILTIN_CTZL 1)
    set(SIMD_ARCHITECTURE X86_64)
    set(COPYRIGHT_YEAR 1991-2026)
    configure_file(${CT_JXL_LIBJPEG_DIR}/src/jconfig.h.in ${CT_JXL_DEPS_INCLUDE}/jconfig.h)
    configure_file(${CT_JXL_LIBJPEG_DIR}/src/jconfigint.h.in ${CMAKE_CURRENT_BINARY_DIR}/deps/jpeg/jconfigint.h)
    configure_file(${CT_JXL_LIBJPEG_DIR}/src/jversion.h.in ${CMAKE_CURRENT_BINARY_DIR}/deps/jpeg/jversion.h)
endblock()
set(ct_jxl_jpeg_sources
    jcapimin.c wrapper/jcapistd-8.c wrapper/jcapistd-12.c wrapper/jcapistd-16.c wrapper/jccoefct-8.c
    wrapper/jccoefct-12.c wrapper/jccolor-8.c wrapper/jccolor-12.c wrapper/jccolor-16.c wrapper/jcdctmgr-8.c
    wrapper/jcdctmgr-12.c wrapper/jcdiffct-8.c wrapper/jcdiffct-12.c wrapper/jcdiffct-16.c jchuff.c jcicc.c jcinit.c
    jclhuff.c wrapper/jclossls-8.c wrapper/jclossls-12.c wrapper/jclossls-16.c wrapper/jcmainct-8.c
    wrapper/jcmainct-12.c wrapper/jcmainct-16.c jcmarker.c jcmaster.c jcomapi.c jcparam.c jcphuff.c
    wrapper/jcprepct-8.c wrapper/jcprepct-12.c wrapper/jcprepct-16.c wrapper/jcsample-8.c wrapper/jcsample-12.c
    wrapper/jcsample-16.c jctrans.c jdapimin.c wrapper/jdapistd-8.c wrapper/jdapistd-12.c wrapper/jdapistd-16.c
    jdatadst.c jdatasrc.c wrapper/jdcoefct-8.c wrapper/jdcoefct-12.c wrapper/jdcolor-8.c wrapper/jdcolor-12.c
    wrapper/jdcolor-16.c wrapper/jddctmgr-8.c wrapper/jddctmgr-12.c wrapper/jddiffct-8.c wrapper/jddiffct-12.c
    wrapper/jddiffct-16.c jdhuff.c jdicc.c jdinput.c jdlhuff.c wrapper/jdlossls-8.c wrapper/jdlossls-12.c
    wrapper/jdlossls-16.c wrapper/jdmainct-8.c wrapper/jdmainct-12.c wrapper/jdmainct-16.c jdmarker.c jdmaster.c
    wrapper/jdmerge-8.c wrapper/jdmerge-12.c jdphuff.c wrapper/jdpostct-8.c wrapper/jdpostct-12.c
    wrapper/jdpostct-16.c wrapper/jdsample-8.c wrapper/jdsample-12.c wrapper/jdsample-16.c jdtrans.c jerror.c
    jfdctflt.c wrapper/jfdctfst-8.c wrapper/jfdctfst-12.c wrapper/jfdctint-8.c wrapper/jfdctint-12.c
    wrapper/jidctflt-8.c wrapper/jidctflt-12.c wrapper/jidctfst-8.c wrapper/jidctfst-12.c wrapper/jidctint-8.c
    wrapper/jidctint-12.c wrapper/jidctred-8.c wrapper/jidctred-12.c jmemmgr.c jmemnobs.c jpeg_nbits.c
    wrapper/jquant1-8.c wrapper/jquant1-12.c wrapper/jquant2-8.c wrapper/jquant2-12.c wrapper/jutils-8.c
    wrapper/jutils-12.c wrapper/jutils-16.c jaricom.c jcarith.c jdarith.c)
list(TRANSFORM ct_jxl_jpeg_sources PREPEND ${CT_JXL_LIBJPEG_DIR}/src/)
set(ct_jxl_jpeg_simd
    jsimdcpu.asm jfdctflt-sse.asm jccolor-sse2.asm jcgray-sse2.asm jchuff-sse2.asm jcphuff-sse2.asm jcsample-sse2.asm
    jdcolor-sse2.asm jdmerge-sse2.asm jdsample-sse2.asm jfdctfst-sse2.asm jfdctint-sse2.asm jidctflt-sse2.asm
    jidctfst-sse2.asm jidctint-sse2.asm jidctred-sse2.asm jquantf-sse2.asm jquanti-sse2.asm jccolor-avx2.asm
    jcgray-avx2.asm jcsample-avx2.asm jdcolor-avx2.asm jdmerge-avx2.asm jdsample-avx2.asm jfdctint-avx2.asm
    jidctint-avx2.asm jquanti-avx2.asm)
list(TRANSFORM ct_jxl_jpeg_simd PREPEND ${CT_JXL_LIBJPEG_DIR}/simd/x86_64/)
add_library(ctjxl_jpeg STATIC ${ct_jxl_jpeg_sources} ${CT_JXL_LIBJPEG_DIR}/simd/jsimd.c ${ct_jxl_jpeg_simd})
target_include_directories(ctjxl_jpeg PUBLIC ${CT_JXL_DEPS_INCLUDE})
target_include_directories(ctjxl_jpeg PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/deps/jpeg ${CT_JXL_LIBJPEG_DIR}/src
                                              ${CT_JXL_LIBJPEG_DIR}/simd)
target_compile_options(ctjxl_jpeg PRIVATE
    "$<$<COMPILE_LANGUAGE:ASM_NASM>:-DELF;-D__x86_64__;-DPIC>"
    "$<$<COMPILE_LANGUAGE:ASM_NASM>:-I${CT_JXL_LIBJPEG_DIR}/simd/nasm/>"
    "$<$<COMPILE_LANGUAGE:ASM_NASM>:-I${CT_JXL_LIBJPEG_DIR}/simd/x86_64/>")

set_target_properties(ctjxl_zlib ctjxl_png ctjxl_gif ctjxl_jpeg PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libjxl's find_package calls find these four (FindZLIB, FindPNG, FindGIF, FindJPEG take set variables as found)
foreach(name ZLIB PNG GIF JPEG)
    string(TOLOWER ${name} lower)
    if(lower STREQUAL "zlib")
        set(target ctjxl_zlib)
    else()
        set(target ctjxl_${lower})
    endif()
    add_library(${name}::${name} ALIAS ${target})
    set(${name}_LIBRARY ${target} CACHE INTERNAL "ctJxl: built in ctJxl/CMakeLists.txt")
    set(${name}_INCLUDE_DIR ${CT_JXL_DEPS_INCLUDE} CACHE INTERNAL "ctJxl: built in ctJxl/CMakeLists.txt")
endforeach()
set(PNG_PNG_INCLUDE_DIR ${CT_JXL_DEPS_INCLUDE} CACHE INTERNAL "ctJxl: built in ctJxl/CMakeLists.txt")

# libjxl as the oracle builds it (jxl/cjxl-oracle/CMakeLists.txt): static, only what cjxl needs,
# no AVX-512 targets. Static libraries through BUILD_SHARED_LIBS only: JPEGXL_STATIC (the oracle's) would also link
# pthread with --whole-archive, which is for fully static programs.
block(SCOPE_FOR VARIABLES)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    foreach(option BENCHMARK DOXYGEN MANPAGES JNI SJPEG OPENEXR FUZZERS DEVTOOLS EXAMPLES PLUGINS VIEWERS TCMALLOC
                   JPEGLI)
        set(JPEGXL_ENABLE_${option} OFF)
    endforeach()
    set(JPEGXL_ENABLE_SKCMS ON)
    set(JPEGXL_BUNDLE_LIBPNG OFF)
    foreach(target AVX3 AVX3_DL AVX3_ZEN4 AVX3_SPR)
        set(JPEGXL_ENABLE_HWY_${target} OFF)
    endforeach()
    add_subdirectory(${CT_JXL_LIBJXL_DIR} libjxl EXCLUDE_FROM_ALL)
endblock()

# the eleven encoder files of ctJxl.Encoder instead of libjxl's own (lib/jxl_lists.cmake)
get_target_property(ct_jxl_enc_sources jxl_enc-obj SOURCES)
set(ct_jxl_replaced "jxl/(enc_frame|enc_heuristics|enc_patch_dictionary|enc_adaptive_quantization|enc_modular")
string(APPEND ct_jxl_replaced "|modular/encoding/enc_encoding|modular/encoding/enc_ma|enc_ans|enc_lz77")
string(APPEND ct_jxl_replaced "|modular/transform/enc_squeeze|encode)\\.cc$")
list(LENGTH ct_jxl_enc_sources ct_jxl_count)
list(FILTER ct_jxl_enc_sources EXCLUDE REGEX "(^|/)${ct_jxl_replaced}")
list(LENGTH ct_jxl_enc_sources ct_jxl_left)
math(EXPR ct_jxl_count "${ct_jxl_count} - ${ct_jxl_left}")
if(NOT ct_jxl_count EQUAL 11)
    message(FATAL_ERROR "ctJxl: ${ct_jxl_count} of the 11 encoder files of libjxl found in jxl_enc-obj")
endif()
set(ct_jxl_encoder
    enc_adaptive_quantization.cc enc_ans.cc enc_encoding.cc enc_frame.cc enc_heuristics.cc enc_lz77.cc enc_ma.cc
    enc_modular.cc enc_patch_dictionary.cc enc_squeeze.cc encode.cc)
list(TRANSFORM ct_jxl_encoder PREPEND ${CT_JXL_ENCODER_DIR}/)
set_property(TARGET jxl_enc-obj PROPERTY SOURCES ${ct_jxl_enc_sources} ${ct_jxl_encoder})

# the input decoders must not be lost silently (libjxl's find_package calls are not REQUIRED)
get_target_property(ct_jxl_codec_libraries jxl_extras_codec LINK_LIBRARIES)
foreach(target ctjxl_png ctjxl_zlib ctjxl_gif ctjxl_jpeg)
    if(NOT target IN_LIST ct_jxl_codec_libraries)
        message(FATAL_ERROR "ctJxl: libjxl did not take ${target} (jxl_extras_codec: ${ct_jxl_codec_libraries})")
    endif()
endforeach()

# cjxl's main and the chunked APNG input: marked copies of libjxl's tools/cjxl_main.cc and lib/extras/dec/apng.cc,
# compiled like libjxl's own tools
add_library(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Cjxl STATIC ${CT_JXL_APP_DIR}/CjxlMain.cc ${CT_JXL_APP_DIR}/ctjxl_apng.cc)
set_target_properties(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Cjxl PROPERTIES CXX_STANDARD 17 CXX_SCAN_FOR_MODULES OFF)
set_source_files_properties(${CT_JXL_APP_DIR}/ctjxl_apng.cc PROPERTIES COMPILE_DEFINITIONS JPEGXL_ENABLE_APNG=1)
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Cjxl PUBLIC jxl_tool jxl_extras_codec jxl jxl_threads hwy
                                                                   ctjxl_png)

set_directory_properties(PROPERTIES COMPILE_OPTIONS "${ct_jxl_compile_options}")

# --- ctJxl's own code ---

add_library(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks STATIC
    ${CT_JXL_TASKS_DIR}/GroupSync.cpp
    ${CT_JXL_TASKS_DIR}/InputBands.cpp
    ${CT_JXL_TASKS_DIR}/ParallelRunner.cpp
    ${CT_JXL_TASKS_DIR}/TaskPool.cpp
)
# libjxl's public headers only (jxl/parallel_runner.h); a link to jxl would close a cycle, see below
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks PUBLIC ${CT_JXL_TASKS_DIR}
                           $<TARGET_PROPERTY:jxl,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared
                                                                    ThinkMeta::ConcurrentTasks)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks)
# the encoder copies in libjxl call into the task graph (GroupSync, RunnerInfo): linked after libjxl
target_link_libraries(jxl PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks)

add_executable(ctJxl
    ${CT_JXL_APP_DIR}/Application.cpp
    ${CT_JXL_APP_DIR}/main.cpp
)
target_link_libraries(ctJxl PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctJxl.Tasks ThinkMeta.ConcurrentTasks.Samples.ctJxl.Cjxl)
ct_sample_fp(ctJxl)
ct_sample_link_core(ctJxl)
set_target_properties(ctJxl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
