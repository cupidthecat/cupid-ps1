#include "file_system.h"

#include "assert.h"
#include "error.h"
#include "log.h"
#include "path.h"
#include "string_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

LOG_CHANNEL(FileSystem);

/* Heap-printf wrapper.  Returns NULL on failure. */
static char* xasprintf(const char* fmt, ...) PRINTFLIKE(1, 2);
static char* xasprintf(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0) { va_end(ap); return NULL; }
  char* buf = (char*)malloc((size_t)n + 1);
  if (!buf) { va_end(ap); return NULL; }
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

static u32 translate_stat_attributes(const struct stat* st, const struct stat* st_link)
{
  return (S_ISDIR(st->st_mode)        ? FS_FILE_ATTRIBUTE_DIRECTORY : 0u) |
         (S_ISLNK(st_link->st_mode)   ? FS_FILE_ATTRIBUTE_LINK      : 0u);
}

bool fs_get_root_directory_list(char*** out_paths, size_t* out_count)
{
  /* Up to 2 entries: $HOME (when set) plus "/". */
  char** paths = (char**)malloc(sizeof(char*) * 2);
  if (!paths) return false;
  size_t n = 0;

  const char* home = getenv("HOME");
  if (home) {
    paths[n] = strdup(home);
    if (!paths[n]) { free(paths); return false; }
    n++;
  }
  paths[n] = strdup("/");
  if (!paths[n]) { for (size_t i = 0; i < n; i++) free(paths[i]); free(paths); return false; }
  n++;

  *out_paths = paths;
  *out_count = n;
  return true;
}

void fs_free_root_directory_list(char** paths, size_t count)
{
  if (!paths) return;
  for (size_t i = 0; i < count; i++)
    free(paths[i]);
  free(paths);
}

void fs_free_find_data_array(fs_find_data_t* arr, size_t count)
{
  if (!arr) return;
  for (size_t i = 0; i < count; i++)
    free(arr[i].file_name);
  free(arr);
}

/* Realpath wrapper used to detect symlink loops during recursive find.  We
 * don't have Path::RealPath ported because it requires SplitNativePath;
 * realpath(3) is sufficient when the path actually exists on disk. */
static char* compute_realpath(const char* path)
{
  return realpath(path, NULL);
}

 /* Visited-paths stack used to break symlink loops in find_files_recursive.
 * Realpath strings are owned. */
typedef struct {
  char**  data;
  size_t  count;
  size_t  capacity;
} visited_stack_t;

static void visited_init(visited_stack_t* v)
{
  v->data = NULL; v->count = 0; v->capacity = 0;
}

static void visited_destroy(visited_stack_t* v)
{
  for (size_t i = 0; i < v->count; i++) free(v->data[i]);
  free(v->data);
}

static bool visited_contains(const visited_stack_t* v, const char* path)
{
  for (size_t i = 0; i < v->count; i++) {
    if (strcmp(v->data[i], path) == 0)
      return true;
  }
  return false;
}

static bool visited_push(visited_stack_t* v, char* path /* takes ownership */)
{
  if (v->count == v->capacity) {
    size_t nc = v->capacity ? v->capacity * 2 : 8;
    char** np = (char**)realloc(v->data, sizeof(char*) * nc);
    if (!np) { free(path); return false; }
    v->data = np;
    v->capacity = nc;
  }
  v->data[v->count++] = path;
  return true;
}

/* Dynamic find_data array used while building results. */
typedef struct {
  fs_find_data_t* data;
  size_t          count;
  size_t          capacity;
} find_array_t;

static void find_array_init(find_array_t* a)
{
  a->data = NULL; a->count = 0; a->capacity = 0;
}

static bool find_array_push(find_array_t* a, fs_find_data_t entry /* takes ownership of file_name */)
{
  if (a->count == a->capacity) {
    size_t nc = a->capacity ? a->capacity * 2 : 16;
    fs_find_data_t* np = (fs_find_data_t*)realloc(a->data, sizeof(fs_find_data_t) * nc);
    if (!np) { free(entry.file_name); return false; }
    a->data = np;
    a->capacity = nc;
  }
  a->data[a->count++] = entry;
  return true;
}

