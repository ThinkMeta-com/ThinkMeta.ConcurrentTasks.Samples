# jpegli bit oracle with selectable Highway target. Wraps the unmodified jpegli source tree
# (../jpegli, google/jpegli @ 031a007) and its unmodified tools/cjpegli.cc.
cmake_minimum_required(VERSION 3.16...3.27)
project(jpegli_oracle C CXX)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPEGLI_STATIC ON CACHE BOOL "" FORCE)
foreach(opt BENCHMARK DOXYGEN MANPAGES JNI SJPEG OPENEXR FUZZERS DEVTOOLS)
  set(JPEGLI_ENABLE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(JPEGLI_BUNDLE_LIBPNG ON CACHE BOOL "" FORCE)

set(JPEGLI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../jpegli)
add_subdirectory(${JPEGLI_SRC} jpegli)

add_executable(cjpegli-oracle oracle_main.cc ${JPEGLI_SRC}/tools/cjpegli.cc)
set_source_files_properties(${JPEGLI_SRC}/tools/cjpegli.cc PROPERTIES
  COMPILE_DEFINITIONS "main=CjpegliMain")
target_link_libraries(cjpegli-oracle
  jpegli-static jpegli_extras-internal jpegli_threads jpegli_tool jpegli_cms hwy)
