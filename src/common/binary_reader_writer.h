/*
 * BinarySpan{Reader,Writer} → fixed-size buffer cursors over caller memory.
 * BinaryFile{Reader,Writer} → cursors over a FILE*.
 *
 * Templated typed read/write methods are expanded by hand into one function
 * per concrete width (s8/u8/.../u64/float, plus bool with byte coercion).
 */

#ifndef CUPID_COMMON_BINARY_READER_WRITER_H
#define CUPID_COMMON_BINARY_READER_WRITER_H

#include "types.h"

#include <stdio.h>
#include <string.h>

typedef struct Error            Error;
typedef struct small_string     small_string_t;

typedef struct {
  const u8* buf;
  size_t    len;
  size_t    pos;
} binary_span_reader_t;

ALWAYS_INLINE void binary_span_reader_init(binary_span_reader_t* r, const u8* buf, size_t len)
{
  r->buf = buf;
  r->len = len;
  r->pos = 0;
}

ALWAYS_INLINE bool   binary_span_reader_is_valid(const binary_span_reader_t* r) { return r->buf && r->len > 0; }
ALWAYS_INLINE bool   binary_span_reader_check_remaining(const binary_span_reader_t* r, size_t n) { return r->pos + n <= r->len; }
ALWAYS_INLINE size_t binary_span_reader_remaining(const binary_span_reader_t* r) { return r->len - r->pos; }
ALWAYS_INLINE size_t binary_span_reader_consumed (const binary_span_reader_t* r) { return r->pos; }

void binary_span_reader_increment(binary_span_reader_t* r, size_t n);
void binary_span_reader_get_remaining(const binary_span_reader_t* r, const u8** out_data, size_t* out_len);

ALWAYS_INLINE bool binary_span_reader_read(binary_span_reader_t* r, void* dst, size_t n)
{
  if (LIKELY(r->pos + n <= r->len)) {
    memcpy(dst, &r->buf[r->pos], n);
    r->pos += n;
    return true;
  }
  return false;
}
ALWAYS_INLINE bool binary_span_reader_peek(const binary_span_reader_t* r, void* dst, size_t n)
{
  if (LIKELY(r->pos + n <= r->len)) {
    memcpy(dst, &r->buf[r->pos], n);
    return true;
  }
  return false;
}

#define BSR_RW_PRIM(suffix, type)                                                                                      \
  ALWAYS_INLINE bool binary_span_reader_read_##suffix (binary_span_reader_t* r, type* d) { return binary_span_reader_read(r, d, sizeof(*d)); } \
  ALWAYS_INLINE bool binary_span_reader_peek_##suffix (const binary_span_reader_t* r, type* d) { return binary_span_reader_peek(r, d, sizeof(*d)); }
BSR_RW_PRIM(s8,  s8)  BSR_RW_PRIM(u8,  u8)
BSR_RW_PRIM(s16, s16) BSR_RW_PRIM(u16, u16)
BSR_RW_PRIM(s32, s32) BSR_RW_PRIM(u32, u32)
BSR_RW_PRIM(s64, s64) BSR_RW_PRIM(u64, u64)
BSR_RW_PRIM(float, float)
#undef BSR_RW_PRIM

ALWAYS_INLINE bool binary_span_reader_read_bool(binary_span_reader_t* r, bool* d)
{
  u8 v;
  if (UNLIKELY(!binary_span_reader_read_u8(r, &v))) return false;
  *d = (v != 0);
  return true;
}

/* C-string: NUL-terminated.  out_data points into r->buf (zero-copy view). */
bool binary_span_reader_peek_cstring(const binary_span_reader_t* r, const char** out_data, u32* out_len);
bool binary_span_reader_read_cstring(binary_span_reader_t* r, const char** out_data, u32* out_len);
/* Convenience: copy into a small_string. */
bool binary_span_reader_peek_cstring_to_ss(const binary_span_reader_t* r, small_string_t* dst);
bool binary_span_reader_read_cstring_to_ss(binary_span_reader_t* r, small_string_t* dst);

/* Size-prefixed string: u32 length followed by raw bytes (no NUL). */
bool binary_span_reader_peek_size_prefixed(const binary_span_reader_t* r, const char** out_data, u32* out_len);
bool binary_span_reader_read_size_prefixed(binary_span_reader_t* r, const char** out_data, u32* out_len);
bool binary_span_reader_peek_size_prefixed_to_ss(const binary_span_reader_t* r, small_string_t* dst);
bool binary_span_reader_read_size_prefixed_to_ss(binary_span_reader_t* r, small_string_t* dst);

typedef struct {
  u8*    buf;
  size_t len;
  size_t pos;
} binary_span_writer_t;

ALWAYS_INLINE void binary_span_writer_init(binary_span_writer_t* w, u8* buf, size_t len)
{
  w->buf = buf;
  w->len = len;
  w->pos = 0;
}

ALWAYS_INLINE bool   binary_span_writer_is_valid(const binary_span_writer_t* w) { return w->buf && w->len > 0; }
ALWAYS_INLINE size_t binary_span_writer_remaining(const binary_span_writer_t* w) { return w->len - w->pos; }
ALWAYS_INLINE size_t binary_span_writer_written  (const binary_span_writer_t* w) { return w->pos; }
void               binary_span_writer_increment(binary_span_writer_t* w, size_t n);
void               binary_span_writer_get_remaining(binary_span_writer_t* w, u8** out_data, size_t* out_len);