static u32 find_files_recursive(const char* origin_path, const char* parent_path, const char* path,
                                const char* pattern, u32 flags, find_array_t* results, visited_stack_t* visited)
{

  char* search_dir = NULL;
  if (path) {
    if (parent_path)
      search_dir = xasprintf("%s/%s/%s", origin_path, parent_path, path);
    else
      search_dir = xasprintf("%s/%s", origin_path, path);
  } else {
    search_dir = strdup(origin_path);
  }
  if (!search_dir) return 0;

  DIR* dirp = opendir(search_dir);
  if (!dirp) { free(search_dir); return 0; }
  free(search_dir);

  /* Single '*' pattern is a hot path that skips wildcard match. */
  bool has_wildcards = (strpbrk(pattern, "*?") != NULL);
  bool wildcard_match_all = has_wildcards && (strcmp(pattern, "*") == 0);

  u32 n_files = 0;
  struct dirent* ent;
  while ((ent = readdir(dirp)) != NULL) {
    if (ent->d_name[0] == '.') {
      if (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))
        continue;
      if (!(flags & FS_FIND_HIDDEN_FILES))
        continue;
    }

    char* full_path = NULL;
    if (parent_path)
      full_path = xasprintf("%s/%s/%s/%s", origin_path, parent_path, path, ent->d_name);
    else if (path)
      full_path = xasprintf("%s/%s/%s", origin_path, path, ent->d_name);
    else
      full_path = xasprintf("%s/%s", origin_path, ent->d_name);
    if (!full_path) continue;

    struct stat sd, sd_link;
    if (stat(full_path, &sd) < 0 || lstat(full_path, &sd_link) < 0) {
      free(full_path);
      continue;
    }

    fs_find_data_t out_data;
    out_data.attributes = translate_stat_attributes(&sd, &sd_link);
    out_data.file_name = NULL;

    if (S_ISDIR(sd.st_mode)) {
      if (flags & FS_FIND_RECURSIVE) {
        /* Loop-detect via realpath of the resolved directory. */
        char* real_recurse = compute_realpath(full_path);
        if (!real_recurse || !visited_contains(visited, real_recurse)) {
          if (real_recurse) {
            char* keep = real_recurse;
            if (!visited_push(visited, keep)) { /* push consumes */ }
          }

          if (parent_path) {
            char* recurse_dir = xasprintf("%s/%s", parent_path, path);
            if (recurse_dir) {
              n_files += find_files_recursive(origin_path, recurse_dir, ent->d_name, pattern, flags, results, visited);
              free(recurse_dir);
            }
          } else {
            n_files += find_files_recursive(origin_path, path, ent->d_name, pattern, flags, results, visited);
          }
        } else {
          free(real_recurse);
        }
      }

      if (!(flags & FS_FIND_FOLDERS)) {
        free(full_path);
        continue;
      }
    } else {
      if (!(flags & FS_FIND_FILES)) {
        free(full_path);
        continue;
      }
    }

    out_data.size = (s64)sd.st_size;
    out_data.creation_time = sd.st_ctime;
    out_data.modification_time = sd.st_mtime;

    if (has_wildcards) {
      if (!wildcard_match_all && !string_util_wildcard_match(ent->d_name, pattern, true)) {
        free(full_path);
        continue;
      }
    } else {
      if (strcmp(ent->d_name, pattern) != 0) {
        free(full_path);
        continue;
      }
    }

    if (!(flags & FS_FIND_RELATIVE_PATHS)) {
      out_data.file_name = full_path;
    } else {
      char* rel = NULL;
      if (parent_path)
        rel = xasprintf("%s/%s/%s", parent_path, path, ent->d_name);
      else if (path)
        rel = xasprintf("%s/%s", path, ent->d_name);
      else
        rel = strdup(ent->d_name);
      free(full_path);
      if (!rel) continue;
      out_data.file_name = rel;
    }

    if (!find_array_push(results, out_data))
      continue;
    n_files++;
  }

  closedir(dirp);
  return n_files;
}

