cmake_minimum_required(VERSION 3.21)
project(cupid_audio_dependency_check LANGUAGES C)

if(NOT IS_ABSOLUTE "${SOUNDTOUCH_BUILD}")
  message(FATAL_ERROR "Set SOUNDTOUCH_BUILD to the absolute dependency build directory")
endif()
unset(SOUNDTOUCH_WRAPPER CACHE)
find_library(SOUNDTOUCH_WRAPPER NAMES SoundTouchDLL
  PATHS "${SOUNDTOUCH_BUILD}" "${SOUNDTOUCH_BUILD}/Release" NO_DEFAULT_PATH NO_CACHE REQUIRED)

add_executable(soundtouch_probe soundtouch_probe.c)
set_target_properties(soundtouch_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(soundtouch_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/soundtouch/include")
target_link_libraries(soundtouch_probe PRIVATE "${SOUNDTOUCH_WRAPPER}")
if(MSVC)
  target_compile_options(soundtouch_probe PRIVATE /W4 /WX)
else()
  target_compile_options(soundtouch_probe PRIVATE -Wall -Wextra -Werror)
endif()
if(WIN32)
  unset(SOUNDTOUCH_RUNTIME CACHE)
  find_file(SOUNDTOUCH_RUNTIME NAMES libSoundTouchDLL.dll SoundTouchDLL.dll
    PATHS "${SOUNDTOUCH_BUILD}" "${SOUNDTOUCH_BUILD}/Release" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  add_custom_command(TARGET soundtouch_probe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOUNDTOUCH_RUNTIME}" "$<TARGET_FILE_DIR:soundtouch_probe>")
endif()
enable_testing()
add_test(NAME soundtouch_c11_interface COMMAND soundtouch_probe)
