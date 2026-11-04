/* cupid-ps1 native Unicode, large-file and persistence filesystem checks. */
#include "common/file_system.h"
#include "common/error.h"
#include "common/windows/windows_util.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <winioctl.h>

#define CHECK(condition)                                                                                     \
  do {                                                                                                       \
    if (!(condition)) {                                                                                      \
      fprintf(stderr, "%s:%u: %s (Windows error %lu, errno %d)\n", __FILE__, (unsigned)__LINE__, #condition, \
              (unsigned long)GetLastError(), errno);                                                         \
      exit(1);                                                                                               \
    }                                                                                                        \
  } while (0)

static char* child(const char* base, const char* name) {
  const size_t length = strlen(base) + strlen(name) + 2;
  char*        result = malloc(length);
  CHECK(result);
  snprintf(result, length, "%s/%s", base, name);
  return result;
}

static void check_content(const char* path, const void* data, size_t size) {
  u8*    actual = NULL;
  size_t length = 0;
  CHECK(fs_read_binary_file_path(path, &actual, &length, NULL));
  CHECK(length == size && (size == 0 || memcmp(actual, data, size) == 0));
  free(actual);
}

static void make_junction(const char* link, const char* target) {
  CHECK(fs_create_directory(link, false, NULL));
  wchar_t* wide_link   = windows_path_to_wide(link, NULL);
  wchar_t* wide_target = windows_path_to_wide(target, NULL);
  CHECK(wide_link && wide_target && !wcsncmp(wide_target, L"\\\\?\\", 4));
  struct {
    DWORD   tag;
    USHORT  length, reserved, sub_offset, sub_length, print_offset, print_length;
    wchar_t paths[2048];
  } reparse   = {0};
  reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
  wcscpy(reparse.paths, L"\\??\\");
  wcscat(reparse.paths, wide_target + 4);
  const size_t sub_length = wcslen(reparse.paths), print_length = wcslen(wide_target + 4);
  CHECK(sub_length + print_length + 2 < 2048);
  wcscpy(reparse.paths + sub_length + 1, wide_target + 4);
  reparse.sub_length   = (USHORT)(sub_length * sizeof(wchar_t));
  reparse.print_offset = (USHORT)((sub_length + 1) * sizeof(wchar_t));
  reparse.print_length = (USHORT)(print_length * sizeof(wchar_t));
  reparse.length       = (USHORT)(8 + (sub_length + print_length + 2) * sizeof(wchar_t));
  HANDLE handle = CreateFileW(wide_link, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  CHECK(handle != INVALID_HANDLE_VALUE);
  DWORD returned;
  CHECK(DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &reparse, reparse.length + 8, NULL, 0, &returned, NULL));
  CloseHandle(handle);
  free(wide_link);
  free(wide_target);
}

