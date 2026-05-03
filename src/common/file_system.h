/*
 * FileSystem:: namespace collapsed to fs_ prefix.  std::string returns
 * become heap char* (caller frees with free()) or write into a caller-
 * supplied small_string_t*.  std::optional<T> becomes bool + out-pointer.
 * std::vector<T> returns become (T* data, size_t count) heap pairs plus a
 * dedicated free helper.  std::span<T> becomes (T* data, size_t count).
 *
 * ManagedCFilePtr (unique_ptr<FILE,FileDeleter>) is dropped - callers hold
 * a plain FILE* and call fclose themselves.  POSIXLock / LockedFile /
 * AtomicRenamedFile RAII wrappers and the unused PSF/archive helpers are
 * not ported; consumers will be reworked when reached.
 *
 * FileShareMode is preserved as an enum for source compatibility but is a
 * no-op on Linux: there is no Win32-style mandatory sharing, and any callers
 * passing it to fs_open_shared_file simply ignore it.  All Open* variants
 * collapse to fs_open_file(path, mode, err) returning FILE*.
 */

#ifndef CUPID_COMMON_FILE_SYSTEM_H
#define CUPID_COMMON_FILE_SYSTEM_H

#include "small_string.h"
#include "types.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

typedef struct Error Error;

enum {
  FS_FILE_ATTRIBUTE_DIRECTORY  = (1u << 0),
  FS_FILE_ATTRIBUTE_READ_ONLY  = (1u << 1),
  FS_FILE_ATTRIBUTE_COMPRESSED = (1u << 2),
  FS_FILE_ATTRIBUTE_LINK       = (1u << 3),
};

enum {
  FS_FIND_RECURSIVE      = (1u << 0),
  FS_FIND_RELATIVE_PATHS = (1u << 1),
  FS_FIND_HIDDEN_FILES   = (1u << 2),
  FS_FIND_FOLDERS        = (1u << 3),
  FS_FIND_FILES          = (1u << 4),
  FS_FIND_KEEP_ARRAY     = (1u << 5),
  FS_FIND_SORT_BY_NAME   = (1u << 6),
};

typedef enum {
  FS_FILE_SHARE_DENY_READ_WRITE = 0,
  FS_FILE_SHARE_DENY_WRITE      = 1,
  FS_FILE_SHARE_DENY_READ       = 2,
  FS_FILE_SHARE_DENY_NONE       = 3,
} fs_file_share_mode_t;

typedef struct {
  time_t creation_time;     /* inode change time on linux */
  time_t modification_time;
  s64    size;
  u32    attributes;
} fs_stat_data_t;

typedef struct {
  time_t creation_time;
  time_t modification_time;
  char*  file_name;         /* heap, owned by struct */
  s64    size;
  u32    attributes;
} fs_find_data_t;

 /* Returns a heap array of NUL-terminated paths.  Free with
 * fs_free_root_directory_list(arr, count). */
bool fs_get_root_directory_list(char*** out_paths, size_t* out_count);
void fs_free_root_directory_list(char** paths, size_t count);

bool fs_find_files(const char* path, const char* pattern, u32 flags,
                   fs_find_data_t** out_results, size_t* out_count);
void fs_free_find_data_array(fs_find_data_t* arr, size_t count);

bool fs_stat_path(const char* path, struct stat* st, Error* error);
bool fs_stat_file(FILE* fp, struct stat* st, Error* error);
bool fs_stat_path_data(const char* path, fs_stat_data_t* sd, Error* error);
bool fs_stat_file_data(FILE* fp, fs_stat_data_t* sd, Error* error);
s64  fs_get_path_file_size(const char* path);

bool fs_file_exists      (const char* path);
bool fs_directory_exists (const char* path);
bool fs_is_real_directory(const char* path); /* false for symlinks */
bool fs_is_directory_empty(const char* path);

bool fs_delete_file(const char* path, Error* error);
bool fs_rename_path(const char* old_path, const char* new_path, Error* error);
bool fs_delete_directory(const char* path, Error* error);
bool fs_recursive_delete_directory(const char* path, Error* error);

/* mode is fopen mode string.  Returns NULL on failure.  Caller must fclose. */
FILE* fs_open_file(const char* path, const char* mode, Error* error);

FILE* fs_open_shared_file(const char* path, const char* mode,
                          fs_file_share_mode_t share_mode, Error* error);

/* Opens a temporary file based on base_path.  If out_path is non-NULL, it
 * receives the heap-allocated path string (caller frees with free()).
 * Returns NULL on failure. */
FILE* fs_open_temporary_file(const char* base_path_data, u32 base_path_len,
                             char** out_path, Error* error);

 /* Opens existing file r+b; if missing, creates it (raced with w+bx fallback).
 * retry_ms is ignored on Linux (no Win32 sharing-violation retry needed). */
FILE* fs_open_existing_or_create_file(const char* path, s32 retry_ms, Error* error);

/* fd-based open. */
int fs_open_fd_file(const char* path, int flags, int mode, Error* error);

int  fs_fseek64(FILE* fp, s64 offset, int whence);
bool fs_fseek64_e(FILE* fp, s64 offset, int whence, Error* error);
s64  fs_ftell64(FILE* fp);
s64  fs_fsize64(FILE* fp, Error* error);
bool fs_ftruncate64(FILE* fp, s64 size, Error* error);

 /* Read whole file as bytes; allocates *out_data on heap (caller frees with
 * free()).  *out_len receives byte count.  Returns false on error. */
bool fs_read_binary_file_path(const char* path, u8** out_data, size_t* out_len, Error* error);
bool fs_read_binary_file     (FILE* fp,         u8** out_data, size_t* out_len, Error* error);

 /* Read whole file as text.  *out_data is NUL-terminated; *out_len is byte
 * length excluding the NUL.  Strips a leading UTF-8 BOM.  Caller frees
 * *out_data with free(). */
bool fs_read_file_to_string_path(const char* path, char** out_data, size_t* out_len, Error* error);
bool fs_read_file_to_string     (FILE* fp,         char** out_data, size_t* out_len, Error* error);

bool fs_write_binary_file(const char* path, const void* data, size_t data_length, Error* error);
bool fs_write_string_to_file_view(const char* path, const char* data, u32 len, Error* error);
bool fs_write_string_to_file_cstr(const char* path, const char* str, Error* error);

bool fs_create_directory       (const char* path, bool recursive, Error* error);
bool fs_ensure_directory_exists(const char* path, bool recursive, Error* error);
bool fs_copy_file_path         (const char* source, const char* destination, bool replace, Error* error);

/* Returns heap-allocated absolute path (caller frees with free()). */
char* fs_get_program_path   (Error* error);
char* fs_get_working_directory(void);
bool  fs_set_working_directory(const char* path);

/* No-op on non-Windows; preserved for source compatibility. */
bool fs_set_path_compression(const char* path, bool enable);

/* Linux-specific: chmod +/-x */
bool fs_set_path_executable(const char* path, bool executable, Error* error);

#endif /* CUPID_COMMON_FILE_SYSTEM_H */
