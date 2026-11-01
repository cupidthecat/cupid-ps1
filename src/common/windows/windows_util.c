/* cupid-ps1 Windows API boundary conversion and diagnostics. */
#include "windows_util.h"
#if defined(_WIN32)
#include "common/error.h"
#include <stdlib.h>
#include <wchar.h>

void windows_set_error(Error* error, const char* operation, DWORD code) {
  Error_set_string_fmt(error, "%s: Windows error %lu", operation, (unsigned long)code);
}

wchar_t* windows_utf8_to_wide(const char* text, Error* error) {
  if (!text) {
    Error_set_string(error, "Missing UTF-8 text");
    return NULL;
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (length == 0) {
    windows_set_error(error, "Decoding UTF-8", GetLastError());
    return NULL;
  }
  wchar_t* result = malloc((size_t)length * sizeof(*result));
  if (!result) {
    Error_set_string(error, "Allocating UTF-16 text failed");
    return NULL;
  }
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, length)) {
    windows_set_error(error, "Decoding UTF-8", GetLastError());
    free(result);
    return NULL;
  }
  return result;
}
char* windows_wide_to_utf8(const wchar_t* text, Error* error) {
  if (!text) {
    Error_set_string(error, "Missing UTF-16 text");
    return NULL;
  }
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
  if (!length) {
    windows_set_error(error, "Encoding UTF-16", GetLastError());
    return NULL;
  }
  char* result = malloc((size_t)length);
  if (!result) {
    Error_set_string(error, "Allocating UTF-8 text failed");
    return NULL;
  }
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, length, NULL, NULL)) {
    windows_set_error(error, "Encoding UTF-16", GetLastError());
    free(result);
    return NULL;
  }
  return result;
}

wchar_t* windows_path_to_wide(const char* path, Error* error) {
  if (!path || !*path) {
    Error_set_string(error, "Missing file path");
    return NULL;
  }
  wchar_t* wide = windows_utf8_to_wide(path, error);
  if (!wide) return NULL;
  for (wchar_t* p = wide; *p; p++)
    if (*p == L'/') *p = L'\\';
  if (wcsncmp(wide, L"\\\\?\\", 4) == 0 || wcsncmp(wide, L"\\\\.\\", 4) == 0) return wide;
  DWORD    capacity = GetFullPathNameW(wide, 0, NULL, NULL);
  wchar_t* absolute = NULL;
  while (capacity) {
    wchar_t* buffer = realloc(absolute, (size_t)capacity * sizeof(*buffer));
    if (!buffer) {
      free(absolute);
      free(wide);
      Error_set_string(error, "Allocating an absolute path failed");
      return NULL;
    }
    absolute           = buffer;
    const DWORD length = GetFullPathNameW(wide, capacity, absolute, NULL);
    if (!length) break;
    if (length < capacity) {
      free(wide);
      const bool   unc    = absolute[0] == L'\\' && absolute[1] == L'\\';
      const size_t extra  = unc ? 6 : 4;
      wchar_t*     result = malloc(((size_t)length + extra + 1) * sizeof(*result));
      if (!result) {
        free(absolute);
        Error_set_string(error, "Allocating a Windows path failed");
        return NULL;
      }
      wcscpy(result, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\");
      wcscat(result, absolute + (unc ? 2 : 0));
      free(absolute);
      return result;
    }
    capacity = length;
  }
  windows_set_error(error, "Resolving file path", GetLastError());
  free(absolute);
  free(wide);
  return NULL;
}
#endif
