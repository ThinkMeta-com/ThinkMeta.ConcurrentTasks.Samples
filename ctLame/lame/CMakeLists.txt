# The reduced LAME 4.0 (lame) on Linux x64 with clang: the same sources and switches as
# frontend/lame.vcxproj and libmp3lame/libmp3lame.vcxproj, with configLinux.h in place of
# configMS.h. It is the reference program ctLame is checked against on Linux. Windows builds use
# the .vcxproj files; this file never takes part in them.
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build build/linux-release
#
# No contraction of a*b+c into FMA (-ffp-contract=off), as in the MSVC build (/fp:precise).

cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(lame C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(mp3lame STATIC
    libmp3lame/bitstream.c
    libmp3lame/encoder.c
    libmp3lame/fft.c
    libmp3lame/lame.c
    libmp3lame/newmdct.c
    libmp3lame/presets.c
    libmp3lame/psymodel.c
    libmp3lame/quantize.c
    libmp3lame/quantize_pvt.c
    libmp3lame/reservoir.c
    libmp3lame/set_get.c
    libmp3lame/tables.c
    libmp3lame/takehiro.c
    libmp3lame/util.c
    libmp3lame/vbrquantize.c
    libmp3lame/VbrTag.c
    libmp3lame/version.c
    libmp3lame/vector/xmm_quantize_sub.c
)
target_include_directories(mp3lame PRIVATE libmp3lame include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)
target_compile_options(mp3lame PRIVATE -ffp-contract=off)

add_executable(lame
    frontend/console.c
    frontend/get_audio.c
    frontend/lame_main.c
    frontend/main.c
    frontend/parse.c
    frontend/timestatus.c
)
target_include_directories(lame PRIVATE frontend libmp3lame include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lame PRIVATE HAVE_CONFIG_H)
target_compile_options(lame PRIVATE -ffp-contract=off)
target_link_libraries(lame PRIVATE mp3lame m)
