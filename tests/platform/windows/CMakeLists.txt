cmake_minimum_required(VERSION 3.21)
project(cupid_windows_platform_checks LANGUAGES C)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "These checks require a native Windows x86-64 toolchain")
endif()
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(PLATFORM_SOURCES
  "${ROOT}/src/common/windows/memmap.c"
  "${ROOT}/src/common/windows/windows_util.c"
  "${ROOT}/src/common/error.c"
  "${ROOT}/src/common/assert.c"
  "${ROOT}/src/util/platform/windows/page_fault_handler.c"
  "${ROOT}/src/common/windows/threading.c"
  "${ROOT}/src/common/timer.c"
  "${ROOT}/src/common/small_string.c"
  "${ROOT}/src/common/windows/dynamic_library.c"
  "${ROOT}/src/common/windows/file_system.c"
  "${ROOT}/src/common/windows/crash_handler.c"
  "${ROOT}/src/common/file_system_streams.c"
  "${ROOT}/src/common/path.c"
  "${ROOT}/src/common/string_util.c")
foreach(component memory runtime library filesystem path crash)
  set(COMPONENT_SOURCES ${PLATFORM_SOURCES})
  if(component STREQUAL "filesystem")
    list(REMOVE_ITEM COMPONENT_SOURCES "${ROOT}/src/common/windows/file_system.c")
    list(APPEND COMPONENT_SOURCES filesystem_allocations.c)
  endif()
  add_executable(cupid-windows-${component}-tests test_${component}.c ${COMPONENT_SOURCES})
  target_include_directories(cupid-windows-${component}-tests PRIVATE "${ROOT}/src")
  target_compile_definitions(cupid-windows-${component}-tests PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  set_target_properties(cupid-windows-${component}-tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(cupid-windows-${component}-tests PRIVATE
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror)
endforeach()
add_library(cupid-platform-probe SHARED library_probe.c)
set_target_properties(cupid-platform-probe PROPERTIES PREFIX "" C_STANDARD 11)
add_dependencies(cupid-windows-library-tests cupid-platform-probe)
set(HEADERS types memmap timer threading dynamic_library file_system)
set(HEADER_CHECK_SOURCES)
foreach(header IN LISTS HEADERS)
  set(check "${CMAKE_CURRENT_BINARY_DIR}/hcheck/${header}.c")
  file(WRITE "${check}" "#include \"common/${header}.h\"\n")
  list(APPEND HEADER_CHECK_SOURCES "${check}")
endforeach()
add_library(windows-platform-headers OBJECT ${HEADER_CHECK_SOURCES})
target_include_directories(windows-platform-headers PRIVATE "${ROOT}/src")
set_target_properties(windows-platform-headers PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(windows-platform-headers PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME windows_memory COMMAND cupid-windows-memory-tests)
add_test(NAME windows_runtime COMMAND cupid-windows-runtime-tests)
add_test(NAME windows_library COMMAND cupid-windows-library-tests)
add_test(NAME windows_filesystem COMMAND cupid-windows-filesystem-tests)
add_test(NAME windows_path COMMAND cupid-windows-path-tests)
add_test(NAME windows_crash COMMAND cupid-windows-crash-tests)
set_tests_properties(windows_library PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:cupid-platform-probe>")
set_tests_properties(windows_filesystem PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:cupid-windows-filesystem-tests>")
set_tests_properties(windows_memory windows_runtime windows_library windows_filesystem windows_path windows_crash PROPERTIES TIMEOUT 30)