ALWAYS_INLINE bool binary_span_writer_write(binary_span_writer_t* w, const void* src, size_t n)
{
  if (LIKELY(w->pos + n <= w->len)) {
    memcpy(&w->buf[w->pos], src, n);
    w->pos += n;
    return true;
  }
  return false;
}

#define BSW_W_PRIM(suffix, type)                                                                                       \
  ALWAYS_INLINE bool binary_span_writer_write_##suffix(binary_span_writer_t* w, type v) { return binary_span_writer_write(w, &v, sizeof(v)); }
BSW_W_PRIM(s8, s8)  BSW_W_PRIM(u8, u8)
BSW_W_PRIM(s16, s16) BSW_W_PRIM(u16, u16)
BSW_W_PRIM(s32, s32) BSW_W_PRIM(u32, u32)
BSW_W_PRIM(s64, s64) BSW_W_PRIM(u64, u64)
BSW_W_PRIM(float, float)
#undef BSW_W_PRIM

ALWAYS_INLINE bool binary_span_writer_write_bool(binary_span_writer_t* w, bool v)
{
  u8 b = (u8)v;
  return binary_span_writer_write_u8(w, b);
}

bool binary_span_writer_write_cstring        (binary_span_writer_t* w, const char* data, u32 len);
bool binary_span_writer_write_size_prefixed  (binary_span_writer_t* w, const char* data, u32 len);

typedef struct {
  FILE* fp;
  s64   size;
  bool  good;
} binary_file_reader_t;

void binary_file_reader_init(binary_file_reader_t* r, FILE* fp);
ALWAYS_INLINE bool binary_file_reader_is_open(const binary_file_reader_t* r) { return r->fp != NULL; }
ALWAYS_INLINE bool binary_file_reader_is_good(const binary_file_reader_t* r) { return r->good; }
bool binary_file_reader_is_at_end(const binary_file_reader_t* r);

ALWAYS_INLINE bool binary_file_reader_read(binary_file_reader_t* r, void* dst, size_t n)
{
  if (LIKELY(r->good && fread(dst, n, 1, r->fp) == 1)) return true;
  r->good = false;
  return false;
}

#define BFR_R_PRIM(suffix, type)                                                                                       \
  ALWAYS_INLINE bool binary_file_reader_read_##suffix(binary_file_reader_t* r, type* d) { return binary_file_reader_read(r, d, sizeof(*d)); }
BFR_R_PRIM(s8, s8)  BFR_R_PRIM(u8, u8)
BFR_R_PRIM(s16, s16) BFR_R_PRIM(u16, u16)
BFR_R_PRIM(s32, s32) BFR_R_PRIM(u32, u32)
BFR_R_PRIM(s64, s64) BFR_R_PRIM(u64, u64)
BFR_R_PRIM(float, float)
#undef BFR_R_PRIM

ALWAYS_INLINE bool binary_file_reader_read_bool(binary_file_reader_t* r, bool* d)
{
  u8 v;
  if (!binary_file_reader_read_u8(r, &v)) return false;
  *d = (v != 0);
  return true;
}

bool binary_file_reader_read_cstring        (binary_file_reader_t* r, small_string_t* dst);
bool binary_file_reader_read_size_prefixed  (binary_file_reader_t* r, small_string_t* dst);

typedef struct {
  FILE* fp;
  bool  good;
} binary_file_writer_t;

void binary_file_writer_init(binary_file_writer_t* w, FILE* fp);
ALWAYS_INLINE bool binary_file_writer_is_open(const binary_file_writer_t* w) { return w->fp != NULL; }
ALWAYS_INLINE bool binary_file_writer_is_good(const binary_file_writer_t* w) { return w->good; }

ALWAYS_INLINE bool binary_file_writer_write(binary_file_writer_t* w, const void* src, size_t n)
{
  if (LIKELY(w->good && fwrite(src, n, 1, w->fp) == 1)) return true;
  w->good = false;
  return false;
}

#define BFW_W_PRIM(suffix, type)                                                                                       \
  ALWAYS_INLINE bool binary_file_writer_write_##suffix(binary_file_writer_t* w, type v) { return binary_file_writer_write(w, &v, sizeof(v)); }
BFW_W_PRIM(s8, s8)  BFW_W_PRIM(u8, u8)
BFW_W_PRIM(s16, s16) BFW_W_PRIM(u16, u16)
BFW_W_PRIM(s32, s32) BFW_W_PRIM(u32, u32)
BFW_W_PRIM(s64, s64) BFW_W_PRIM(u64, u64)
BFW_W_PRIM(float, float)
#undef BFW_W_PRIM

ALWAYS_INLINE bool binary_file_writer_write_bool(binary_file_writer_t* w, bool v)
{
  u8 b = (u8)v;
  return binary_file_writer_write_u8(w, b);
}

bool binary_file_writer_write_cstring       (binary_file_writer_t* w, const char* data, u32 len);
bool binary_file_writer_write_size_prefixed (binary_file_writer_t* w, const char* data, u32 len);
bool binary_file_writer_flush               (binary_file_writer_t* w, Error* err);

#endif /* CUPID_COMMON_BINARY_READER_WRITER_H */
