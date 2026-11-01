/* cupid-ps1 Windows file streams, metadata and directory traversal. */
#include "common/file_system.h"
#if defined(_WIN32)
#include "windows_util.h"
#include "common/error.h"
#include "common/string_util.h"
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <wchar.h>
#include <winioctl.h>

static DWORD path_attributes(const char* path) {
  wchar_t* wide = windows_path_to_wide(path, NULL);
  if (!wide) return INVALID_FILE_ATTRIBUTES;
  const DWORD result = GetFileAttributesW(wide);
  free(wide);
  return result;
}

static u32 translate_attributes(DWORD attributes) {
  return ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? FS_FILE_ATTRIBUTE_DIRECTORY : 0) |
         ((attributes & FILE_ATTRIBUTE_READONLY) ? FS_FILE_ATTRIBUTE_READ_ONLY : 0) |
         ((attributes & FILE_ATTRIBUTE_COMPRESSED) ? FS_FILE_ATTRIBUTE_COMPRESSED : 0) |
         ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? FS_FILE_ATTRIBUTE_LINK : 0);
}

static time_t file_time(FILETIME value) {
  const u64 seconds = (((u64)value.dwHighDateTime << 32) | value.dwLowDateTime) / 10000000ull;
  const u64 epoch   = 11644473600ull;
  return seconds >= epoch ? (time_t)(seconds - epoch) : -(time_t)(epoch - seconds);
}

static char* path_utf8(const wchar_t* wide, Error* error) {
  char* result;
  if (!wcsncmp(wide, L"\\\\?\\UNC\\", 8)) {
    char* tail = windows_wide_to_utf8(wide + 8, error);
    if (!tail) return NULL;
    const size_t length = strlen(tail);
    result              = malloc(length + 3);
    if (result) {
      memcpy(result, "//", 2);
      memcpy(result + 2, tail, length + 1);
    }
    free(tail);
  } else
    result = windows_wide_to_utf8(!wcsncmp(wide, L"\\\\?\\", 4) ? wide + 4 : wide, error);
  if (result)
    for (char* p = result; *p; p++)
      if (*p == '\\') *p = '/';
  return result;
}

static char* join_path(const char* base, const char* name) {
  const size_t n = strlen(base), m = strlen(name);
  const bool   separator = n && base[n - 1] != '/' && base[n - 1] != '\\';
  if (n > SIZE_MAX - m - 2) return NULL;
  char* result = malloc(n + m + 2);
  if (!result) return NULL;
  memcpy(result, base, n);
  if (separator) result[n] = '/';
  memcpy(result + n + separator, name, m + 1);
  return result;
}

