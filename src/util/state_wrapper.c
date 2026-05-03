#include "state_wrapper.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/small_string.h"

#include <string.h>

void state_wrapper_init_read(state_wrapper_t* sw, const u8* data, size_t len, u32 version)
{
  binary_span_reader_init(&sw->reader, data, len);
  /* Writer half is unused but zero it so accidental writes assert cleanly. */
  binary_span_writer_init(&sw->writer, NULL, 0);
  sw->mode    = STATE_WRAPPER_READ;
  sw->version = version;
  sw->error   = false;
}

void state_wrapper_init_write(state_wrapper_t* sw, u8* data, size_t len, u32 version)
{
  binary_span_reader_init(&sw->reader, NULL, 0);
  binary_span_writer_init(&sw->writer, data, len);
  sw->mode    = STATE_WRAPPER_WRITE;
  sw->version = version;
  sw->error   = false;
}

size_t state_wrapper_get_position(const state_wrapper_t* sw)
{
  return (sw->mode == STATE_WRAPPER_READ) ? sw->reader.pos : sw->writer.pos;
}

void state_wrapper_set_position(state_wrapper_t* sw, size_t pos)
{
  if (sw->mode == STATE_WRAPPER_READ)
    sw->reader.pos = pos;
  else
    sw->writer.pos = pos;
}

size_t state_wrapper_get_size(const state_wrapper_t* sw)
{
  return (sw->mode == STATE_WRAPPER_READ) ? sw->reader.len : sw->writer.len;
}

void state_wrapper_skip_bytes(state_wrapper_t* sw, size_t count)
{
  /* Only meaningful for READ; there is no "skip ahead in the output stream"
   * error flag instead so the caller can react. */
  if (sw->mode != STATE_WRAPPER_READ) {
    sw->error = true;
    return;
  }

  if (sw->error || sw->reader.pos + count > sw->reader.len) {
    sw->error = true;
    return;
  }
  sw->reader.pos += count;
}

/* Sticky-error read: zeroes destination on failure to keep callers branch-free. */
static void sw_read(state_wrapper_t* sw, void* dst, size_t n)
{
  if (sw->error || !binary_span_reader_read(&sw->reader, dst, n)) {
    sw->error = true;
    memset(dst, 0, n);
  }
}

static void sw_write(state_wrapper_t* sw, const void* src, size_t n)
{
  if (sw->error || !binary_span_writer_write(&sw->writer, src, n))
    sw->error = true;
}

#define DEFINE_DO_PRIM(suffix, type)                                                                                   \
  void state_wrapper_do_##suffix(state_wrapper_t* sw, type* v)                                                         \
  {                                                                                                                    \
    if (sw->mode == STATE_WRAPPER_READ)                                                                                \
      sw_read(sw, v, sizeof(*v));                                                                                      \
    else                                                                                                               \
      sw_write(sw, v, sizeof(*v));                                                                                     \
  }

DEFINE_DO_PRIM(u8,     u8)
DEFINE_DO_PRIM(u16,    u16)
DEFINE_DO_PRIM(u32,    u32)
DEFINE_DO_PRIM(u64,    u64)
DEFINE_DO_PRIM(s8,     s8)
DEFINE_DO_PRIM(s16,    s16)
DEFINE_DO_PRIM(s32,    s32)
DEFINE_DO_PRIM(s64,    s64)
DEFINE_DO_PRIM(float,  float)
DEFINE_DO_PRIM(double, double)

#undef DEFINE_DO_PRIM

void state_wrapper_do_bool(state_wrapper_t* sw, bool* v)
{
  /* Wire format: 1 byte (0 or 1).
   * (sizeof(bool) is implementation-defined, the wire format is not). */
  if (sw->mode == STATE_WRAPPER_READ) {
    u8 b = 0;
    sw_read(sw, &b, sizeof(b));
    *v = (b != 0);
  } else {
    u8 b = (u8)(*v ? 1 : 0);
    sw_write(sw, &b, sizeof(b));
  }
}

void state_wrapper_do_array(state_wrapper_t* sw, void* data, size_t element_size, size_t count)
{
  state_wrapper_do_bytes(sw, data, element_size * count);
}

void state_wrapper_do_bytes(state_wrapper_t* sw, void* data, size_t length)
{
  if (sw->mode == STATE_WRAPPER_READ)
    sw_read(sw, data, length);
  else
    sw_write(sw, data, length);
}

void state_wrapper_do_string(state_wrapper_t* sw, small_string_t* str)
{
  /* Length-prefixed: u32 byte count, then raw bytes (no NUL).  Wire format
   * pinned so save states stay binary-compatible. */
  if (sw->mode == STATE_WRAPPER_READ) {
    u32 length = 0;
    state_wrapper_do_u32(sw, &length);
    if (sw->error)
      return;

    if (sw->reader.pos + length > sw->reader.len) {
      sw->error = true;
      return;
    }

    small_string_set_size(str, length, false);
    if (length > 0)
      sw_read(sw, small_string_data(str), length);
    /* Re-derive length in case the payload contained an embedded NUL. */
    small_string_update_size(str);
  } else {
    u32 length = small_string_length(str);
    state_wrapper_do_u32(sw, &length);
    if (length > 0)
      sw_write(sw, small_string_data(str), length);
  }
}

bool state_wrapper_do_marker(state_wrapper_t* sw, const char* marker)
{
  /* DoMarker writes the marker as a length-prefixed string and on READ
   * verifies it matches.  Mismatch is a hard fail but does NOT poison the
   * sticky error flag; callers may still continue past a bad marker if
   * they want best-effort recovery. */
  const u32 marker_len = (u32)strlen(marker);

  if (sw->mode == STATE_WRAPPER_WRITE) {
    u32 len = marker_len;
    state_wrapper_do_u32(sw, &len);
    if (marker_len > 0)
      sw_write(sw, marker, marker_len);
    return !sw->error;
  }

  u32 len = 0;
  state_wrapper_do_u32(sw, &len);
  if (sw->error)
    return false;

  if (len != marker_len || sw->reader.pos + len > sw->reader.len)
    return false;

  if (len > 0) {
    if (memcmp(&sw->reader.buf[sw->reader.pos], marker, len) != 0)
      return false;
    sw->reader.pos += len;
  }
  return true;
}

bool state_wrapper_check_signature(state_wrapper_t* sw, const char* sig, u32 version, Error* err)
{
  /* Signature: identifies the blob format ("DUCK", "CPU0", ...).  Version:
   * monotonically increasing per-format integer; readers reject blobs newer
   * than themselves.  Both pieces are written in WRITE mode and validated
   * in READ mode. */
  if (!state_wrapper_do_marker(sw, sig)) {
    Error_set_string_fmt(err, "State wrapper signature mismatch (expected '%s')", sig);
    sw->error = true;
    return false;
  }

  u32 file_version = version;
  state_wrapper_do_u32(sw, &file_version);
  if (sw->error) {
    Error_set_string(err, "Failed to read state wrapper version");
    return false;
  }

  if (sw->mode == STATE_WRAPPER_READ) {
    if (file_version > version) {
      Error_set_string_fmt(err, "State wrapper version %u is newer than supported version %u",
                           file_version, version);
      sw->error = true;
      return false;
    }
    /* Adopt the on-disk version so per-field gates can compare against it. */
    sw->version = file_version;
  }

  return true;
}
