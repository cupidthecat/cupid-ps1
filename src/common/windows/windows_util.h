/* cupid-ps1 Windows SDK and UTF-8 boundary helpers. */
#ifndef CUPID_COMMON_WINDOWS_UTIL_H
#define CUPID_COMMON_WINDOWS_UTIL_H

#include "common/types.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct Error Error;
void                 windows_set_error(Error* error, const char* operation, DWORD code);
wchar_t*             windows_utf8_to_wide(const char* text, Error* error);
char*                windows_wide_to_utf8(const wchar_t* text, Error* error);
wchar_t*             windows_path_to_wide(const char* path, Error* error);
#endif
#endif