static void check_streams(const char* root) {
  const u8 bytes[] = {0, 0x1A, '\r', '\n', 0xFF, 0x42};
  char*    path    = child(root, "\xCE\xBB-\xE7\x8C\xAB.bin");
  CHECK(fs_write_binary_file(path, bytes, sizeof(bytes), NULL));
  check_content(path, bytes, sizeof(bytes));
  CHECK(fs_file_exists(path) && fs_get_path_file_size(path) == sizeof(bytes));
  FILE* input = fs_open_shared_file(path, "rb", FS_FILE_SHARE_DENY_WRITE, NULL);
  CHECK(input);
  CHECK(!fs_open_file(path, "wb", NULL));
  FILE* second = fs_open_file(path, "rb", NULL);
  CHECK(second);
  fclose(second);
  fclose(input);
  check_content(path, bytes, sizeof(bytes));
  CHECK(!fs_open_file(path, "wbx", NULL));
  check_content(path, bytes, sizeof(bytes));
  FILE* update = fs_open_existing_or_create_file(path, 0, NULL);
  CHECK(update && fs_fsize64(update, NULL) == sizeof(bytes));
  fclose(update);
  CHECK(!fs_copy_file_path(path, path, true, NULL));
  check_content(path, bytes, sizeof(bytes));
  char* destination = child(root, "card.mcd");
  CHECK(fs_write_binary_file(destination, "old", 3, NULL));
  FILE* old = fs_open_file(destination, "rb", NULL);
  CHECK(old && fs_rename_path(path, destination, NULL));
  CHECK(!fs_file_exists(path));
  check_content(destination, bytes, sizeof(bytes));
  char previous[3];
  CHECK(fread(previous, 1, 3, old) == 3 && !memcmp(previous, "old", 3));
  fclose(old);
  CHECK(!fs_copy_file_path(destination, destination, false, NULL));
  char* copied = child(root, "copy.BIN");
  CHECK(fs_copy_file_path(destination, copied, false, NULL));
  check_content(copied, bytes, sizeof(bytes));
  const int fd = fs_open_fd_file(copied, _O_RDONLY | _O_BINARY, 0, NULL);
  CHECK(fd >= 0);
  u8 raw[sizeof(bytes)];
  CHECK(_read(fd, raw, sizeof(raw)) == sizeof(raw) && !memcmp(raw, bytes, sizeof(bytes)));
  _close(fd);
  const u8 bom[] = {0xEF, 0xBB, 0xBF, 'x', '\n'};
  CHECK(fs_write_binary_file(path, bom, sizeof(bom), NULL));
  char*  text = NULL;
  size_t length;
  CHECK(fs_read_file_to_string_path(path, &text, &length, NULL));
  CHECK(length == 2 && !memcmp(text, "x\n\0", 3));
  free(text);
  CHECK(fs_write_binary_file(path, NULL, 0, NULL));
  check_content(path, NULL, 0);
  Error error = ERROR_INIT;
  CHECK(!fs_open_file("\xC0", "rb", &error) && Error_is_valid(&error));
  Error_destroy(&error);
  free(path);
  free(destination);
  free(copied);
}

static void check_large_file(const char* root) {
  char* path = child(root, "sparse.bin");
  FILE* fp   = fs_open_file(path, "w+b", NULL);
  CHECK(fp);
  HANDLE handle = (HANDLE)_get_osfhandle(_fileno(fp));
  DWORD  returned;
  CHECK(DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL));
  const s64 offset = ((s64)1 << 32) + 123;
  CHECK(fs_fseek64_e(fp, offset, SEEK_SET, NULL) && fs_ftell64(fp) == offset);
  CHECK(fputc(0x73, fp) == 0x73 && !fflush(fp));
  CHECK(fs_fsize64(fp, NULL) == offset + 1 && fs_ftell64(fp) == offset + 1);
  fs_stat_data_t   data;
  fs_native_stat_t raw;
  CHECK(fs_stat_path_data(path, &data, NULL) && data.size == offset + 1);
  CHECK(fs_stat_file_data(fp, &data, NULL) && data.size == offset + 1);
  CHECK(fs_stat_path(path, &raw, NULL) && raw.st_size == offset + 1);
  CHECK(fs_stat_file(fp, &raw, NULL) && raw.st_size == offset + 1);
  CHECK(fs_fseek64(fp, offset - 1, SEEK_SET) == 0 && fgetc(fp) == 0 && fgetc(fp) == 0x73);
  CHECK(fs_ftruncate64(fp, 2, NULL) && fs_fsize64(fp, NULL) == 2);
  CHECK(!fs_ftruncate64(fp, -1, NULL));
  fclose(fp);
  CHECK(fs_get_path_file_size(path) == 2);
  free(path);
}