static int find_data_compare(const void* a, const void* b)
{
  const fs_find_data_t* lhs = (const fs_find_data_t*)a;
  const fs_find_data_t* rhs = (const fs_find_data_t*)b;
  /* Directories first. */
  const u32 ldir = lhs->attributes & FS_FILE_ATTRIBUTE_DIRECTORY;
  const u32 rdir = rhs->attributes & FS_FILE_ATTRIBUTE_DIRECTORY;
  if (ldir != rdir)
    return ldir ? -1 : 1;
  return string_util_strcasecmp(lhs->file_name, rhs->file_name);
}

bool fs_find_files(const char* path, const char* pattern, u32 flags,
                   fs_find_data_t** out_results, size_t* out_count)
{
  find_array_t results;
  if (flags & FS_FIND_KEEP_ARRAY) {
    /* Caller passes back a previously returned array via *out_results / *out_count. */
    results.data     = *out_results;
    results.count    = *out_count;
    results.capacity = *out_count;
  } else {
    find_array_init(&results);
  }

  visited_stack_t visited;
  visited_init(&visited);
  if (flags & FS_FIND_RECURSIVE) {
    char* real_path = compute_realpath(path);
    if (real_path)
      visited_push(&visited, real_path);
  }

  const u32 added = find_files_recursive(path, NULL, NULL, pattern, flags, &results, &visited);

  visited_destroy(&visited);

  /* On a cancelled or empty walk we publish a NULL/0 result regardless
   * of KEEP_ARRAY.  Caller's prior array (if KEEP_ARRAY) is left
   * intact in *out_results / *out_count. */
  if (added == 0) {
    if (!(flags & FS_FIND_KEEP_ARRAY)) {
      fs_free_find_data_array(results.data, results.count);
      *out_results = NULL;
      *out_count   = 0;
    } else {
      *out_results = results.data;
      *out_count   = results.count;
    }
    return false;
  }

  if (flags & FS_FIND_SORT_BY_NAME) {
    qsort(results.data, results.count, sizeof(fs_find_data_t), find_data_compare);
  }

  *out_results = results.data;
  *out_count   = results.count;
  return true;
}