static bool stat_handle(HANDLE handle, fs_stat_data_t* result, Error* error) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) {
    windows_set_error(error, "Reading file metadata", GetLastError());
    return false;
  }
  result->creation_time     = file_time(info.ftCreationTime);
  result->modification_time = file_time(info.ftLastWriteTime);
  result->attributes        = translate_attributes(info.dwFileAttributes);
  result->size              = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                                  ? 0
                                  : (s64)(((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow);
  return true;
}

bool fs_stat_path_data(const char* path, fs_stat_data_t* result, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  const DWORD attributes = GetFileAttributesW(wide);
  HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  const DWORD code = GetLastError();
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    windows_set_error(error, "Opening file metadata", code);
    return false;
  }
  const bool ok = stat_handle(handle, result, error);
  CloseHandle(handle);
  if (ok && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    result->attributes |= FS_FILE_ATTRIBUTE_LINK;
  return ok;
}

bool fs_stat_file_data(FILE* fp, fs_stat_data_t* result, Error* error) {
  const intptr_t handle = _get_osfhandle(_fileno(fp));
  if (handle == -1) {
    Error_set_errno(error, errno);
    return false;
  }
  return stat_handle((HANDLE)handle, result, error);
}

static bool raw_stat_handle(HANDLE handle, fs_native_stat_t* result, Error* error) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) {
    windows_set_error(error, "Reading native file metadata", GetLastError());
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->st_dev   = info.dwVolumeSerialNumber;
  result->st_rdev  = result->st_dev;
  result->st_ino   = (unsigned short)info.nFileIndexLow;
  result->st_nlink = (short)info.nNumberOfLinks;
  result->st_mode  = (unsigned short)(((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR : _S_IFREG) |
                                     _S_IREAD | ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0 : _S_IWRITE));
  result->st_size  = (s64)(((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow);
  result->st_ctime = file_time(info.ftCreationTime);
  result->st_mtime = file_time(info.ftLastWriteTime);
  result->st_atime = file_time(info.ftLastAccessTime);
  return true;
}

bool fs_stat_path(const char* path, fs_native_stat_t* result, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  const DWORD code = GetLastError();
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    windows_set_error(error, "Opening native file metadata", code);
    return false;
  }
  const bool ok = raw_stat_handle(handle, result, error);
  CloseHandle(handle);
  return ok;
}

bool fs_stat_file(FILE* fp, fs_native_stat_t* result, Error* error) {
  const intptr_t handle = _get_osfhandle(_fileno(fp));
  if (handle == -1) {
    Error_set_errno(error, errno);
    return false;
  }
  return raw_stat_handle((HANDLE)handle, result, error);
}

s64 fs_get_path_file_size(const char* path) {
  fs_stat_data_t result;
  return fs_stat_path_data(path, &result, NULL) ? result.size : -1;
}
bool fs_file_exists(const char* path) {
  fs_stat_data_t result;
  return fs_stat_path_data(path, &result, NULL) && !(result.attributes & FS_FILE_ATTRIBUTE_DIRECTORY);
}
bool fs_directory_exists(const char* path) {
  fs_stat_data_t result;
  return fs_stat_path_data(path, &result, NULL) && (result.attributes & FS_FILE_ATTRIBUTE_DIRECTORY);
}
bool fs_is_real_directory(const char* path) {
  const DWORD attributes = path_attributes(path);
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static FILE* stream_from_handle(HANDLE handle, int flags, const char* mode, Error* error) {
  const int fd = _open_osfhandle((intptr_t)handle, flags);
  if (fd < 0) {
    Error_set_errno(error, errno);
    CloseHandle(handle);
    return NULL;
  }
  FILE* result = _fdopen(fd, mode);
  if (!result) {
    Error_set_errno(error, errno);
    _close(fd);
  }
  return result;
}

FILE* fs_open_shared_file(const char* path, const char* mode, fs_file_share_mode_t share_mode, Error* error) {
  if (!mode || !*mode || share_mode > FS_FILE_SHARE_DENY_NONE) {
    Error_set_errno(error, EINVAL);
    return NULL;
  }
  const bool plus = strchr(mode, '+') != NULL, binary = strchr(mode, 'b') != NULL;
  const bool exclusive = strchr(mode, 'x') != NULL;
  DWORD      access, disposition;
  int        flags;
  switch (mode[0]) {
    case 'r':
      access      = GENERIC_READ | (plus ? GENERIC_WRITE : 0);
      disposition = OPEN_EXISTING;
      flags       = plus ? _O_RDWR : _O_RDONLY;
      break;
    case 'w':
      access      = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
      disposition = exclusive ? CREATE_NEW : CREATE_ALWAYS;
      flags       = plus ? _O_RDWR : _O_WRONLY;
      break;
    case 'a':
      access      = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
      disposition = OPEN_ALWAYS;
      flags       = (plus ? _O_RDWR : _O_WRONLY) | _O_APPEND;
      break;
    default:
      Error_set_errno(error, EINVAL);
      return NULL;
  }
  if (exclusive && mode[0] != 'w') {
    Error_set_errno(error, EINVAL);
    return NULL;
  }
  for (const char* p = mode + 1; *p; p++)
    if (*p != '+' && *p != 'b' && *p != 't' && *p != 'x') {
      Error_set_errno(error, EINVAL);
      return NULL;
    }
  DWORD sharing = FILE_SHARE_DELETE;
  if (share_mode == FS_FILE_SHARE_DENY_NONE || share_mode == FS_FILE_SHARE_DENY_WRITE) sharing |= FILE_SHARE_READ;
  if (share_mode == FS_FILE_SHARE_DENY_NONE || share_mode == FS_FILE_SHARE_DENY_READ) sharing |= FILE_SHARE_WRITE;
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return NULL;
  HANDLE      handle = CreateFileW(wide, access, sharing, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
  const DWORD code   = GetLastError();
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    windows_set_error(error, "Opening file", code);
    return NULL;
  }
  char clean_mode[4] = {mode[0], binary ? 'b' : 't', plus ? '+' : '\0', '\0'};
  return stream_from_handle(handle, flags | (binary ? _O_BINARY : _O_TEXT), clean_mode, error);
}

FILE* fs_open_file(const char* path, const char* mode, Error* error) {
  return fs_open_shared_file(path, mode, FS_FILE_SHARE_DENY_NONE, error);
}

FILE* fs_open_anonymous_file(Error* error) {
  const DWORD length = GetTempPathW(0, NULL);
  wchar_t*    wide   = length ? malloc((size_t)length * sizeof(*wide)) : NULL;
  if (!wide) {
    Error_set_errno(error, ENOMEM);
    return NULL;
  }
  const DWORD actual = GetTempPathW(length, wide);
  if (!actual || actual >= length) {
    windows_set_error(error, "Finding temporary directory", GetLastError());
    free(wide);
    return NULL;
  }
  char* directory = path_utf8(wide, error);
  free(wide);
  if (!directory) return NULL;
  char* base = join_path(directory, "cupid-ps1");
  free(directory);
  if (!base) {
    Error_set_errno(error, ENOMEM);
    return NULL;
  }
  FILE* fp = fs_open_temporary_file(base, (u32)strlen(base), NULL, error);
  free(base);
  return fp;
}

FILE* fs_open_readonly_buffer(const void* data, size_t size, Error* error) {
  if (size && !data) {
    Error_set_errno(error, EINVAL);
    return NULL;
  }
  FILE* scratch = fs_open_anonymous_file(error);
  if (!scratch) return NULL;
  if ((size && fwrite(data, 1, size, scratch) != size) || fflush(scratch) || fs_fseek64(scratch, 0, SEEK_SET)) {
    Error_set_errno(error, errno);
    fclose(scratch);
    return NULL;
  }
  HANDLE readonly;
  if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(_fileno(scratch)), GetCurrentProcess(), &readonly,
                       GENERIC_READ | SYNCHRONIZE, FALSE, 0)) {
    windows_set_error(error, "Creating read-only buffer stream", GetLastError());
    fclose(scratch);
    return NULL;
  }
  fclose(scratch);
  return stream_from_handle(readonly, _O_RDONLY | _O_BINARY, "rb", error);
}