static void check_atomic_replacement(const char* root) {
  char* path = child(root, "atomic-\xCE\xBB.mcd");
  CHECK(fs_write_binary_file_atomic(path, "original", 8, NULL));
  FILE* previous = fs_open_file(path, "rb", NULL);
  CHECK(previous);
  CHECK(fs_write_binary_file_atomic(path, "replacement", 11, NULL));
  char old[8];
  CHECK(fread(old, 1, sizeof(old), previous) == sizeof(old) && !memcmp(old, "original", 8));
  fclose(previous);
  check_content(path, "replacement", 11);
  /* A reader that denies deletion must prevent replacement without damaging
   * the destination or leaving a sibling temporary file. */
  wchar_t* wide = windows_path_to_wide(path, NULL);
  CHECK(wide);
  HANDLE locked = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  CHECK(locked != INVALID_HANDLE_VALUE);
  free(wide);
  Error error = ERROR_INIT;
  CHECK(!fs_write_binary_file_atomic(path, "discard", 7, &error) && Error_is_valid(&error));
  CloseHandle(locked);
  check_content(path, "replacement", 11);
  Error_destroy(&error);
  fs_find_data_t* leftovers = NULL;
  size_t          count     = 0;
  CHECK(!fs_find_files(root, "atomic-*.mcd.*", FS_FIND_FILES | FS_FIND_HIDDEN_FILES, &leftovers, &count) && count == 0);
  fs_free_find_data_array(leftovers, count);
  CHECK(!fs_write_binary_file_atomic(path, NULL, 1, NULL));
  check_content(path, "replacement", 11);
  free(path);
}

static void check_temporary(const char* root) {
  char* base  = child(root, "temporary-\xCE\xBB");
  char *first = NULL, *second = NULL;
  FILE* a = fs_open_temporary_file(base, (u32)strlen(base), &first, NULL);
  FILE* b = fs_open_temporary_file(base, (u32)strlen(base), &second, NULL);
  CHECK(a && b && strcmp(first, second));
  CHECK(fs_file_exists(first) && fs_file_exists(second));
  CHECK(fwrite("abc", 1, 3, a) == 3);
  fclose(a);
  fclose(b);
  check_content(first, "abc", 3);
  CHECK(fs_delete_file(first, NULL) && fs_delete_file(second, NULL));
  free(first);
  free(second);
  a = fs_open_temporary_file(base, (u32)strlen(base), NULL, NULL);
  CHECK(a);
  wchar_t     name[32768];
  const DWORD length = GetFinalPathNameByHandleW((HANDLE)_get_osfhandle(_fileno(a)), name, 32768, FILE_NAME_NORMALIZED);
  CHECK(length && length < 32768);
  char* unnamed = windows_wide_to_utf8(name, NULL);
  CHECK(unnamed);
  fclose(a);
  CHECK(!fs_file_exists(unnamed));
  free(unnamed);
  free(base);
  a = fs_open_anonymous_file(NULL);
  CHECK(a && fwrite("scratch", 1, 7, a) == 7 && fs_fsize64(a, NULL) == 7);
  fclose(a);
  const unsigned char contents[] = {0, 26, 13, 10, 255};
  a                              = fs_open_readonly_buffer(contents, sizeof(contents), NULL);
  CHECK(a && fs_fsize64(a, NULL) == (s64)sizeof(contents));
  unsigned char actual[sizeof(contents)];
  CHECK(fread(actual, 1, sizeof(actual), a) == sizeof(actual) && !memcmp(actual, contents, sizeof(actual)));
  CHECK(fs_fseek64(a, 0, SEEK_SET) == 0 && fputc(1, a) == EOF);
  fclose(a);
  a = fs_open_readonly_buffer(NULL, 0, NULL);
  CHECK(a && fs_fsize64(a, NULL) == 0 && fgetc(a) == EOF);
  fclose(a);
  CHECK(!fs_open_readonly_buffer(NULL, 1, NULL));
}

