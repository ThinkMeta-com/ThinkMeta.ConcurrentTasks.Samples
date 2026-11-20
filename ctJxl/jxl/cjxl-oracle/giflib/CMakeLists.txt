# giflib 5.2.2 (jxl/giflib-5.2.2, unchanged) as a static library for the reference cjxl.
# Same file set as the vcpkg port the official libjxl Windows release uses; instead of vcpkg's msvc.diff
# (only file-open helpers and strtok_r, not used by libjxl) the names are mapped by definitions.
cmake_minimum_required(VERSION 3.16)
project(giflib C)
set(GIF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../giflib-5.2.2)
add_library(gif STATIC
  ${GIF_SRC}/dgif_lib.c ${GIF_SRC}/egif_lib.c ${GIF_SRC}/gifalloc.c ${GIF_SRC}/gif_err.c
  ${GIF_SRC}/gif_font.c ${GIF_SRC}/gif_hash.c ${GIF_SRC}/openbsd-reallocarray.c)
if(MSVC) # the names of the C runtime of Visual Studio
  target_compile_definitions(gif PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS strtok_r=strtok_s)
endif()
set_property(TARGET gif PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
install(FILES ${GIF_SRC}/gif_lib.h DESTINATION include)
install(TARGETS gif ARCHIVE DESTINATION lib)