FILE* fs_open_existing_or_create_file(const char* path, s32 retry_ms, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return NULL;
  const ULONGLONG deadline = GetTickCount64() + (retry_ms > 0 ? (u32)retry_ms : 0);
  for (;;) {
    HANDLE handle =
        CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle != INVALID_HANDLE_VALUE) {
      free(wide);
      return stream_from_handle(handle, _O_RDWR | _O_BINARY, "r+b", error);
    }
    const DWORD     code = GetLastError();
    const ULONGLONG now  = GetTickCount64();
    if ((code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) || now >= deadline) {
      windows_set_error(error, "Opening or creating file", code);
      free(wide);
      return NULL;
    }
    Sleep((DWORD)((deadline - now) < 10 ? deadline - now : 10));
  }
}

int fs_open_fd_file(const char* path, int flags, int mode, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return -1;
  const int result = _wopen(wide, flags, mode);
  free(wide);
  if (result < 0) Error_set_errno(error, errno);
  return result;
}

int fs_fseek64(FILE* fp, s64 offset, int whence) {
  return _fseeki64(fp, offset, whence);
}
s64 fs_ftell64(FILE* fp) {
  return _ftelli64(fp);
}
bool fs_ftruncate64(FILE* fp, s64 size, Error* error) {
  if (size < 0) {
    Error_set_errno(error, EINVAL);
    return false;
  }
  if (fflush(fp) != 0) {
    Error_set_errno(error, errno);
    return false;
  }
  const errno_t code = _chsize_s(_fileno(fp), size);
  if (code) Error_set_errno(error, code);
  return code == 0;
}

bool fs_flush_file(FILE* fp, Error* error) {
  if (fflush(fp) != 0) {
    Error_set_errno(error, errno);
    return false;
  }
  const intptr_t handle = _get_osfhandle(_fileno(fp));
  if (handle == -1) {
    Error_set_errno(error, errno);
    return false;
  }
  if (FlushFileBuffers((HANDLE)handle)) return true;
  windows_set_error(error, "Synchronizing file", GetLastError());
  return false;
}

