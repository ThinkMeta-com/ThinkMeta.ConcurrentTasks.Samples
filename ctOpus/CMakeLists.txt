# ctOpus on Linux (clang, Ninja): the same projects as the .vcxproj files of this folder, without ctOpusGui and
# ctOpusLiveBench (Windows only). Windows builds use the .vcxproj files; this file never takes part in them. On its own:
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#
# (or included by the CMakeLists.txt of the samples of the API repository, after Samples.Shared). The reference program
# opusenc has its own CMakeLists.txt in opus/opus-tools-0.2.
#
# Targets: ctOpus (the program, in bin/ of the build folder), ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests (unit
# tests, if CT_SHARED_TEST_SHIM_DIR names the portable CppUnitTest.h).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ctOpus LANGUAGES CXX)
    include(${CMAKE_CURRENT_LIST_DIR}/../Shared/Standalone.cmake)
endif()

set(CT_OPUS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CT_OPUS_ENCODER_DIR ${CT_OPUS_DIR}/src/Encoder)
set(CT_OPUS_TASKS_DIR ${CT_OPUS_DIR}/src/Tasks)

# the encoder: the port of opusenc, libopusenc and libopus, built for AVX2 as on Windows (its headers too: the task
# graph inlines them)
set(CT_OPUS_ENCODER_SOURCES
    Resampler WavInput OggPacker OpusHeaders OggOpusEncoder ParallelResampler PlanarStore OpusStreamEncoder
    OpusMultistreamEncoder KissFft CeltEncoder CeltSpeculation RangeEncoder CeltMode CeltMdct CeltBands CeltEnergy
    CeltRate CeltPitch CeltVq OpusPacket SilkSigProc SilkResampler SilkStereo SilkEncoder SilkVad SilkCoding
    SilkEncodeFrame SilkNsq SilkNlsf SilkPredCoefs SilkPitchAnalysis SilkSigProcFlp SilkNoiseShape ToneAnalysis
)
list(TRANSFORM CT_OPUS_ENCODER_SOURCES PREPEND ${CT_OPUS_ENCODER_DIR}/)
list(TRANSFORM CT_OPUS_ENCODER_SOURCES APPEND .cpp)
add_library(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder STATIC ${CT_OPUS_ENCODER_SOURCES})
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder PUBLIC ${CT_OPUS_ENCODER_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared)
ct_sample_avx2(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder PUBLIC)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder)

# the task graph
add_library(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks STATIC
    ${CT_OPUS_TASKS_DIR}/EncodeGraph.cpp
    ${CT_OPUS_TASKS_DIR}/EncodeTasks.cpp
    ${CT_OPUS_TASKS_DIR}/FileIo.cpp
    ${CT_OPUS_TASKS_DIR}/ToneChain.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks PUBLIC ${CT_OPUS_TASKS_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks PUBLIC ThinkMeta.ConcurrentTasks.Samples.ctOpus.Encoder ThinkMeta::ConcurrentTasks)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks)

# the program, with the CPU check of AVX2 programs
add_executable(ctOpus
    ${CT_OPUS_DIR}/src/App/Application.cpp
    ${CT_OPUS_DIR}/src/App/CommandLine.cpp
    ${CT_OPUS_DIR}/src/App/main.cpp
)
target_link_libraries(ctOpus PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck)
ct_sample_fp(ctOpus)
ct_sample_link_core(ctOpus)
set_target_properties(ctOpus PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CT_SHARED_TEST_SHIM_DIR)
    set(CT_OPUS_TESTS_DIR ${CT_OPUS_DIR}/src/Tests)
    add_executable(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests
        ${CT_OPUS_TESTS_DIR}/EdgeTests.cpp
        ${CT_OPUS_TESTS_DIR}/GraphTests.cpp
        ${CT_SHARED_TEST_SHIM_DIR}/TestMain.cpp
    )
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests PRIVATE ${CT_OPUS_TESTS_DIR} ${CT_SHARED_TEST_SHIM_DIR})
    target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tasks
                          ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck)
    ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests)
    ct_sample_link_core(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests)
    add_test(NAME ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests COMMAND ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests)
    set_tests_properties(ThinkMeta.ConcurrentTasks.Samples.ctOpus.Tests PROPERTIES TIMEOUT 1800)
endif()
