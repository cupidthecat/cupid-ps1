/* cupid-ps1 UTF-8 module names, symbol calls and ownership checks. */
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/small_string.h"
#include "common/windows/windows_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%u: %s (Windows error %lu)\n", __FILE__, (unsigned)__LINE__, #condition, \
              (unsigned long)GetLastError());                                                      \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)

int main(void)
{
  wchar_t             wide_name[128];
  char                utf8_name[128];
  const unsigned long pid = (unsigned long)GetCurrentProcessId();
  CHECK(swprintf(wide_name, 128, L"cupid-platform-%lu-\x03BB.dll", pid) > 0);
  CHECK(snprintf(utf8_name, 128, "cupid-platform-%lu-\xCE\xBB.dll", pid) > 0);
  CHECK(CopyFileW(L"cupid-platform-probe.dll", wide_name, TRUE));
  Error             error = ERROR_INIT;
  dynamic_library_t library;
  dynamic_library_init(&library);
  CHECK(!dynamic_library_is_open(&library));
  CHECK(!dynamic_library_get_symbol_address(&library, "missing"));
  CHECK(dynamic_library_open(&library, utf8_name, &error));
  CHECK(dynamic_library_is_open(&library));
  void* address = dynamic_library_get_symbol_address(&library, "cupid_platform_answer");
  int (*answer)(void);
  memcpy(&answer, &address, sizeof(answer));
  CHECK(answer && answer() == 73);
  CHECK(!dynamic_library_get_symbol_address(&library, "NoSuchExport"));
  CHECK(!dynamic_library_open(&library, "no-such-cupid-library.dll", &error));
  CHECK(Error_is_valid(&error) && answer() == 73);
  CHECK(!dynamic_library_open(&library, "\xC0", &error));
  CHECK(answer() == 73);
  dynamic_library_close(&library);
  CHECK(!dynamic_library_is_open(&library));
  CHECK(DeleteFileW(wide_name));
  HMODULE adopted = LoadLibraryW(L"kernel32.dll");
  CHECK(adopted);
  dynamic_library_adopt(&library, adopted);
  CHECK(dynamic_library_get_handle(&library) == adopted);
  CHECK(dynamic_library_get_symbol_address(&library, "GetCurrentProcessId"));
  dynamic_library_destroy(&library);
  small_string_t name;
  small_string_init(&name);
  dynamic_library_get_unprefixed_filename(&name, "EGL");
  CHECK(!strcmp(small_string_c_str(&name), "EGL.dll"));
  dynamic_library_get_versioned_filename(&name, "EGL", 1, 2, 3);
  CHECK(!strcmp(small_string_c_str(&name), "EGL-1-2-3.dll"));
  dynamic_library_get_versioned_filename(&name, "EGL", 1, 2, -1);
  CHECK(!strcmp(small_string_c_str(&name), "EGL-1-2.dll"));
  dynamic_library_get_versioned_filename(&name, "EGL", 1, -1, -1);
  CHECK(!strcmp(small_string_c_str(&name), "EGL-1.dll"));
  dynamic_library_get_versioned_filename(&name, "EGL", -1, -1, -1);
  CHECK(!strcmp(small_string_c_str(&name), "EGL.dll"));
  small_string_destroy(&name);
  Error_destroy(&error);
  puts("Windows loader checks passed: UTF-8 paths, C symbols, failed loads and ownership");
  return 0;
}