static void check_directories(const char* root, const char* outside) {
  char* nested = child(root, "\xCE\xBB/sub/deep");
  CHECK(fs_create_directory(nested, true, NULL) && fs_directory_exists(nested));
  CHECK(fs_is_directory_empty(nested) && !fs_is_directory_empty(root));
  char* loop = child(nested, "loop");
  make_junction(loop, root);
  CHECK(!fs_is_real_directory(loop));
  char* external = child(root, "outside-link");
  make_junction(external, outside);
  fs_find_data_t* entries = NULL;
  size_t          count   = 0;
  CHECK(fs_find_files(root, "*.BIN", FS_FIND_FILES | FS_FIND_RECURSIVE | FS_FIND_RELATIVE_PATHS, &entries, &count));
  bool have_copy = false, have_unicode = false;
  for (size_t i = 0; i < count; i++) {
    if (!strcmp(entries[i].file_name, "copy.BIN")) have_copy = true;
    if (!strcmp(entries[i].file_name, "\xCE\xBB-\xE7\x8C\xAB.bin")) have_unicode = true;
  }
  CHECK(have_copy && have_unicode);
  const size_t previous = count;
  CHECK(!fs_find_files(root, "*.absent", FS_FIND_FILES | FS_FIND_KEEP_ARRAY, &entries, &count) && count == previous);
  fs_free_find_data_array(entries, count);
  /* Long path operations use extended paths internally, without changing the
   * UTF-8 path accepted by the public API. */
  char* long_path = strdup(nested);
  CHECK(long_path);
  for (unsigned i = 0; i < 8; i++) {
    char* next = child(long_path, "long-directory-component-0123456789");
    free(long_path);
    long_path = next;
  }
  CHECK(strlen(long_path) > 260 && fs_create_directory(long_path, true, NULL));
  char* file = child(long_path, "long-\xCE\xBB.bin");
  CHECK(fs_write_binary_file(file, "long", 4, NULL));
  check_content(file, "long", 4);
  fs_native_stat_t stat;
  CHECK(fs_stat_path(file, &stat, NULL) && stat.st_size == 4);
  free(file);
  free(long_path);
  free(nested);
  free(loop);
  free(external);
}

int main(void) {
  const char* protected_roots[] = {"C:/", "//not-a-real-server/share/", "\\\\?\\unc\\not-a-real-server\\share\\",
                                   "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\"};
  for (size_t i = 0; i < countof(protected_roots); i++) {
    Error error = ERROR_INIT;
    CHECK(!fs_recursive_delete_directory(protected_roots[i], &error));
    CHECK(strstr(Error_get_description(&error), "filesystem root"));
    Error_destroy(&error);
  }
  char* program = fs_get_program_path(NULL);
  char* cwd     = fs_get_working_directory();
  CHECK(program && cwd);
  char* separator = strrchr(program, '/');
  CHECK(separator);
  *separator = 0;
  CHECK(!strcmp(program, cwd)); /* All generated paths stay beside this test binary. */
  char name[80];
  snprintf(name, sizeof(name), "cupid-fs-test-%lu", (unsigned long)GetCurrentProcessId());
  char* root = child(cwd, name);
  snprintf(name, sizeof(name), "cupid-fs-outside-%lu", (unsigned long)GetCurrentProcessId());
  char* outside = child(cwd, name);
  CHECK(!fs_directory_exists(root) && !fs_directory_exists(outside));
  CHECK(fs_create_directory(root, false, NULL) && fs_create_directory(outside, false, NULL));
  char* sentinel = child(outside, "preserved.txt");
  CHECK(fs_write_string_to_file_cstr(sentinel, "preserve", NULL));
  check_streams(root);
  check_large_file(root);
  check_atomic_replacement(root);
  check_temporary(root);
  check_directories(root, outside);
  CHECK(fs_set_working_directory(root));
  char* changed = fs_get_working_directory();
  CHECK(changed && !strcmp(changed, root));
  free(changed);
  CHECK(fs_set_working_directory(cwd));
  char** drives      = NULL;
  size_t drive_count = 0;
  CHECK(fs_get_root_directory_list(&drives, &drive_count) && drive_count > 0);
  fs_free_root_directory_list(drives, drive_count);
  /* Verify the absolute owned paths before recursive deletion. The outside
   * sentinel must survive removal of the junction in the primary tree. */
  CHECK(!strncmp(root, cwd, strlen(cwd)) && root[strlen(cwd)] == '/');
  CHECK(!strncmp(outside, cwd, strlen(cwd)) && outside[strlen(cwd)] == '/');
  CHECK(fs_recursive_delete_directory(root, NULL));
  CHECK(!fs_directory_exists(root));
  check_content(sentinel, "preserve", 8);
  CHECK(fs_recursive_delete_directory(outside, NULL));
  free(sentinel);
  free(root);
  free(outside);
  free(program);
  free(cwd);
  puts("Windows filesystem checks passed: Unicode/long paths, sparse 64-bit offsets, sharing, replacement and "
       "junctions");
  return 0;
}
