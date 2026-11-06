# ctJpeg on Linux (clang, Ninja): the same projects as the .vcxproj files of this folder. Windows builds use those;
# this file never takes part in them. On its own:
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#
# (or included by the CMakeLists.txt of the samples of the API repository, after Samples.Shared).
#
# Targets: ctJpeg (the program, in bin/ of the build folder), ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests (unit
# tests, if CT_SHARED_TEST_SHIM_DIR names the portable CppUnitTest.h), GenTestImages (the synthetic test images of
# verify/Prepare-Inputs.ps1, where its source is there). ctJpeg runs on any x64 CPU: the encoder is plain x86-64
# except the AVX2 build of the pixel phase (PixelPhaseAvx2.cpp), which the program chooses at run time (--simd), so
# it needs no CPU check.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ctJpeg LANGUAGES CXX)
    include(${CMAKE_CURRENT_LIST_DIR}/../Shared/Standalone.cmake)
endif()

set(CT_JPEG_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CT_JPEG_ENCODER_DIR ${CT_JPEG_DIR}/src/Encoder)
set(CT_JPEG_TASKS_DIR ${CT_JPEG_DIR}/src/Tasks)

add_library(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Encoder STATIC
    ${CT_JPEG_ENCODER_DIR}/BandStitch.cpp
    ${CT_JPEG_ENCODER_DIR}/BitWriter.cpp
    ${CT_JPEG_ENCODER_DIR}/ByteBuffer.cpp
    ${CT_JPEG_ENCODER_DIR}/ChunkSplice.cpp
    ${CT_JPEG_ENCODER_DIR}/Coefficients.cpp
    ${CT_JPEG_ENCODER_DIR}/Encoder.cpp
    ${CT_JPEG_ENCODER_DIR}/EncoderSetup.cpp
    ${CT_JPEG_ENCODER_DIR}/HuffmanCodes.cpp
    ${CT_JPEG_ENCODER_DIR}/Image.cpp
    ${CT_JPEG_ENCODER_DIR}/JpegliTables.cpp
    ${CT_JPEG_ENCODER_DIR}/Markers.cpp
    ${CT_JPEG_ENCODER_DIR}/PixelPhase.cpp
    ${CT_JPEG_ENCODER_DIR}/PixelPhaseAvx2.cpp
    ${CT_JPEG_ENCODER_DIR}/PixelPhaseSse2.cpp
    ${CT_JPEG_ENCODER_DIR}/ScanWriter.cpp
    ${CT_JPEG_ENCODER_DIR}/SimdFeatures.cpp
    ${CT_JPEG_ENCODER_DIR}/Tokenize.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Encoder PUBLIC ${CT_JPEG_ENCODER_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Encoder PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Encoder)
# the AVX2 feature set: this translation unit only, as /arch:AVX2 on the one file in the .vcxproj
set_source_files_properties(${CT_JPEG_ENCODER_DIR}/PixelPhaseAvx2.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v3")

add_library(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks STATIC
    ${CT_JPEG_TASKS_DIR}/AsyncInput.cpp
    ${CT_JPEG_TASKS_DIR}/AsyncOutput.cpp
    ${CT_JPEG_TASKS_DIR}/EncodeGraph.cpp
    ${CT_JPEG_TASKS_DIR}/ScanChunks.cpp
    ${CT_JPEG_TASKS_DIR}/StageWorkers.cpp
    ${CT_JPEG_TASKS_DIR}/TokenBands.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks PUBLIC ${CT_JPEG_TASKS_DIR})
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks PUBLIC ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Encoder ThinkMeta::ConcurrentTasks)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks)

add_executable(ctJpeg
    ${CT_JPEG_DIR}/src/App/Application.cpp
    ${CT_JPEG_DIR}/src/App/CommandLine.cpp
    ${CT_JPEG_DIR}/src/App/main.cpp
)
target_link_libraries(ctJpeg PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks)
ct_sample_fp(ctJpeg)
ct_sample_link_core(ctJpeg)
set_target_properties(ctJpeg PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# the generator of the synthetic test images (verify/Prepare-Inputs.ps1), a plain C++ program; in the Samples
# repository only
if(EXISTS ${CT_JPEG_DIR}/src/GenTestImages/GenTestImages.cpp)
    add_executable(GenTestImages ${CT_JPEG_DIR}/src/GenTestImages/GenTestImages.cpp)
    ct_sample_fp(GenTestImages)
    set_target_properties(GenTestImages PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(CT_SHARED_TEST_SHIM_DIR)
    set(CT_JPEG_TESTS_DIR ${CT_JPEG_DIR}/src/Tests)
    add_executable(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests
        ${CT_JPEG_TESTS_DIR}/BitWriterTests.cpp
        ${CT_JPEG_TESTS_DIR}/GraphTests.cpp
        ${CT_JPEG_TESTS_DIR}/ImageTests.cpp
        ${CT_JPEG_TESTS_DIR}/KernelTests.cpp
        ${CT_JPEG_TESTS_DIR}/MarkerTests.cpp
        ${CT_JPEG_TESTS_DIR}/SimdFeaturesTests.cpp
        ${CT_JPEG_TESTS_DIR}/StageJoinBenchmark.cpp
        ${CT_SHARED_TEST_SHIM_DIR}/TestMain.cpp
    )
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests PRIVATE ${CT_JPEG_TESTS_DIR} ${CT_SHARED_TEST_SHIM_DIR})
    target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tasks)
    ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests)
    ct_sample_link_core(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests)
    add_test(NAME ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests COMMAND ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests)
    set_tests_properties(ThinkMeta.ConcurrentTasks.Samples.ctJpeg.Tests PROPERTIES TIMEOUT 1800)
endif()