static volatile LONG s_temp_serial;
FILE*                fs_open_temporary_file(const char* base, u32 length, char** out_path, Error* error) {
  if (out_path) *out_path = NULL;
  if (!length || !base || memchr(base, 0, length)) {
    Error_set_errno(error, EINVAL);
    return NULL;
  }
  char* name = malloc((size_t)length + 64);
  if (!name) {
    Error_set_string(error, "Allocating a temporary filename failed");
    return NULL;
  }
  memcpy(name, base, length);
  for (unsigned attempt = 0; attempt < 128; attempt++) {
    snprintf(name + length, 64, ".%lu.%lu.tmp", (unsigned long)GetCurrentProcessId(),
                            (unsigned long)InterlockedIncrement(&s_temp_serial));
    wchar_t* wide = windows_path_to_wide(name, error);
    if (!wide) {
      free(name);
      return NULL;
    }
    HANDLE handle =
        CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW,
                                   FILE_ATTRIBUTE_TEMPORARY | (out_path ? 0 : FILE_FLAG_DELETE_ON_CLOSE), NULL);
    const DWORD code = GetLastError();
    if (handle != INVALID_HANDLE_VALUE) {
      FILE* result = stream_from_handle(handle, _O_RDWR | _O_BINARY, "w+b", error);
      if (!result) DeleteFileW(wide);
      free(wide);
      if (result && out_path)
        *out_path = name;
      else
        free(name);
      return result;
    }
    free(wide);
    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
      windows_set_error(error, "Creating a temporary file", code);
      free(name);
      return NULL;
    }
  }
  free(name);
  Error_set_string(error, "Temporary filenames are already in use");
  return NULL;
}

bool fs_rename_path(const char* old_path, const char* new_path, Error* error) {
  wchar_t* source      = windows_path_to_wide(old_path, error);
  wchar_t* destination = windows_path_to_wide(new_path, error);
  if (!source || !destination) {
    free(source);
    free(destination);
    return false;
  }
  HANDLE handle = CreateFileW(source, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  bool   ok     = false;
  DWORD  code   = GetLastError();
  if (handle != INVALID_HANDLE_VALUE) {
    const size_t      name_bytes = wcslen(destination) * sizeof(*destination);
    const size_t      size       = offsetof(FILE_RENAME_INFO, FileName) + name_bytes;
    FILE_RENAME_INFO* rename     = calloc(1, size);
    if (!rename)
      code = ERROR_NOT_ENOUGH_MEMORY;
    else {
      rename->Flags          = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
      rename->FileNameLength = (DWORD)name_bytes;
      memcpy(rename->FileName, destination, name_bytes);
      /* FileRenameInfoEx is class 22; older SDK enums omit the Win10 entries. */
      ok   = SetFileInformationByHandle(handle, (FILE_INFO_BY_HANDLE_CLASS)22, rename, (DWORD)size) != 0;
      code = GetLastError();
      free(rename);
    }
    CloseHandle(handle);
    if (!ok && (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION)) {
      ok   = MoveFileExW(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
      code = GetLastError();
    }
  }
  if (!ok) windows_set_error(error, "Replacing file path", code);
  free(source);
  free(destination);
  return ok;
}

bool fs_copy_file_path(const char* source, const char* destination, bool replace, Error* error) {
  wchar_t* src = windows_path_to_wide(source, error);
  wchar_t* dst = windows_path_to_wide(destination, error);
  if (!src || !dst) {
    free(src);
    free(dst);
    return false;
  }
  const bool ok = CopyFileW(src, dst, !replace) != 0;
  if (!ok) windows_set_error(error, "Copying file", GetLastError());
  free(src);
  free(dst);
  return ok;
}

bool fs_delete_file(const char* path, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  const DWORD attributes = GetFileAttributesW(wide);
  bool        ok         = false;
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        ok = RemoveDirectoryW(wide) != 0;
      else
        SetLastError(ERROR_DIRECTORY);
    } else
      ok = DeleteFileW(wide) != 0;
  }
  if (!ok) windows_set_error(error, "Deleting file", GetLastError());
  free(wide);
  return ok;
}

