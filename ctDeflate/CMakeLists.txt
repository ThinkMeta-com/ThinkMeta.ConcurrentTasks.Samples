# ctDeflate on Linux (clang, Ninja): the same projects as the .vcxproj files of this folder. Windows builds use those;
# this file never takes part in them. On its own:
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#
# (or included by the CMakeLists.txt of the samples of the API repository, after Samples.Shared).
#
# Targets: ctDeflate (the program, in bin/ of the build folder), ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests
# (unit tests, with libdeflate as the in-process oracle, if CT_SHARED_TEST_SHIM_DIR names the portable CppUnitTest.h).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ctDeflate LANGUAGES C CXX)
    include(${CMAKE_CURRENT_LIST_DIR}/../Shared/Standalone.cmake)
endif()

set(CT_DEFLATE_DIR ${CMAKE_CURRENT_LIST_DIR})

# the encoder: the libdeflate port, whole program for AVX2 as on Windows
add_library(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder STATIC
    ${CT_DEFLATE_DIR}/src/Encoder/BlockWriter.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/Checksums.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/Compressor.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/Huffman.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/BlockTracker.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/ChunkedDeflate.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/Parser.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/Resolver.cpp
    ${CT_DEFLATE_DIR}/src/Encoder/MultiParser.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder PUBLIC ${CT_DEFLATE_DIR}/src/Encoder)
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared)
ct_sample_avx2(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder PUBLIC)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder)

# the task graph
add_library(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks STATIC
    ${CT_DEFLATE_DIR}/src/Tasks/DeflateGraph.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks PUBLIC ${CT_DEFLATE_DIR}/src/Tasks)
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks PUBLIC ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Encoder ThinkMeta::ConcurrentTasks)
ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks)

# the program, with the CPU check of AVX2 programs
add_executable(ctDeflate
    ${CT_DEFLATE_DIR}/src/App/Application.cpp
    ${CT_DEFLATE_DIR}/src/App/CommandLine.cpp
    ${CT_DEFLATE_DIR}/src/App/main.cpp
)
target_link_libraries(ctDeflate PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck)
ct_sample_fp(ctDeflate)
ct_sample_link_core(ctDeflate)
set_target_properties(ctDeflate PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CT_SHARED_TEST_SHIM_DIR)
    # libdeflate 1.26 as the in-process oracle of the tests: the files of libdeflate.vcxproj
    enable_language(C)
    set(CT_LIBDEFLATE_DIR ${CT_DEFLATE_DIR}/libdeflate/libdeflate-1.26)
    add_library(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Libdeflate STATIC
        ${CT_LIBDEFLATE_DIR}/lib/adler32.c
        ${CT_LIBDEFLATE_DIR}/lib/crc32.c
        ${CT_LIBDEFLATE_DIR}/lib/deflate_compress.c
        ${CT_LIBDEFLATE_DIR}/lib/deflate_decompress.c
        ${CT_LIBDEFLATE_DIR}/lib/gzip_compress.c
        ${CT_LIBDEFLATE_DIR}/lib/gzip_decompress.c
        ${CT_LIBDEFLATE_DIR}/lib/utils.c
        ${CT_LIBDEFLATE_DIR}/lib/zlib_compress.c
        ${CT_LIBDEFLATE_DIR}/lib/zlib_decompress.c
        ${CT_LIBDEFLATE_DIR}/lib/x86/cpu_features.c
    )
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Libdeflate PUBLIC ${CT_LIBDEFLATE_DIR})
    set_target_properties(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Libdeflate PROPERTIES LINKER_LANGUAGE C)

    set(CT_DEFLATE_TESTS_DIR ${CT_DEFLATE_DIR}/src/Tests)
    add_executable(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests
        ${CT_DEFLATE_TESTS_DIR}/ChecksumTests.cpp
        ${CT_DEFLATE_TESTS_DIR}/OracleTests.cpp
        ${CT_DEFLATE_TESTS_DIR}/ChunkedTests.cpp
        ${CT_DEFLATE_TESTS_DIR}/CorpusTests.cpp
        ${CT_DEFLATE_TESTS_DIR}/GraphTests.cpp
        ${CT_DEFLATE_TESTS_DIR}/ParserSpeedTests.cpp
        ${CT_SHARED_TEST_SHIM_DIR}/TestMain.cpp
    )
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests PRIVATE ${CT_DEFLATE_TESTS_DIR} ${CT_SHARED_TEST_SHIM_DIR})
    target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests PRIVATE ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tasks
                          ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Libdeflate ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck)
    ct_sample_fp(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests)
    ct_sample_link_core(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests)
    add_test(NAME ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests COMMAND ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests)
    set_tests_properties(ThinkMeta.ConcurrentTasks.Samples.ctDeflate.Tests PROPERTIES TIMEOUT 1800)
endif()