bool fs_stat_path(const char* path, struct stat* st, Error* error)
{
  if (stat(path, st) != 0) {
    Error_set_errno_prefix(error, "stat() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_stat_file(FILE* fp, struct stat* st, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0) {
    Error_set_errno_prefix(error, "fileno() failed: ", errno);
    return false;
  }
  if (fstat(fd, st) != 0) {
    Error_set_errno_prefix(error, "fstat() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_stat_path_data(const char* path, fs_stat_data_t* sd, Error* error)
{
  struct stat ssd, ssd_link;
  if (stat(path, &ssd) < 0 || lstat(path, &ssd_link) < 0) {
    Error_set_errno_prefix(error, "stat() failed: ", errno);
    return false;
  }
  sd->creation_time     = ssd.st_ctime;
  sd->modification_time = ssd.st_mtime;
  sd->attributes        = translate_stat_attributes(&ssd, &ssd_link);
  sd->size              = S_ISREG(ssd.st_mode) ? (s64)ssd.st_size : 0;
  return true;
}

bool fs_stat_file_data(FILE* fp, fs_stat_data_t* sd, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0) {
    Error_set_errno_prefix(error, "fileno() failed: ", errno);
    return false;
  }
  struct stat ssd;
  if (fstat(fd, &ssd) != 0) {
    Error_set_errno_prefix(error, "stat() failed: ", errno);
    return false;
  }
  sd->creation_time     = ssd.st_ctime;
  sd->modification_time = ssd.st_mtime;
  sd->attributes        = translate_stat_attributes(&ssd, &ssd);
  sd->size              = S_ISREG(ssd.st_mode) ? (s64)ssd.st_size : 0;
  return true;
}

s64 fs_get_path_file_size(const char* path)
{
  fs_stat_data_t sd;
  if (!fs_stat_path_data(path, &sd, NULL))
    return -1;
  return sd.size;
}

bool fs_file_exists(const char* path)
{
  struct stat st;
  if (stat(path, &st) < 0) return false;
  return !S_ISDIR(st.st_mode);
}

bool fs_directory_exists(const char* path)
{
  struct stat st;
  if (stat(path, &st) < 0) return false;
  return S_ISDIR(st.st_mode);
}

bool fs_is_real_directory(const char* path)
{
  struct stat st;
  if (lstat(path, &st) < 0) return false;
  return (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode));
}

bool fs_is_directory_empty(const char* path)
{
  DIR* dirp = opendir(path);
  if (!dirp) return true;

  struct dirent* ent;
  while ((ent = readdir(dirp)) != NULL) {
    if (ent->d_name[0] == '.') {
      if (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))
        continue;
    }
    closedir(dirp);
    return false;
  }
  closedir(dirp);
  return true;
}

bool fs_delete_file(const char* path, Error* error)
{
  struct stat sd;
  if (lstat(path, &sd) != 0 || (S_ISDIR(sd.st_mode) && !S_ISLNK(sd.st_mode))) {
    Error_set_string(error, "File does not exist.");
    return false;
  }
  if (unlink(path) != 0) {
    Error_set_errno_prefix(error, "unlink() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_rename_path(const char* old_path, const char* new_path, Error* error)
{
  if (rename(old_path, new_path) != 0) {
    Error_set_errno_prefix(error, "rename() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_delete_directory(const char* path, Error* error)
{
  struct stat sd;
  if (stat(path, &sd) != 0 || !S_ISDIR(sd.st_mode))
    return false;
  /* Symlinked dirs go through unlink, real ones through rmdir. */
  if (S_ISLNK(sd.st_mode)) {
    if (unlink(path) != 0) {
      Error_set_errno_prefix(error, "unlink() failed: ", errno);
      return false;
    }
  } else {
    if (rmdir(path) != 0) {
      Error_set_errno_prefix(error, "rmdir() failed: ", errno);
      return false;
    }
  }
  return true;
}

bool fs_recursive_delete_directory(const char* path, Error* error)
{
  fs_find_data_t* results = NULL;
  size_t count = 0;
  if (fs_find_files(path, "*", FS_FIND_FILES | FS_FIND_FOLDERS | FS_FIND_HIDDEN_FILES, &results, &count)) {
    for (size_t i = 0; i < count; i++) {
      const fs_find_data_t* fd = &results[i];
      if ((fd->attributes & (FS_FILE_ATTRIBUTE_DIRECTORY | FS_FILE_ATTRIBUTE_LINK)) == FS_FILE_ATTRIBUTE_DIRECTORY) {
        if (!fs_recursive_delete_directory(fd->file_name, error)) {
          fs_free_find_data_array(results, count);
          return false;
        }
      } else {
        if (!fs_delete_file(fd->file_name, error)) {
          Error_add_prefix_fmt(error, "Failed to delete %s: ", fd->file_name);
          fs_free_find_data_array(results, count);
          return false;
        }
      }
    }
    fs_free_find_data_array(results, count);
  }

  return fs_delete_directory(path, error);
}

FILE* fs_open_file(const char* path, const char* mode, Error* error)
{
  FILE* fp = fopen(path, mode);
  if (!fp)
    Error_set_errno(error, errno);
  return fp;
}

FILE* fs_open_shared_file(const char* path, const char* mode, fs_file_share_mode_t share_mode, Error* error)
{
  /* share_mode is a no-op on Linux. */
  (void)share_mode;
  return fs_open_file(path, mode, error);
}

FILE* fs_open_temporary_file(const char* base_path_data, u32 base_path_len, char** out_path, Error* error)
{
  if (base_path_len == 0) {
    Error_set_string(error, "Base path is empty.");
    return NULL;
  }

  /* Append ".XXXXXX" template for mkstemp. */
  const size_t name_buf_size = (size_t)base_path_len + 8;
  char* name_buf = (char*)malloc(name_buf_size);
  if (!name_buf) {
    Error_set_string(error, "Out of memory.");
    return NULL;
  }
  memcpy(name_buf, base_path_data, base_path_len);
  string_util_strlcpy_cstr(name_buf + base_path_len, ".XXXXXX", name_buf_size - base_path_len);

  const int fd = mkstemp(name_buf);
  if (fd < 0) {
    Error_set_errno_prefix(error, "mkstemp() failed: ", errno);
    free(name_buf);
    return NULL;
  }

  FILE* fp = fdopen(fd, "w+b");
  if (!fp) {
    Error_set_errno_prefix(error, "fdopen() failed: ", errno);
    close(fd);
    free(name_buf);
    return NULL;
  }

  if (out_path)
    *out_path = name_buf;
  else
    free(name_buf);
  return fp;
}

FILE* fs_open_existing_or_create_file(const char* path, s32 retry_ms, Error* error)
{
  /* retry_ms is Win32-only sharing-violation backoff. */
  (void)retry_ms;

  FILE* fp = fopen(path, "r+b");
  if (fp) return fp;

  if (errno != ENOENT) {
    Error_set_errno(error, errno);
    return NULL;
  }

  /* Race-safe create: "x" fails when the file already exists. */
  fp = fopen(path, "w+bx");
  if (fp) return fp;

  if (errno == EEXIST)
    fp = fopen(path, "r+b");
  if (!fp) {
    Error_set_errno(error, errno);
    return NULL;
  }
  return fp;
}

int fs_open_fd_file(const char* path, int flags, int mode, Error* error)
{
  const int fd = open(path, flags, mode);
  if (fd < 0)
    Error_set_errno(error, errno);
  return fd;
}

int fs_fseek64(FILE* fp, s64 offset, int whence)
{
  /* off_t is 64-bit on Linux under _GNU_SOURCE (which we always define), so
   * fseeko handles the full s64 range and no truncation guard is needed. */
  _Static_assert(sizeof(off_t) == sizeof(s64), "off_t must be 64-bit");
  return fseeko(fp, (off_t)offset, whence);
}

bool fs_fseek64_e(FILE* fp, s64 offset, int whence, Error* error)
{
  if (fs_fseek64(fp, offset, whence) == 0)
    return true;
  Error_set_errno(error, errno);
  return false;
}

s64 fs_ftell64(FILE* fp)
{
  return (s64)ftello(fp);
}

s64 fs_fsize64(FILE* fp, Error* error)
{
  const s64 pos = fs_ftell64(fp);
  if (pos < 0) {
    Error_set_errno_prefix(error, "ftello() failed: ", errno);
    return -1;
  }
  if (fs_fseek64(fp, 0, SEEK_END) != 0) {
    Error_set_errno_prefix(error, "fseeko() to end failed: ", errno);
    return -1;
  }
  const s64 size = fs_ftell64(fp);
  if (size < 0) {
    Error_set_errno_prefix(error, "ftello() failed: ", errno);
    return -1;
  }
  if (fs_fseek64(fp, pos, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "fseeko() to original position failed: ", errno);
    return -1;
  }
  return size;
}

bool fs_ftruncate64(FILE* fp, s64 size, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0) {
    Error_set_errno_prefix(error, "fileno() failed: ", errno);
    return false;
  }
  if (ftruncate(fd, (off_t)size) < 0) {
    Error_set_errno_prefix(error, "ftruncate() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_read_binary_file(FILE* fp, u8** out_data, size_t* out_len, Error* error)
{
  *out_data = NULL;
  *out_len  = 0;

  if (fs_fseek64(fp, 0, SEEK_END) != 0) {
    Error_set_errno_prefix(error, "fseeko() to end failed: ", errno);
    return false;
  }
  const s64 size = fs_ftell64(fp);
  if (size < 0) {
    Error_set_errno_prefix(error, "ftello() for length failed: ", errno);
    return false;
  }
  if (fs_fseek64(fp, 0, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "fseeko() to start failed: ", errno);
    return false;
  }

  if (size == 0) return true;

  u8* buf = (u8*)malloc((size_t)size);
  if (!buf) {
    Error_set_string(error, "Out of memory.");
    return false;
  }
  if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    Error_set_errno_prefix(error, "fread() failed: ", errno);
    free(buf);
    return false;
  }
  *out_data = buf;
  *out_len  = (size_t)size;
  return true;
}

bool fs_read_binary_file_path(const char* path, u8** out_data, size_t* out_len, Error* error)
{
  FILE* fp = fs_open_file(path, "rb", error);
  if (!fp) { *out_data = NULL; *out_len = 0; return false; }
  const bool ok = fs_read_binary_file(fp, out_data, out_len, error);
  fclose(fp);
  return ok;
}

bool fs_read_file_to_string(FILE* fp, char** out_data, size_t* out_len, Error* error)
{
  *out_data = NULL;
  *out_len  = 0;

  if (fs_fseek64(fp, 0, SEEK_END) != 0) {
    Error_set_errno_prefix(error, "fseeko() to end failed: ", errno);
    return false;
  }
  const s64 size = fs_ftell64(fp);
  if (size < 0) {
    Error_set_errno_prefix(error, "ftello() for length failed: ", errno);
    return false;
  }
  if (fs_fseek64(fp, 0, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "fseeko() to start failed: ", errno);
    return false;
  }

  char* buf = (char*)malloc((size_t)size + 1);
  if (!buf) {
    Error_set_string(error, "Out of memory.");
    return false;
  }
  if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    Error_set_errno_prefix(error, "fread() failed: ", errno);
    free(buf);
    return false;
  }
  buf[size] = '\0';

  size_t len = (size_t)size;
  static const u8 UTF8_BOM[] = { 0xEF, 0xBB, 0xBF };
  if (len >= sizeof(UTF8_BOM) && memcmp(buf, UTF8_BOM, sizeof(UTF8_BOM)) == 0) {
    memmove(buf, buf + sizeof(UTF8_BOM), len - sizeof(UTF8_BOM));
    len -= sizeof(UTF8_BOM);
    buf[len] = '\0';
  }

  *out_data = buf;
  *out_len  = len;
  return true;
}

bool fs_read_file_to_string_path(const char* path, char** out_data, size_t* out_len, Error* error)
{
  FILE* fp = fs_open_file(path, "rb", error);
  if (!fp) { *out_data = NULL; *out_len = 0; return false; }
  const bool ok = fs_read_file_to_string(fp, out_data, out_len, error);
  fclose(fp);
  return ok;
}

bool fs_write_binary_file(const char* path, const void* data, size_t data_length, Error* error)
{
  FILE* fp = fs_open_file(path, "wb", error);
  if (!fp) return false;

  if (data_length > 0 && fwrite(data, 1, data_length, fp) != data_length) {
    Error_set_errno_prefix(error, "fwrite() failed: ", errno);
    fclose(fp);
    return false;
  }
  if (fclose(fp) != 0) {
    Error_set_errno_prefix(error, "fclose() failed: ", errno);
    return false;
  }
  return true;
}

bool fs_write_string_to_file_view(const char* path, const char* data, u32 len, Error* error)
{
  return fs_write_binary_file(path, data, (size_t)len, error);
}

bool fs_write_string_to_file_cstr(const char* path, const char* str, Error* error)
{
  return fs_write_binary_file(path, str, str ? strlen(str) : 0, error);
}

bool fs_create_directory(const char* path, bool recursive, Error* error)
{
  const size_t path_length = strlen(path);
  if (path_length == 0)
    return false;

  if (mkdir(path, 0777) == 0)
    return true;

  int last_error = errno;
  if (last_error == EEXIST) {
    struct stat sd;
    if (stat(path, &sd) == 0 && S_ISDIR(sd.st_mode))
      return true;
  }

  if (!recursive) {
    Error_set_errno_prefix(error, "mkdir() failed: ", last_error);
    return false;
  }

  if (last_error != ENOENT) {
    Error_set_errno_prefix(error, "mkdir() failed: ", last_error);
    return false;
  }

  char* tmp = (char*)malloc(path_length + 1);
  if (!tmp) {
    Error_set_string(error, "Out of memory.");
    return false;
  }
  size_t tmp_len = 0;

  for (size_t i = 0; i < path_length; i++) {
    if (i > 0 && path[i] == '/') {
      tmp[tmp_len] = '\0';
      if (mkdir(tmp, 0777) < 0) {
        last_error = errno;
        if (last_error != EEXIST) {
          Error_set_errno_prefix(error, "mkdir() failed: ", last_error);
          free(tmp);
          return false;
        }
      }
    }
    tmp[tmp_len++] = path[i];
  }

  /* Final segment if it doesn't end with '/'. */
  if (path[path_length - 1] != '/') {
    if (mkdir(path, 0777) < 0) {
      last_error = errno;
      if (last_error != EEXIST) {
        Error_set_errno_prefix(error, "mkdir() failed: ", last_error);
        free(tmp);
        return false;
      }
    }
  }

  free(tmp);
  return true;
}

bool fs_ensure_directory_exists(const char* path, bool recursive, Error* error)
{
  if (fs_directory_exists(path))
    return true;
  return fs_create_directory(path, recursive, error);
}

bool fs_copy_file_path(const char* source, const char* destination, bool replace, Error* error)
{
  if (!replace && fs_file_exists(destination)) {
    Error_set_string(error, "File already exists.");
    return false;
  }

  FILE* in_fp = fs_open_file(source, "rb", error);
  if (!in_fp) return false;

  FILE* out_fp = fs_open_file(destination, "wb", error);
  if (!out_fp) { fclose(in_fp); return false; }

  u8 buf[4096];
  while (!feof(in_fp)) {
    const size_t bytes_in = fread(buf, 1, sizeof(buf), in_fp);
    if ((bytes_in == 0 && !feof(in_fp)) ||
        (bytes_in > 0 && fwrite(buf, 1, bytes_in, out_fp) != bytes_in)) {
      Error_set_errno_prefix(error, "fread() or fwrite() failed: ", errno);
      fclose(out_fp);
      fclose(in_fp);
      fs_delete_file(destination, NULL);
      return false;
    }
  }

  if (fflush(out_fp) != 0) {
    Error_set_errno_prefix(error, "fflush() failed: ", errno);
    fclose(out_fp);
    fclose(in_fp);
    fs_delete_file(destination, NULL);
    return false;
  }

  fclose(out_fp);
  fclose(in_fp);
  return true;
}

char* fs_get_program_path(Error* error)
{
  static const char* exe_path = "/proc/self/exe";

  size_t cur_size = PATH_MAX;
  char* buffer = (char*)malloc(cur_size);
  if (!buffer) { Error_set_string(error, "Out of memory."); return NULL; }

  for (;;) {
    const ssize_t len = readlink(exe_path, buffer, cur_size);
    if (len < 0) {
      Error_set_errno_prefix(error, "readlink() failed: ", errno);
      free(buffer);
      return NULL;
    } else if ((size_t)len < cur_size) {
      buffer[len] = '\0';
      return buffer;
    }
    cur_size *= 2;
    char* nb = (char*)realloc(buffer, cur_size);
    if (!nb) {
      Error_set_string(error, "Out of memory.");
      free(buffer);
      return NULL;
    }
    buffer = nb;
  }
}

char* fs_get_working_directory(void)
{
  size_t cap = PATH_MAX;
  char* buf = (char*)malloc(cap);
  if (!buf) return NULL;
  while (!getcwd(buf, cap)) {
    if (errno != ERANGE) { free(buf); return NULL; }
    cap *= 2;
    char* nb = (char*)realloc(buf, cap);
    if (!nb) { free(buf); return NULL; }
    buf = nb;
  }
  return buf;
}

bool fs_set_working_directory(const char* path)
{
  return chdir(path) == 0;
}

bool fs_set_path_compression(const char* path, bool enable)
{
  /* NTFS-only flag; nothing to do on Linux. */
  (void)path; (void)enable;
  return false;
}

bool fs_set_path_executable(const char* path, bool executable, Error* error)
{
  struct stat st;
  if (stat(path, &st) != 0) {
    Error_set_errno_prefix(error, "stat() failed: ", errno);
    return false;
  }

  mode_t new_mode;
  if (executable)
    new_mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
  else
    new_mode = st.st_mode & ~(mode_t)(S_IXUSR | S_IXGRP | S_IXOTH);

  if (st.st_mode == new_mode)
    return true;

  if (chmod(path, new_mode) != 0) {
    Error_set_errno_prefix(error, "chmod() failed: ", errno);
    return false;
  }
  return true;
}
