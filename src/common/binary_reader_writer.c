#include "binary_reader_writer.h"

#include "assert.h"
#include "error.h"
#include "file_system.h"
#include "small_string.h"

#include <errno.h>
#include <limits.h>

void binary_span_reader_increment(binary_span_reader_t* r, size_t n)
{
  DebugAssert(n < binary_span_reader_remaining(r));
  r->pos += n;
}

void binary_span_reader_get_remaining(const binary_span_reader_t* r, const u8** out_data, size_t* out_len)
{
  *out_data = r->buf + r->pos;
  *out_len  = r->len - r->pos;
}

bool binary_span_reader_peek_cstring(const binary_span_reader_t* r, const char** out_data, u32* out_len)
{
  size_t pos = r->pos, sz = 0;
  while (pos < r->len) {
    if (r->buf[pos] == 0) {
      *out_data = (const char*)&r->buf[r->pos];
      *out_len  = (u32)sz;
      return true;
    }
    pos++; sz++;
  }
  return false;
}

bool binary_span_reader_read_cstring(binary_span_reader_t* r, const char** out_data, u32* out_len)
{
  if (!binary_span_reader_peek_cstring(r, out_data, out_len)) return false;
  r->pos += *out_len + 1;
  return true;
}

bool binary_span_reader_peek_cstring_to_ss(const binary_span_reader_t* r, small_string_t* dst)
{
  const char* data; u32 len;
  if (!binary_span_reader_peek_cstring(r, &data, &len)) return false;
  small_string_assign_view(dst, data, len);
  return true;
}

bool binary_span_reader_read_cstring_to_ss(binary_span_reader_t* r, small_string_t* dst)
{
  const char* data; u32 len;
  if (!binary_span_reader_read_cstring(r, &data, &len)) return false;
  small_string_assign_view(dst, data, len);
  return true;
}

bool binary_span_reader_peek_size_prefixed(const binary_span_reader_t* r, const char** out_data, u32* out_len)
{
  u32 length;
  if (!binary_span_reader_peek_u32(r, &length)) return false;
  if (r->pos + sizeof(length) + length > r->len) return false;
  *out_data = (const char*)&r->buf[r->pos + sizeof(length)];
  *out_len  = length;
  return true;
}

bool binary_span_reader_read_size_prefixed(binary_span_reader_t* r, const char** out_data, u32* out_len)
{
  if (!binary_span_reader_peek_size_prefixed(r, out_data, out_len)) return false;
  r->pos += sizeof(u32) + *out_len;
  return true;
}

bool binary_span_reader_peek_size_prefixed_to_ss(const binary_span_reader_t* r, small_string_t* dst)
{
  const char* data; u32 len;
  if (!binary_span_reader_peek_size_prefixed(r, &data, &len)) return false;
  small_string_assign_view(dst, data, len);
  return true;
}

bool binary_span_reader_read_size_prefixed_to_ss(binary_span_reader_t* r, small_string_t* dst)
{
  const char* data; u32 len;
  if (!binary_span_reader_read_size_prefixed(r, &data, &len)) return false;
  small_string_assign_view(dst, data, len);
  return true;
}

void binary_span_writer_increment(binary_span_writer_t* w, size_t n)
{
  DebugAssert(n < binary_span_writer_remaining(w));
  w->pos += n;
}

void binary_span_writer_get_remaining(binary_span_writer_t* w, u8** out_data, size_t* out_len)
{
  *out_data = w->buf + w->pos;
  *out_len  = w->len - w->pos;
}

bool binary_span_writer_write_cstring(binary_span_writer_t* w, const char* data, u32 len)
{
  if (w->pos + len + 1 > w->len) return false;
  if (len) memcpy(&w->buf[w->pos], data, len);
  w->buf[w->pos + len] = 0;
  w->pos += len + 1;
  return true;
}

bool binary_span_writer_write_size_prefixed(binary_span_writer_t* w, const char* data, u32 len)
{
  if (w->pos + sizeof(u32) + len > w->len) return false;
  memcpy(&w->buf[w->pos], &len, sizeof(len));
  w->pos += sizeof(len);
  if (len) {
    memcpy(&w->buf[w->pos], data, len);
    w->pos += len;
  }
  return true;
}

void binary_file_reader_init(binary_file_reader_t* r, FILE* fp)
{
  r->fp   = fp;
  r->size = fp ? fs_fsize64(fp, NULL) : 0;
  r->good = (fp != NULL);
}

bool binary_file_reader_is_at_end(const binary_file_reader_t* r)
{
  return !r->fp || fs_ftell64(r->fp) == r->size;
}

bool binary_file_reader_read_cstring(binary_file_reader_t* r, small_string_t* dst)
{
  small_string_clear(dst);
  while (r->good) {
    u8 v;
    if (fread(&v, sizeof(v), 1, r->fp) != 1) {
      r->good = false;
      return false;
    }
    if (v == 0) return true;
    small_string_append_char(dst, (char)v);
  }
  return false;
}

bool binary_file_reader_read_size_prefixed(binary_file_reader_t* r, small_string_t* dst)
{
  u32 length;
  if (!binary_file_reader_read_u32(r, &length)) return false;
  small_string_resize(dst, length, 0, false);
  return length == 0 || binary_file_reader_read(r, small_string_data(dst), length);
}

void binary_file_writer_init(binary_file_writer_t* w, FILE* fp)
{
  w->fp   = fp;
  w->good = (fp != NULL);
}

bool binary_file_writer_write_cstring(binary_file_writer_t* w, const char* data, u32 len)
{
  if (len && w->good && fwrite(data, len, 1, w->fp) != 1) {
    w->good = false;
    return false;
  }
  const u8 nul = 0;
  if (w->good && fwrite(&nul, 1, 1, w->fp) != 1) {
    w->good = false;
    return false;
  }
  return w->good;
}

bool binary_file_writer_write_size_prefixed(binary_file_writer_t* w, const char* data, u32 len)
{
  if (!w->good) return false;
  if (fwrite(&len, sizeof(len), 1, w->fp) != 1) {
    w->good = false;
    return false;
  }
  if (len && fwrite(data, len, 1, w->fp) != 1) {
    w->good = false;
    return false;
  }
  return true;
}

bool binary_file_writer_flush(binary_file_writer_t* w, Error* err)
{
  if (!w->good) {
    Error_set_string(err, "Write error previously occurred.");
    return false;
  }
  if (fflush(w->fp) != 0) {
    w->good = false;
    Error_set_errno_prefix(err, "fflush() failed: ", errno);
    return false;
  }
  return true;
}