bool fs_delete_directory(const char* path, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  const bool ok = RemoveDirectoryW(wide) != 0;
  if (!ok) windows_set_error(error, "Removing directory", GetLastError());
  free(wide);
  return ok;
}

static size_t root_length(const wchar_t* path) {
  if (_wcsnicmp(path, L"\\\\?\\UNC\\", 8) == 0) {
    const wchar_t* server = wcschr(path + 8, L'\\');
    if (!server) return wcslen(path);
    const wchar_t* share = wcschr(server + 1, L'\\');
    return share ? (size_t)(share - path + 1) : wcslen(path);
  }
  if (!wcsncmp(path, L"\\\\?\\", 4) || !wcsncmp(path, L"\\\\.\\", 4)) {
    const wchar_t* end = wcschr(path + 4, L'\\');
    return end ? (size_t)(end - path + 1) : wcslen(path);
  }
  return 0;
}

bool fs_create_directory(const char* path, bool recursive, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  const size_t length = wcslen(wide), root = root_length(wide);
  bool         ok = true;
  if (recursive) {
    for (size_t i = root; i < length; i++) {
      if (wide[i] != L'\\') continue;
      wide[i] = 0;
      if (!CreateDirectoryW(wide, NULL) && !(GetFileAttributesW(wide) & FILE_ATTRIBUTE_DIRECTORY)) ok = false;
      if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) ok = false;
      wide[i] = L'\\';
      if (!ok) break;
    }
  }
  if (ok && !CreateDirectoryW(wide, NULL)) {
    const DWORD attributes = GetFileAttributesW(wide);
    ok                     = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
  }
  if (!ok) windows_set_error(error, "Creating directory", GetLastError());
  free(wide);
  return ok;
}

static wchar_t* append_wide(const wchar_t* path, const wchar_t* name) {
  const size_t n = wcslen(path), m = wcslen(name);
  wchar_t*     result = malloc((n + m + 2) * sizeof(*result));
  if (!result) return NULL;
  memcpy(result, path, n * sizeof(*result));
  size_t at = n;
  if (at && result[at - 1] != L'\\') result[at++] = L'\\';
  memcpy(result + at, name, (m + 1) * sizeof(*result));
  return result;
}

static bool delete_tree(const wchar_t* path, Error* error) {
  const DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    windows_set_error(error, "Reading directory attributes", GetLastError());
    return false;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    Error_set_errno(error, ENOTDIR);
    return false;
  }
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    wchar_t* search = append_wide(path, L"*");
    if (!search) {
      Error_set_errno(error, ENOMEM);
      return false;
    }
    WIN32_FIND_DATAW entry;
    HANDLE           find = FindFirstFileW(search, &entry);
    const DWORD      code = GetLastError();
    free(search);
    if (find == INVALID_HANDLE_VALUE && code != ERROR_FILE_NOT_FOUND) {
      windows_set_error(error, "Enumerating directory", code);
      return false;
    }
    if (find != INVALID_HANDLE_VALUE) {
      bool ok = true;
      do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        wchar_t* child = append_wide(path, entry.cFileName);
        if (!child) {
          Error_set_errno(error, ENOMEM);
          ok = false;
          break;
        }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
          ok = delete_tree(child, error);
        else if (!DeleteFileW(child)) {
          windows_set_error(error, "Deleting directory entry", GetLastError());
          ok = false;
        }
        free(child);
        if (!ok) break;
      } while (FindNextFileW(find, &entry));
      const DWORD last = GetLastError();
      FindClose(find);
      if (!ok) return false;
      if (last != ERROR_NO_MORE_FILES) {
        windows_set_error(error, "Enumerating directory", last);
        return false;
      }
    }
  }
  if (RemoveDirectoryW(path)) return true;
  windows_set_error(error, "Removing directory", GetLastError());
  return false;
}

bool fs_recursive_delete_directory(const char* path, Error* error) {
  wchar_t* wide = windows_path_to_wide(path, error);
  if (!wide) return false;
  if (wcslen(wide) <= root_length(wide)) {
    free(wide);
    Error_set_string(error, "Cannot remove a filesystem root");
    return false;
  }
  const bool ok = delete_tree(wide, error);
  free(wide);
  return ok;
}

