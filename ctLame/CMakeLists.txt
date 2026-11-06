# ctLame on Linux (clang, Ninja): the same projects as the .vcxproj files of this folder, without ctLame.Nmr (an
# analysis tool, Windows only). Windows builds use the .vcxproj files; this file never takes part in them. On its own:
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#
# (or included by the CMakeLists.txt of the samples of the API repository, after Samples.Shared). The reference program
# lame has its own CMakeLists.txt in lame.
#
# Targets: ctLame (the program, in bin/ of the build folder), ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests (unit
# tests, if CT_SHARED_TEST_SHIM_DIR names the portable CppUnitTest.h). ctLame runs on any x64 CPU: it chooses the
# scalar, SSE4.1 or AVX2 code at run time (--simd), so only the *Sse41.cpp and *Avx2.cpp files are built for their
# instruction sets and it needs no CPU check.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ctLame LANGUAGES CXX)
    include(${CMAKE_CURRENT_LIST_DIR}/../Shared/Standalone.cmake)
endif()

set(CT_LAME_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CT_LAME_ENCODER_DIR ${CT_LAME_DIR}/src/Encoder)
set(CT_LAME_TASKS_DIR ${CT_LAME_DIR}/src/Tasks)

set(CT_LAME_ENCODER_SOURCES
    BitCount Bitstream ConfigPrint Encoder EncoderOptions Fft FhtAvx2 FhtSse41 HuffmanTables Input Mdct MdctAvx2
    MdctKernels MdctSse41 Presets Profiling PsyAvx2 PsyInit PsyMath PsyPrepare PsyResolve PsySse41 Quantize QuantizeAvx2
    QuantizeSse41 Reservoir SampleStream SessionConfig SimdFeatures Stages Tables VbrQuantize VbrTag Version
)
list(TRANSFORM CT_LAME_ENCODER_SOURCES PREPEND ${CT_LAME_ENCODER_DIR}/)
list(TRANSFORM CT_LAME_ENCODER_SOURCES APPEND .cpp)
add_library(ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder STATIC ${CT_LAME_ENCODER_SOURCES})
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder PUBLIC ${CT_LAME_ENCODER_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder)
# the feature sets, file by file as in the .vcxproj: /arch:AVX2 on the *Avx2.cpp files; MSVC compiles the SSE4.1
# intrinsics of the *Sse41.cpp files without a switch, clang needs -msse4.1 for them
foreach(file IN LISTS CT_LAME_ENCODER_SOURCES)
    if(file MATCHES "Avx2\\.cpp$")
        set_source_files_properties(${file} PROPERTIES COMPILE_OPTIONS "-march=x86-64-v3")
    elseif(file MATCHES "Sse41\\.cpp$")
        set_source_files_properties(${file} PROPERTIES COMPILE_OPTIONS "-msse4.1")
    endif()
endforeach()

set(CT_LAME_TASKS_SOURCES
    BlockPipe DispatchTask EncodeGraph EncodeTask FrameEncoder FrameRing Mp3WriterTask PcmFeed SequentialEncodeTask WavReaderTask
)
list(TRANSFORM CT_LAME_TASKS_SOURCES PREPEND ${CT_LAME_TASKS_DIR}/)
list(TRANSFORM CT_LAME_TASKS_SOURCES APPEND .cpp)
add_library(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks STATIC ${CT_LAME_TASKS_SOURCES})
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks PUBLIC ${CT_LAME_TASKS_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks PUBLIC ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder ThinkMeta::ConcurrentTasks)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks)

add_executable(ctLame
    ${CT_LAME_DIR}/src/App/Application.cpp
    ${CT_LAME_DIR}/src/App/CommandLine.cpp
    ${CT_LAME_DIR}/src/App/WavFile.cpp
    ${CT_LAME_DIR}/src/App/main.cpp
)
target_link_libraries(ctLame PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks)
ct_sample_fp(ctLame)
ct_sample_link_core(ctLame)
set_target_properties(ctLame PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CT_SHARED_TEST_SHIM_DIR)
    set(CT_LAME_TESTS_DIR ${CT_LAME_DIR}/src/Tests)
    # without the two measurements EventCostBenchmark and IoPipelineBenchmark (against CopyFileW): they run on Windows
    set(CT_LAME_TESTS_SOURCES
        EncoderGoldenTests FrameRingTests InitParamsTests InputMdctTests IoPipelineTests MdctGoldenTests PsyGoldenTests
        SchedulerHandleTests SimdTests VersionTests
    )
    list(TRANSFORM CT_LAME_TESTS_SOURCES PREPEND ${CT_LAME_TESTS_DIR}/)
    list(TRANSFORM CT_LAME_TESTS_SOURCES APPEND .cpp)
    add_executable(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests ${CT_LAME_TESTS_SOURCES} ${CT_SHARED_TEST_SHIM_DIR}/TestMain.cpp)
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests PRIVATE ${CT_LAME_TESTS_DIR} ${CT_SHARED_TEST_SHIM_DIR})
    target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctLame.Tasks)
    ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests)
    ct_sample_link_core(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests)
    add_test(NAME ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests COMMAND ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests)
    set_tests_properties(ThinkMeta.ConcurrentTasks.Samples.ctLame.Tests PROPERTIES TIMEOUT 1800)
endif()
