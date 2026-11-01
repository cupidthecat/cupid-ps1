/* cupid-ps1 shared stream operations and filesystem result ownership. */
#include "file_system.h"
#include "error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void fs_free_root_directory_list(char** paths, size_t count) {
  if (!paths) return;
  for (size_t i = 0; i < count; i++)
    free(paths[i]);
  free(paths);
}

void fs_free_find_data_array(fs_find_data_t* arr, size_t count) {
  if (!arr) return;
  for (size_t i = 0; i < count; i++)
    free(arr[i].file_name);
  free(arr);
}

bool fs_fseek64_e(FILE* fp, s64 offset, int whence, Error* error) {
  if (fs_fseek64(fp, offset, whence) == 0) return true;
  Error_set_errno(error, errno);
  return false;
}

s64 fs_fsize64(FILE* fp, Error* error) {
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

bool fs_read_binary_file(FILE* fp, u8** out_data, size_t* out_len, Error* error) {
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

bool fs_read_binary_file_path(const char* path, u8** out_data, size_t* out_len, Error* error) {
  FILE* fp = fs_open_file(path, "rb", error);
  if (!fp) {
    *out_data = NULL;
    *out_len  = 0;
    return false;
  }
  const bool ok = fs_read_binary_file(fp, out_data, out_len, error);
  fclose(fp);
  return ok;
}

bool fs_read_file_to_string(FILE* fp, char** out_data, size_t* out_len, Error* error) {
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

  size_t          len        = (size_t)size;
  static const u8 UTF8_BOM[] = {0xEF, 0xBB, 0xBF};
  if (len >= sizeof(UTF8_BOM) && memcmp(buf, UTF8_BOM, sizeof(UTF8_BOM)) == 0) {
    memmove(buf, buf + sizeof(UTF8_BOM), len - sizeof(UTF8_BOM));
    len -= sizeof(UTF8_BOM);
    buf[len] = '\0';
  }

  *out_data = buf;
  *out_len  = len;
  return true;
}

bool fs_read_file_to_string_path(const char* path, char** out_data, size_t* out_len, Error* error) {
  FILE* fp = fs_open_file(path, "rb", error);
  if (!fp) {
    *out_data = NULL;
    *out_len  = 0;
    return false;
  }
  const bool ok = fs_read_file_to_string(fp, out_data, out_len, error);
  fclose(fp);
  return ok;
}

bool fs_write_binary_file(const char* path, const void* data, size_t data_length, Error* error) {
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

bool fs_write_string_to_file_view(const char* path, const char* data, u32 len, Error* error) {
  return fs_write_binary_file(path, data, (size_t)len, error);
}

bool fs_write_binary_file_atomic(const char* path, const void* data, size_t data_length, Error* error) {
  if (!path || (data_length && !data) || strlen(path) > UINT32_MAX) {
    Error_set_errno(error, EINVAL);
    return false;
  }
  char* temporary = NULL;
  FILE* fp        = fs_open_temporary_file(path, (u32)strlen(path), &temporary, error);
  if (!fp) return false;
  bool ok = !data_length || fwrite(data, 1, data_length, fp) == data_length;
  if (!ok) Error_set_errno_prefix(error, "Writing temporary file failed: ", errno ? errno : EIO);
  if (ok) ok = fs_flush_file(fp, error);
  if (fclose(fp) != 0 && ok) {
    Error_set_errno_prefix(error, "Closing temporary file failed: ", errno);
    ok = false;
  }
  if (ok) ok = fs_rename_path(temporary, path, error);
  if (!ok) fs_delete_file(temporary, NULL);
  free(temporary);
  return ok;
}

bool fs_write_string_to_file_cstr(const char* path, const char* str, Error* error) {
  return fs_write_binary_file(path, str, str ? strlen(str) : 0, error);
}

bool fs_ensure_directory_exists(const char* path, bool recursive, Error* error) {
  if (fs_directory_exists(path)) return true;
  return fs_create_directory(path, recursive, error);
}
