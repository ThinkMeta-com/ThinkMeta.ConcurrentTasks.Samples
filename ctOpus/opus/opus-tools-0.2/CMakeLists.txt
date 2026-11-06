# opusenc (opus-tools 0.2 with libopusenc 0.2.1, libopus 1.6.1 and libogg 1.3.6) on Linux x64 with
# clang: the reference program ctOpus is checked against on Linux. It mirrors the Windows build:
# the source files are read from opusenc.vcxproj and the .vcxproj files it references, with the
# same defines and config headers (linux/config.h in place of msvc/config.h). The upstream
# build systems of these projects choose other options and are not used. Windows builds use the
# .vcxproj files; this file never takes part in them.
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build build/linux-release
#
# No contraction of a*b+c into FMA (-ffp-contract=off), as in the MSVC build (/fp:precise). The
# x86 files of libopus get the instruction sets that MSVC allows without a switch (as the
# upstream CMakeLists.txt does); the run-time CPU detection (OPUS_HAVE_RTCD) picks them.

cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(opusenc C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_compile_options(-ffp-contract=off)

get_filename_component(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(OGG_DIR ${OPUS_ROOT}/libogg-1.3.6)
set(LIBOPUS_DIR ${OPUS_ROOT}/opus-1.6.1)
set(LIBOPUSENC_DIR ${OPUS_ROOT}/libopusenc-0.2.1)
set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The <ClCompile Include="..."> files of a .vcxproj, as absolute paths.
function(ct_vcxproj_sources result vcxproj)
    get_filename_component(dir "${vcxproj}" DIRECTORY)
    file(STRINGS "${vcxproj}" lines REGEX "<ClCompile Include=\"[^\"]+\"")
    set(files)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" file "${line}")
        string(REPLACE "\\" "/" file "${file}")
        list(APPEND files "${dir}/${file}")
    endforeach()
    set(${result} ${files} PARENT_SCOPE)
endfunction()

# libogg: os_types.h needs config_types.h on Linux (generated as its own CMake build does).
ct_vcxproj_sources(OGG_SOURCES ${OGG_DIR}/libogg.vcxproj)
set(INCLUDE_INTTYPES_H 1)
set(INCLUDE_STDINT_H 1)
set(INCLUDE_SYS_TYPES_H 1)
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
set(SIZE64 int64_t)
set(USIZE64 uint64_t)
configure_file(${OGG_DIR}/include/ogg/config_types.h.in ${CMAKE_BINARY_DIR}/ogg-include/ogg/config_types.h @ONLY)
add_library(ogg STATIC ${OGG_SOURCES})
target_include_directories(ogg PUBLIC ${OGG_DIR}/include ${CMAKE_BINARY_DIR}/ogg-include)

# libopus: config.h in its root, already neutral apart from an MSVC block.
ct_vcxproj_sources(LIBOPUS_SOURCES ${LIBOPUS_DIR}/libopus.vcxproj)
add_library(opus STATIC ${LIBOPUS_SOURCES})
target_include_directories(opus
    PUBLIC ${LIBOPUS_DIR}/include
    PRIVATE ${LIBOPUS_DIR} ${LIBOPUS_DIR}/celt ${LIBOPUS_DIR}/silk ${LIBOPUS_DIR}/silk/float ${LIBOPUS_DIR}/dnn ${LIBOPUS_DIR}/src)
target_compile_definitions(opus PRIVATE OPUS_BUILD HAVE_CONFIG_H USE_ALLOCA HAVE_ALLOCA_H ENABLE_HARDENING DISABLE_DEBUG_FLOAT)
foreach(file IN LISTS LIBOPUS_SOURCES)
    if(file MATCHES "_sse4_1\\.c$")
        set_source_files_properties(${file} PROPERTIES COMPILE_OPTIONS "-msse4.1")
    elseif(file MATCHES "(_avx2|_avx)\\.c$")
        set_source_files_properties(${file} PROPERTIES COMPILE_OPTIONS "-mavx;-mfma;-mavx2")
    endif()
endforeach()
target_link_libraries(opus PUBLIC m)

# libopusenc: win32/config.h holds no Windows parts; msvc/ has opus_compat.h and version.h.
# src/unicode_support.c has a branch for other systems (opeint_fopen over fopen).
ct_vcxproj_sources(LIBOPUSENC_SOURCES ${LIBOPUSENC_DIR}/libopusenc.vcxproj)
add_library(opusenc-lib STATIC ${LIBOPUSENC_SOURCES})
target_include_directories(opusenc-lib
    PUBLIC ${LIBOPUSENC_DIR}/include
    PRIVATE ${LIBOPUSENC_DIR}/msvc ${LIBOPUSENC_DIR}/win32)
target_compile_definitions(opusenc-lib PRIVATE RANDOM_PREFIX=libopusenc OUTSIDE_SPEEX HAVE_CONFIG_H)
target_compile_options(opusenc-lib PRIVATE -include opus_compat.h)
target_link_libraries(opusenc-lib PUBLIC opus)

# opusenc: linux/config.h in place of msvc/config.h, msvc/ still for version.h and opus_compat.h;
# win32/unicode_support.c is Windows only.
ct_vcxproj_sources(TOOLS_SOURCES ${TOOLS_DIR}/opusenc.vcxproj)
list(FILTER TOOLS_SOURCES EXCLUDE REGEX "/win32/")
add_executable(opusenc ${TOOLS_SOURCES})
target_include_directories(opusenc PRIVATE ${TOOLS_DIR}/linux ${TOOLS_DIR}/msvc ${TOOLS_DIR}/include ${TOOLS_DIR}/src)
target_compile_definitions(opusenc PRIVATE HAVE_CONFIG_H)
target_compile_options(opusenc PRIVATE -include opus_compat.h)
target_link_libraries(opusenc PRIVATE opusenc-lib opus ogg m)
