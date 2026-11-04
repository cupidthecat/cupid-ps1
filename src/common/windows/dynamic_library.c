/* cupid-ps1 Windows library ownership and UTF-8 loading. */
#include "common/dynamic_library.h"
#if defined(_WIN32)
#include "windows_util.h"
#include "common/assert.h"
#include "common/small_string.h"
#include <stdlib.h>

void dynamic_library_init(dynamic_library_t* library) {
  library->handle = NULL;
}
void dynamic_library_destroy(dynamic_library_t* library) {
  dynamic_library_close(library);
}
bool dynamic_library_is_open(const dynamic_library_t* library) {
  return library->handle != NULL;
}

void dynamic_library_get_unprefixed_filename(small_string_t* result, const char* name) {
  small_string_sprintf(result, "%s.dll", name);
}

void dynamic_library_get_versioned_filename(small_string_t* result, const char* name, int major, int minor, int patch) {
  if (major >= 0 && minor >= 0 && patch >= 0)
    small_string_sprintf(result, "%s-%d-%d-%d.dll", name, major, minor, patch);
  else if (major >= 0 && minor >= 0)
    small_string_sprintf(result, "%s-%d-%d.dll", name, major, minor);
  else if (major >= 0)
    small_string_sprintf(result, "%s-%d.dll", name, major);
  else
    small_string_sprintf(result, "%s.dll", name);
}

bool dynamic_library_open(dynamic_library_t* library, const char* filename, Error* error) {
  wchar_t* wide = windows_utf8_to_wide(filename, error);
  if (!wide) return false;
  HMODULE     handle = LoadLibraryW(wide);
  const DWORD code   = GetLastError();
  free(wide);
  if (!handle) {
    windows_set_error(error, "Loading library", code);
    return false;
  }
  dynamic_library_close(library);
  library->handle = handle;
  return true;
}

void dynamic_library_adopt(dynamic_library_t* library, void* handle) {
  AssertMsg(handle, "Cannot adopt a null module handle");
  if (library->handle == handle) return;
  dynamic_library_close(library);
  library->handle = handle;
}

void dynamic_library_close(dynamic_library_t* library) {
  if (library->handle) {
    if (!FreeLibrary(library->handle)) Panic("Closing a module handle failed");
    library->handle = NULL;
  }
}

void* dynamic_library_get_symbol_address(const dynamic_library_t* library, const char* name) {
  if (!library->handle) return NULL;
  FARPROC function = GetProcAddress(library->handle, name);
  void*   result;
  memcpy(&result, &function, sizeof(result));
  return result;
}

void* dynamic_library_get_handle(const dynamic_library_t* library) {
  return library->handle;
}
#endif
