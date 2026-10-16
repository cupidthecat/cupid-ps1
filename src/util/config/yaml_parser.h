/*
 * files (gamedb.yaml, discsets.yaml, discdb.yaml).  Supports:
 *   - Block mappings (key: value\n  key: ...)
 *   - Block sequences (- item\n- item)
 *   - Plain scalars and double-quoted scalars (with \" \\ escapes)
 *   - UTF-8 passthrough (non-ASCII bytes are preserved verbatim)
 *   - Indentation-based nesting (any consistent indent step accepted)
 *   - Comments (# to end of line, ignored inside double-quoted strings)
 *
 * Does NOT support: anchors/aliases, tags, flow style ({}/[]),
 * multi-line scalars (>, |), single-quoted scalars, multi-document streams
 * (---).  None of those appear in the three resource files we consume.
 *
 * The parser walks the document and invokes a caller-supplied event callback.
 * Callbacks receive raw byte ranges into the input buffer; the buffer must
 * outlive the strings the caller retains.  No allocations are performed by
 * the parser (other than a small fixed-depth stack on the parser's own
 * stack frame).
 *
 * Scalars are returned as raw bytes from the input buffer.  For
 * double-quoted scalars, the surrounding quotes are stripped but the
 * internal escapes (\" and \\) are NOT decoded; callers that need decoded
 * text use yaml_unescape_scalar().
 */

#ifndef CUPID_UTIL_YAML_PARSER_H
#define CUPID_UTIL_YAML_PARSER_H

#include "common/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  YAML_EVENT_MAP_BEGIN,
  YAML_EVENT_MAP_END,
  YAML_EVENT_SEQ_BEGIN,
  YAML_EVENT_SEQ_END,
  YAML_EVENT_KEY,    /* str + len = key text (raw bytes) */
  YAML_EVENT_SCALAR, /* str + len = scalar value (raw bytes) */
} yaml_event_kind_t;

typedef struct {
  yaml_event_kind_t kind;
  const char*       str;  /* points into input buffer; NULL for container events */
  size_t            len;
  u32               line; /* 1-based, for diagnostics */
  bool              quoted; /* true if scalar/key was originally double-quoted */
} yaml_event_t;

/* Callback returns true to continue parsing, false to abort. */
typedef bool (*yaml_event_cb_t)(const yaml_event_t* ev, void* user);

typedef struct {
  const char* message; /* string literal, no need to free */
  u32         line;    /* 1-based line of failure */
} yaml_parse_error_t;

/* Parse `len` bytes of YAML, invoking `cb` per event.  Returns true on
 * success, false on parse error or callback abort.  On failure fills
 * `*out_err` if non-NULL. */
bool yaml_parse(const char* input, size_t len, yaml_event_cb_t cb, void* user,
                yaml_parse_error_t* out_err);

/* Decode \" and \\ escapes from a (possibly raw) scalar source range into a
 * caller buffer.  Always NUL-terminates; returns the number of bytes written
 * (not counting the NUL), capped at out_size-1.  Returns 0 if src is NULL or
 * src_len is zero (still NUL-terminates if out_size > 0).
 *
 * Safe to call on plain scalars too; backslash sequences other than \" and
 * \\ are passed through verbatim. */
size_t yaml_unescape_scalar(const char* src, size_t src_len, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_UTIL_YAML_PARSER_H */
