# Samples.Shared on Linux (clang, Ninja): the common code of the encoder samples. Windows builds use
# the .vcxproj files; this file never takes part in them. Added by Standalone.cmake when a sample is
# built on its own, or by the CMakeLists.txt of the samples of the API repository.
#
# Targets:
#   ThinkMeta.ConcurrentTasks.Samples.Shared           the library (plain x86-64, no AVX2: the CPU
#                                                      check runs on any CPU)
#   ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck  the CPU check of programs built for AVX2, an
#                                                      object file in their .preinit_array (CpuCheck.h)
#   ThinkMeta.ConcurrentTasks.Samples.Shared.Tests     the unit tests, with the portable CppUnitTest.h
#                                                      of the library's tests (if CT_SHARED_TEST_SHIM_DIR
#                                                      names it)

include_guard(GLOBAL)

# The floating-point rule of every encoder sample: no contraction of a*b+c into FMA, as MSVC (/fp:precise) does not
# contract; the outputs must stay bit-identical to their references.
function(ct_sample_fp target)
    target_compile_options(${target} PRIVATE -ffp-contract=off)
endfunction()

# Code built for AVX2, as /arch:AVX2 on Windows: AVX2, FMA, BMI1/2, LZCNT, MOVBE, F16C (x86-64-v3), and PCLMULQDQ.
# Programs with such code link ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck. scope: PRIVATE, or PUBLIC for a
# library whose headers hold such code.
function(ct_sample_avx2 target scope)
    target_compile_options(${target} ${scope} -march=x86-64-v3 -mpclmul)
endfunction()

# The published programs (bin/linux-x64 of the samples), the counterpart of the static C runtime on Windows:
# libstdc++ and libgcc linked in, the core copied next to the program and found there (RUNPATH $ORIGIN). Such a
# program needs nothing but glibc and the core beside it.
option(CT_SAMPLE_PUBLISH "Build the sample programs as published: static libstdc++, the core next to them" OFF)

# A program of a sample: the library of ThinkMeta.ConcurrentTasks, the core as a file (as the unit tests and the
# documentation samples link it), and the core's folder in the run path of the build (published: the program's own).
function(ct_sample_link_core target)
    target_link_libraries(${target} PRIVATE ThinkMeta::ConcurrentTasks $<TARGET_LINKER_FILE:ThinkMeta.ConcurrentTasks.Core> pthread)
    get_target_property(imported ThinkMeta.ConcurrentTasks.Core IMPORTED)
    if(NOT imported)
        add_dependencies(${target} ThinkMeta.ConcurrentTasks.Core)
    endif()
    if(CT_SAMPLE_PUBLISH)
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
        # the install run path in the build tree: only $ORIGIN, none of the link folders CMake adds to BUILD_RPATH
        set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ThinkMeta.ConcurrentTasks.Core> $<TARGET_FILE_DIR:${target}>)
    else()
        set_target_properties(${target} PROPERTIES BUILD_RPATH $<TARGET_FILE_DIR:ThinkMeta.ConcurrentTasks.Core>)
    endif()
endfunction()

set(CT_SHARED_DIR ${CMAKE_CURRENT_LIST_DIR}/src/Shared)

add_library(ThinkMeta.ConcurrentTasks.Samples.Shared STATIC
    ${CT_SHARED_DIR}/Console.cpp
    ${CT_SHARED_DIR}/CpuCheck.cpp
    ${CT_SHARED_DIR}/CpuFeatures.cpp
    ${CT_SHARED_DIR}/File.cpp
    ${CT_SHARED_DIR}/Formatter.cpp
)
target_include_directories(ThinkMeta.ConcurrentTasks.Samples.Shared PUBLIC ${CT_SHARED_DIR})
target_compile_features(ThinkMeta.ConcurrentTasks.Samples.Shared PUBLIC cxx_std_23)
# as on Windows: no exception tables
target_compile_options(ThinkMeta.ConcurrentTasks.Samples.Shared PRIVATE -fno-exceptions)

add_library(ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck OBJECT ${CT_SHARED_DIR}/CpuCheckConsole.cpp)
target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck PUBLIC ThinkMeta.ConcurrentTasks.Samples.Shared)

if(CT_SHARED_TEST_SHIM_DIR)
    set(CT_SHARED_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR}/src/Tests)
    add_executable(ThinkMeta.ConcurrentTasks.Samples.Shared.Tests
        ${CT_SHARED_TESTS_DIR}/FormatterTests.cpp
        ${CT_SHARED_TESTS_DIR}/TextTests.cpp
        ${CT_SHARED_TEST_SHIM_DIR}/TestMain.cpp
    )
    target_include_directories(ThinkMeta.ConcurrentTasks.Samples.Shared.Tests PRIVATE ${CT_SHARED_TESTS_DIR} ${CT_SHARED_TEST_SHIM_DIR})
    # with the CPU check, so that every test run also runs it
    target_link_libraries(ThinkMeta.ConcurrentTasks.Samples.Shared.Tests PRIVATE ThinkMeta.ConcurrentTasks.Samples.Shared.CpuCheck)
    add_test(NAME ThinkMeta.ConcurrentTasks.Samples.Shared.Tests COMMAND ThinkMeta.ConcurrentTasks.Samples.Shared.Tests)
endif()
