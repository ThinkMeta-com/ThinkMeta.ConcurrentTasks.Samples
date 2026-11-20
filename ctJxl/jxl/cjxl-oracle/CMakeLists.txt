# cjxl bit oracle with selectable Highway target. Wraps the unmodified libjxl 0.13.0 source tree
# in ../libjxl-0.13.0 and its unmodified tools/cjxl_main.cc.
# Also builds the plain cjxl/djxl tools (dynamic dispatch) from the same tree as the speed reference.
cmake_minimum_required(VERSION 3.16...3.27)
project(cjxl_oracle C CXX)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPEGXL_STATIC ON CACHE BOOL "" FORCE)
foreach(opt BENCHMARK DOXYGEN MANPAGES JNI SJPEG OPENEXR FUZZERS DEVTOOLS EXAMPLES PLUGINS VIEWERS
            TCMALLOC JPEGLI)
  set(JPEGXL_ENABLE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG ON CACHE BOOL "" FORCE)
# No AVX-512 (project rule): AVX2 is the widest target.
foreach(tgt AVX3 AVX3_DL AVX3_ZEN4 AVX3_SPR)
  set(JPEGXL_ENABLE_HWY_${tgt} OFF CACHE BOOL "" FORCE)
endforeach()

# -DJXL_SRC=... builds another tree
if(NOT JXL_SRC)
  set(JXL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../libjxl-0.13.0)
endif()
# libjxl's extras call find_package without REQUIRED; the oracle must not lose a decoder silently.
find_package(GIF 5.1 REQUIRED)
find_package(JPEG REQUIRED)

add_subdirectory(${JXL_SRC} libjxl)

# as libjxl's own tools: C++17 and the UTF-8 manifest (non-ASCII paths)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
add_executable(cjxl-oracle oracle_main.cc ${JXL_SRC}/tools/cjxl_main.cc ${JXL_SRC}/tools/utf8.manifest)
set_source_files_properties(${JXL_SRC}/tools/cjxl_main.cc PROPERTIES
  COMPILE_DEFINITIONS "main=CjxlMain")
target_link_libraries(cjxl-oracle jxl jxl_extras_codec jxl_threads jxl_tool hwy)
