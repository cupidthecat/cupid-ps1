cmake_minimum_required(VERSION 3.21)
project(cupid_ps1 VERSION 0.1 LANGUAGES C)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC)
  message(FATAL_ERROR "This build requires Windows x86-64 GCC/Clang. Use Makefile on Linux.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)
find_package(SDL2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(Threads REQUIRED)
set(CUPID_SOUNDTOUCH_ROOT "" CACHE PATH "Directory containing the recipe-built SoundTouch DLL and import library")
find_library(SOUNDTOUCH_LIBRARY NAMES SoundTouchDLL
  PATHS "${CUPID_SOUNDTOUCH_ROOT}" NO_DEFAULT_PATH REQUIRED)
find_file(SOUNDTOUCH_RUNTIME NAMES libSoundTouchDLL.dll SoundTouchDLL.dll
  PATHS "${CUPID_SOUNDTOUCH_ROOT}" NO_DEFAULT_PATH REQUIRED)

file(GLOB COMMON_SOURCES CONFIGURE_DEPENDS src/common/*.c src/common/windows/*.c)
foreach(posix file_system memmap threading dynamic_library crash_handler fastjmp)
  list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/common/${posix}.c")
endforeach()
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS src/util/*.c)
list(REMOVE_ITEM UTIL_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/platform/page_fault_handler.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/disc/cd_image_device.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/graphics/opengl_context_egl.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/graphics/opengl_context_egl_xlib.c")
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/core/*.c)
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/core/gpu/gpu_commands.c")
file(GLOB FRONTEND_SOURCES CONFIGURE_DEPENDS src/frontend/*.c src/frontend/windows/*.c)
file(GLOB CHDR_SOURCES CONFIGURE_DEPENDS dep/libchdr/src/*.c)
file(GLOB LZMA_SOURCES CONFIGURE_DEPENDS dep/lzma/src/*.c)
add_library(cupid-disc-codecs STATIC ${CHDR_SOURCES} ${LZMA_SOURCES})
target_include_directories(cupid-disc-codecs PRIVATE dep/libchdr/include dep/lzma/include)
target_compile_definitions(cupid-disc-codecs PRIVATE Z7_ST)
target_compile_options(cupid-disc-codecs PRIVATE -w -fno-strict-aliasing)
target_link_libraries(cupid-disc-codecs PRIVATE ZLIB::ZLIB zstd::libzstd_shared)
add_library(cupid-gl-loader STATIC dep/glad/src/gl.c)
target_include_directories(cupid-gl-loader PUBLIC dep/glad/include)
add_executable(cupid-ps1 ${COMMON_SOURCES} ${UTIL_SOURCES} ${CORE_SOURCES} ${FRONTEND_SOURCES})
target_include_directories(cupid-ps1 PRIVATE src dep/libchdr/include dep/lzma/include dep/soundtouch/include dep/stb)
target_compile_definitions(cupid-ps1 PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 SDL_MAIN_HANDLED __USE_MINGW_ANSI_STDIO=1)
target_compile_options(cupid-ps1 PRIVATE
  -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
  -Wno-unused-parameter -Wno-unused-function -Werror -fno-strict-aliasing)
target_link_options(cupid-ps1 PRIVATE -municode)
target_link_libraries(cupid-ps1 PRIVATE cupid-disc-codecs cupid-gl-loader SDL2::SDL2
  ZLIB::ZLIB zstd::libzstd_shared LibLZMA::LibLZMA Threads::Threads "${SOUNDTOUCH_LIBRARY}")
set_source_files_properties(src/core/gpu/software/gpu_sw_rasterizer_avx2.c
  PROPERTIES COMPILE_OPTIONS "-mavx2;-mavx;-msse4.1;-msse2;-mfma")
set_source_files_properties(src/core/gpu/software/gpu_sw_rasterizer_simd.c
  PROPERTIES COMPILE_OPTIONS "-msse4.1;-msse2")
set_source_files_properties(src/core/gpu/hardware/tc_image_loader.c PROPERTIES COMPILE_OPTIONS "-w")
add_custom_command(TARGET cupid-ps1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SOUNDTOUCH_RUNTIME}" "$<TARGET_FILE_DIR:cupid-ps1>")

file(GLOB_RECURSE PROJECT_HEADERS CONFIGURE_DEPENDS src/common/*.h src/util/*.h src/core/*.h src/frontend/*.h)
set(HEADER_CHECK_SOURCES)
foreach(header IN LISTS PROJECT_HEADERS)
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}/src" "${header}")
  set(check "${CMAKE_CURRENT_BINARY_DIR}/hcheck/${relative}.c")
  file(WRITE "${check}" "#include \"${relative}\"\n")
  list(APPEND HEADER_CHECK_SOURCES "${check}")
endforeach()
add_library(cupid-headers OBJECT ${HEADER_CHECK_SOURCES})
target_include_directories(cupid-headers PRIVATE src dep/glad/include dep/libchdr/include dep/lzma/include dep/soundtouch/include dep/stb)
target_compile_definitions(cupid-headers PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 SDL_MAIN_HANDLED __USE_MINGW_ANSI_STDIO=1)
target_compile_options(cupid-headers PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cupid-headers PRIVATE SDL2::SDL2 LibLZMA::LibLZMA)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hcheck/common/gsvector.h.c"
  PROPERTIES COMPILE_OPTIONS "-msse4.1")