typedef struct {
  DWORD volume;
  u64   index;
} directory_id_t;
typedef struct {
  fs_find_data_t* data;
  size_t          count, capacity;
  directory_id_t* visited;
  size_t          visited_count;
} find_context_t;

static bool find_recursive(find_context_t* context, const char* origin, const char* relative, const char* pattern,
                           u32 flags) {
  char* current = *relative ? join_path(origin, relative) : strdup(origin);
  if (!current) return false;
  wchar_t* wide = windows_path_to_wide(current, NULL);
  free(current);
  if (!wide) return false;
  HANDLE directory = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  BY_HANDLE_FILE_INFORMATION info;
  const bool have_info = directory != INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory, &info);
  if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
  if (!have_info) {
    free(wide);
    return false;
  }
  const directory_id_t id = {info.dwVolumeSerialNumber, ((u64)info.nFileIndexHigh << 32) | info.nFileIndexLow};
  for (size_t i = 0; i < context->visited_count; i++) {
    if (context->visited[i].volume == id.volume && context->visited[i].index == id.index) {
      free(wide);
      return true;
    }
  }
  directory_id_t* ids = realloc(context->visited, (context->visited_count + 1) * sizeof(*ids));
  if (!ids) {
    free(wide);
    return false;
  }
  context->visited              = ids;
  ids[context->visited_count++] = id;
  wchar_t* search               = append_wide(wide, L"*");
  free(wide);
  if (!search) return false;
  WIN32_FIND_DATAW entry;
  HANDLE           find = FindFirstFileW(search, &entry);
  const DWORD      code = GetLastError();
  free(search);
  if (find == INVALID_HANDLE_VALUE) return code == ERROR_FILE_NOT_FOUND;
  bool ok = true;
  do {
    if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
    if (!(flags & FS_FIND_HIDDEN_FILES) &&
        ((entry.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || entry.cFileName[0] == L'.'))
      continue;
    char* name = windows_wide_to_utf8(entry.cFileName, NULL);
    char* rel  = name ? (*relative ? join_path(relative, name) : strdup(name)) : NULL;
    if (!name || !rel) {
      free(name);
      free(rel);
      ok = false;
      break;
    }
    const bool is_directory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory && (flags & FS_FIND_RECURSIVE)) ok = find_recursive(context, origin, rel, pattern, flags);
    const bool selected = is_directory ? (flags & FS_FIND_FOLDERS) != 0 : (flags & FS_FIND_FILES) != 0;
    if (ok && selected && string_util_wildcard_match(name, pattern, false)) {
      if (context->count == context->capacity) {
        const size_t    capacity = context->capacity ? context->capacity * 2 : 16;
        fs_find_data_t* data     = realloc(context->data, capacity * sizeof(*data));
        if (!data)
          ok = false;
        else {
          context->data     = data;
          context->capacity = capacity;
        }
      }
      if (ok) {
        char* result_name = (flags & FS_FIND_RELATIVE_PATHS) ? strdup(rel) : join_path(origin, rel);
        if (!result_name)
          ok = false;
        else
          context->data[context->count++] =
              (fs_find_data_t){file_time(entry.ftCreationTime), file_time(entry.ftLastWriteTime), result_name,
                               (s64)(((u64)entry.nFileSizeHigh << 32) | entry.nFileSizeLow),
                               translate_attributes(entry.dwFileAttributes)};
      }
    }
    free(name);
    free(rel);
    if (!ok) break;
  } while (FindNextFileW(find, &entry));
  const DWORD last = GetLastError();
  FindClose(find);
  return ok && last == ERROR_NO_MORE_FILES;
}

static int compare_find(const void* a, const void* b) {
  const fs_find_data_t *x = a, *y = b;
  const bool            xd = (x->attributes & FS_FILE_ATTRIBUTE_DIRECTORY) != 0,
             yd            = (y->attributes & FS_FILE_ATTRIBUTE_DIRECTORY) != 0;
  return xd != yd ? (xd ? -1 : 1) : string_util_strcasecmp(x->file_name, y->file_name);
}

