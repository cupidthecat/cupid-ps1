/* cupid-ps1 Windows API boundary conversion and diagnostics. */
#include "windows_util.h"
#if defined(_WIN32)
#include "common/error.h"
#include <stdlib.h>

void windows_set_error(Error* error, const char* operation, DWORD code)
{
  Error_set_string_fmt(error, "%s: Windows error %lu", operation, (unsigned long)code);
}

wchar_t* windows_utf8_to_wide(const char* text, Error* error)
{
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
#endif
