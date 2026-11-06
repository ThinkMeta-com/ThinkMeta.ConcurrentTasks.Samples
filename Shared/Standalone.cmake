# The setup of an encoder sample built on its own (Linux: clang, Ninja), as in the Samples repository: included by
# the CMakeLists.txt of a sample right after its project() call when it is the top-level project. Inside the API
# repository, API/Samples/CMakeLists.txt does this setup instead.
#
#   cmake -S <sample folder> -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#
# Finds ThinkMeta.ConcurrentTasks in the API submodule of the Samples repository (../ThinkMeta.ConcurrentTasks.API next
# to the sample folder) or, inside the API repository, two levels up; adds Samples.Shared (this folder) and the unit
# tests with the portable CppUnitTest.h of the library's tests.

include_guard(GLOBAL)

# Without a build type: a release build, as the core is one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# -Wall -Wextra always; as errors on request (a newer clang may warn about more than the one the code was checked with)
option(CT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${CT_WARNINGS_AS_ERRORS}>>:-Werror>
)

if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../ThinkMeta.ConcurrentTasks.API/cmake/ThinkMeta.ConcurrentTasksConfig.cmake)
    set(CT_SAMPLE_API_DIR ${CMAKE_CURRENT_LIST_DIR}/../ThinkMeta.ConcurrentTasks.API)
else()
    set(CT_SAMPLE_API_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()
cmake_path(NORMAL_PATH CT_SAMPLE_API_DIR)
find_package(ThinkMeta.ConcurrentTasks CONFIG REQUIRED PATHS ${CT_SAMPLE_API_DIR}/cmake NO_DEFAULT_PATH)

option(CT_BUILD_TESTS "Build the unit tests" ON)
if(CT_BUILD_TESTS)
    enable_testing()
    set(CT_SHARED_TEST_SHIM_DIR ${CT_SAMPLE_API_DIR}/Library/ThinkMeta.ConcurrentTasks.Tests/Portable)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR} shared)