bool fs_find_files(const char* path, const char* pattern, u32 flags, fs_find_data_t** out, size_t* count) {
  const size_t   old_count = (flags & FS_FIND_KEEP_ARRAY) ? *count : 0;
  find_context_t context   = {(flags & FS_FIND_KEEP_ARRAY) ? *out : NULL, old_count, old_count, NULL, 0};
  const bool     ok        = path && pattern && find_recursive(&context, path, "", pattern, flags);
  free(context.visited);
  if (!ok) {
    for (size_t i = old_count; i < context.count; i++)
      free(context.data[i].file_name);
    context.count = old_count;
  }
  if ((flags & FS_FIND_SORT_BY_NAME) && context.count)
    qsort(context.data, context.count, sizeof(*context.data), compare_find);
  if (!context.count) {
    free(context.data);
    context.data = NULL;
  }
  *out   = context.data;
  *count = context.count;
  return ok && context.count > old_count;
}

bool fs_is_directory_empty(const char* path) {
  fs_find_data_t* entries = NULL;
  size_t          count   = 0;
  const bool found = fs_find_files(path, "*", FS_FIND_FILES | FS_FIND_FOLDERS | FS_FIND_HIDDEN_FILES, &entries, &count);
  fs_free_find_data_array(entries, count);
  return !found;
}

bool fs_get_root_directory_list(char*** out, size_t* count) {
  *out            = NULL;
  *count          = 0;
  DWORD    length = GetLogicalDriveStringsW(0, NULL);
  wchar_t* drives = calloc((size_t)length + 1, sizeof(*drives));
  if (!length || !drives) {
    free(drives);
    return false;
  }
  if (GetLogicalDriveStringsW(length, drives) >= length) {
    free(drives);
    return false;
  }
  char** paths = calloc(26, sizeof(*paths));
  if (!paths) {
    free(drives);
    return false;
  }
  for (const wchar_t* path = drives; *path; path += wcslen(path) + 1) {
    if (*count == 26 || !(paths[*count] = path_utf8(path, NULL))) {
      fs_free_root_directory_list(paths, *count);
      free(drives);
      *count = 0;
      return false;
    }
    (*count)++;
  }
  free(drives);
  *out = paths;
  return true;
}

char* fs_get_program_path(Error* error) {
  DWORD    capacity = 512;
  wchar_t* buffer   = NULL;
  for (;;) {
    wchar_t* next = realloc(buffer, (size_t)capacity * sizeof(*next));
    if (!next) {
      free(buffer);
      Error_set_errno(error, ENOMEM);
      return NULL;
    }
    buffer             = next;
    const DWORD length = GetModuleFileNameW(NULL, buffer, capacity);
    if (!length) {
      windows_set_error(error, "Reading executable path", GetLastError());
      free(buffer);
      return NULL;
    }
    if (length < capacity) {
      char* result = path_utf8(buffer, error);
      free(buffer);
      return result;
    }
    capacity *= 2;
  }
}

char* fs_get_working_directory(void) {
  DWORD    capacity = GetCurrentDirectoryW(0, NULL);
  wchar_t* buffer   = NULL;
  while (capacity) {
    wchar_t* next = realloc(buffer, (size_t)capacity * sizeof(*next));
    if (!next) {
      free(buffer);
      return NULL;
    }
    buffer             = next;
    const DWORD length = GetCurrentDirectoryW(capacity, buffer);
    if (!length) break;
    if (length < capacity) {
      char* result = path_utf8(buffer, NULL);
      free(buffer);
      return result;
    }
    capacity = length;
  }
  free(buffer);
  return NULL;
}

bool fs_set_working_directory(const char* path) {
  wchar_t* wide = windows_path_to_wide(path, NULL);
  if (!wide) return false;
  const bool ok = SetCurrentDirectoryW(wide) != 0;
  free(wide);
  return ok;
}

bool fs_set_path_compression(const char* path, bool enable) {
  wchar_t* wide = windows_path_to_wide(path, NULL);
  if (!wide) return false;
  HANDLE handle =
      CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) return false;
  USHORT     format = enable ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;
  DWORD      returned;
  const bool ok =
      DeviceIoControl(handle, FSCTL_SET_COMPRESSION, &format, sizeof(format), NULL, 0, &returned, NULL) != 0;
  CloseHandle(handle);
  return ok;
}

bool fs_set_path_executable(const char* path, bool executable, Error* error) {
  (void)path;
  (void)executable;
  Error_set_string(error, "Unix execute permission bits are unavailable on Windows");
  return false;
}
#endif
