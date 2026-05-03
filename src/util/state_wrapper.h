/*
 * StateWrapper -> state_wrapper_t.
 *
 * Symmetric serialize/deserialize cursor used by every save-state Serialize()
 * site.  In C++ the API was templated (`sw.Do(&value)` dispatches on the type
 * of `value`); in C we expand each concrete primitive into its own typed
 * function (`state_wrapper_do_u32`, `_float`, ...).  Aggregates / POD structs
 * are serialized via `state_wrapper_do_array(sw, &s, sizeof(s), 1)` or
 * `state_wrapper_do_bytes`; both are bytewise so they round-trip whatever
 * was written, no member-by-member descent like a templated Do<MyStruct>.
 *
 * A wrapper is one-shot, single-direction: build it from a binary span and
 * a mode (READ pulls bytes out, WRITE pushes bytes in), then call do_*
 * helpers.  All errors are sticky: once `has_error` is true every later call
 * is a no-op (READ zeroes the destination, WRITE silently drops bytes), so
 * call sites can stay branch-free and check the flag at the end.
 *
 * The underlying buffer cursors come from common/binary_reader_writer.h.
 */

#ifndef CUPID_UTIL_STATE_WRAPPER_H
#define CUPID_UTIL_STATE_WRAPPER_H

#include "common/binary_reader_writer.h"
#include "common/types.h"

typedef struct small_string small_string_t;
typedef struct Error        Error;

typedef enum {
  STATE_WRAPPER_READ  = 0,
  STATE_WRAPPER_WRITE = 1,
} state_wrapper_mode_t;

/* Single struct holds both a reader and a writer; only the half matching
 * `mode` is touched.  Keeping both inline avoids a heap allocation and the
 * union games we'd otherwise need for the C++ templated Do<T> code paths. */
typedef struct state_wrapper {
  binary_span_reader_t reader;
  binary_span_writer_t writer;
  state_wrapper_mode_t mode;
  u32                  version;
  bool                 error;
} state_wrapper_t;

/* READ-mode: the data span is borrowed read-only for the wrapper's lifetime. */
void state_wrapper_init_read (state_wrapper_t* sw, const u8* data, size_t len, u32 version);
/* WRITE-mode: caller owns the writable span; bytes are pushed into it. */
void state_wrapper_init_write(state_wrapper_t* sw,       u8* data, size_t len, u32 version);

ALWAYS_INLINE bool state_wrapper_has_error  (const state_wrapper_t* sw) { return sw->error; }
ALWAYS_INLINE bool state_wrapper_is_reading (const state_wrapper_t* sw) { return sw->mode == STATE_WRAPPER_READ; }
ALWAYS_INLINE bool state_wrapper_is_writing (const state_wrapper_t* sw) { return sw->mode == STATE_WRAPPER_WRITE; }
ALWAYS_INLINE u32  state_wrapper_get_version(const state_wrapper_t* sw) { return sw->version; }

size_t state_wrapper_get_position(const state_wrapper_t* sw);
void   state_wrapper_set_position(state_wrapper_t* sw, size_t pos);
size_t state_wrapper_get_size    (const state_wrapper_t* sw);

/* Skip `count` bytes during READ.  Asserts in WRITE mode. */
void state_wrapper_skip_bytes(state_wrapper_t* sw, size_t count);

void state_wrapper_do_u8    (state_wrapper_t* sw, u8*     v);
void state_wrapper_do_u16   (state_wrapper_t* sw, u16*    v);
void state_wrapper_do_u32   (state_wrapper_t* sw, u32*    v);
void state_wrapper_do_u64   (state_wrapper_t* sw, u64*    v);
void state_wrapper_do_s8    (state_wrapper_t* sw, s8*     v);
void state_wrapper_do_s16   (state_wrapper_t* sw, s16*    v);
void state_wrapper_do_s32   (state_wrapper_t* sw, s32*    v);
void state_wrapper_do_s64   (state_wrapper_t* sw, s64*    v);
void state_wrapper_do_float (state_wrapper_t* sw, float*  v);
void state_wrapper_do_double(state_wrapper_t* sw, double* v);

void state_wrapper_do_bool  (state_wrapper_t* sw, bool*   v);

/* Bytewise array.  Stand-in for templated DoArray<T>(T*, size_t); no
 * per-element endian handling, the bytes round-trip as written.  Caller
 * passes total_bytes = element_size * count; we keep both for clarity. */
void state_wrapper_do_array(state_wrapper_t* sw, void* data, size_t element_size, size_t count);

/* Raw byte blob: stays as-is, callers know their own size. */
void state_wrapper_do_bytes(state_wrapper_t* sw, void* data, size_t length);

/* Length-prefixed string into/out of a small_string_t.  Length is u32. */
void state_wrapper_do_string(state_wrapper_t* sw, small_string_t* str);

 /* DoMarker port: writes / reads a length-prefixed marker string and verifies
 * it matches `marker` on READ.  Returns false on mismatch or sticky error. */
bool state_wrapper_do_marker(state_wrapper_t* sw, const char* marker);

/* Combined signature + version gate.  Writes/reads `sig` as a marker, then
 * the u32 `version`.  On READ, mismatched signature or version > current
 * version is treated as a hard error and recorded into `err` (if non-NULL).
 * Used by save-state files to assert "this blob is the format I think it is".
 */
bool state_wrapper_check_signature(state_wrapper_t* sw, const char* sig, u32 version, Error* err);

#endif /* CUPID_UTIL_STATE_WRAPPER_H */
