/*
 * Hand-rolled minimal YAML subset parser; see yaml_parser.h for the supported
 * grammar and rationale.  No allocation; no third-party dep.
 *
 * Implementation overview
 * -----------------------
 * Single-pass scanner with a fixed-depth container stack.  Each iteration of
 * the main loop processes one logical line:
 *   1. Skip blank lines and full-line comments, advancing line counter.
 *   2. Measure leading-space indent.
 *   3. If the line begins with "- " at this indent, the line is a sequence
 *      item.  Pop containers until top is a SEQ at this indent (open one if
 *      needed), then recurse on the rest of the line at indent+2 (where the
 *      "- " was); the rest may be a scalar or another "key: value" pair
 *      that opens a nested MAP at indent+2.
 *   4. Otherwise the line is a mapping entry "key:" or "key: value".  Pop
 *      containers until top is a MAP at this indent (open one if needed),
 *      emit KEY, then either emit SCALAR (value present) or remember that a
 *      nested container is expected when we next see a deeper indent.
 *
 * Container stack
 * ---------------
 * Each frame stores (kind, indent).  Indent semantics:
 *   - For a MAP, indent is the column where keys live.
 *   - For a SEQ, indent is the column where '-' lives.
 * When we see a line at indent N:
 *   - Pop frames whose indent > N.
 *   - If top frame's indent == N and kind matches expected, continue.
 *   - If top frame's indent < N, this is a nested container; the parent
 *     either has a pending KEY (the new container is the value of that key)
 *     or we just opened a new container as part of '- ' descent.
 *
 * "Pending key" state
 * -------------------
 * After emitting KEY for "key:" with no value on the line, we set a flag.
 * The next non-blank line at greater indent opens either a SEQ ('- ' first
 * char) or a MAP ('key:' first non-space token).  The flag is cleared at
 * that point; mismatches produce a parse error.
 */

#include "util/yaml_parser.h"

#include "common/log.h"

#include <stddef.h>
#include <string.h>

LOG_CHANNEL(GameDatabase);

#define MAX_DEPTH 32

typedef enum { CTN_MAP, CTN_SEQ } ctn_kind_t;

typedef struct {
  ctn_kind_t kind;
  u32        indent;
} ctn_t;

typedef struct {
  const char*         p;          /* current cursor */
  const char*         end;        /* one past last byte */
  u32                 line;       /* current 1-based line */
  ctn_t               stack[MAX_DEPTH];
  u32                 depth;
  yaml_event_cb_t     cb;
  void*               user;
  yaml_parse_error_t* err;
  bool                aborted;    /* callback returned false */
} parser_t;

static bool fail(parser_t* p, const char* msg)
{
  if (p->err) {
    p->err->message = msg;
    p->err->line    = p->line;
  }
  return false;
}

static bool emit(parser_t* p, yaml_event_kind_t k, const char* s, size_t n, bool quoted)
{
  yaml_event_t ev;
  ev.kind   = k;
  ev.str    = s;
  ev.len    = n;
  ev.line   = p->line;
  ev.quoted = quoted;
  if (!p->cb(&ev, p->user)) {
    p->aborted = true;
    return false;
  }
  return true;
}

static bool push_ctn(parser_t* p, ctn_kind_t k, u32 indent)
{
  if (p->depth >= MAX_DEPTH)
    return fail(p, "container nesting too deep");
  p->stack[p->depth].kind   = k;
  p->stack[p->depth].indent = indent;
  p->depth++;
  return emit(p, k == CTN_MAP ? YAML_EVENT_MAP_BEGIN : YAML_EVENT_SEQ_BEGIN, NULL, 0, false);
}

static bool pop_ctn(parser_t* p)
{
  if (p->depth == 0)
    return true;
  p->depth--;
  const ctn_kind_t k = p->stack[p->depth].kind;
  return emit(p, k == CTN_MAP ? YAML_EVENT_MAP_END : YAML_EVENT_SEQ_END, NULL, 0, false);
}

/* Pop frames whose indent is strictly greater than `indent`. */
static bool pop_to(parser_t* p, u32 indent)
{
  while (p->depth > 0 && p->stack[p->depth - 1].indent > indent) {
    if (!pop_ctn(p))
      return false;
  }
  return true;
}

/* Read a double-quoted scalar starting at the cursor (which is on the
 * opening quote).  Stores raw inner bytes in out_str/out_len (excluding
 * quotes).  Advances p->p past the closing quote.  Returns false on
 * unterminated input. */
static bool read_quoted(parser_t* p, const char** out_str, size_t* out_len)
{
  if (p->p >= p->end || *p->p != '"')
    return fail(p, "expected '\"'");
  p->p++; /* past opening quote */
  const char* start = p->p;
  while (p->p < p->end) {
    const char c = *p->p;
    if (c == '\\') {
      /* Skip escape: backslash + next byte (any). */
      if (p->p + 1 >= p->end)
        return fail(p, "unterminated escape in quoted string");
      p->p += 2;
      continue;
    }
    if (c == '"') {
      *out_str = start;
      *out_len = (size_t)(p->p - start);
      p->p++; /* past closing quote */
      return true;
    }
    if (c == '\n')
      return fail(p, "newline inside quoted string");
    p->p++;
  }
  return fail(p, "unterminated quoted string");
}

/* Read a plain scalar from current cursor up to: end-of-input, newline,
 * unescaped '#' (comment) preceded by whitespace, or any of the bytes in
 * `terminators` (which may be NULL/empty).  Trailing ASCII whitespace is
 * trimmed from the returned slice.  Advances p->p to the terminator (the
 * '#' or terminator byte) or end-of-line, but does NOT consume the
 * terminator itself.  Returns true; never reports a parse error. */
static bool read_plain(parser_t* p, const char* terminators,
                       const char** out_str, size_t* out_len)
{
  const char* start = p->p;
  const char* last_nonspace = start; /* one past the last non-space char seen */
  while (p->p < p->end) {
    const char c = *p->p;
    if (c == '\n')
      break;
    /* '#' starts a comment only when preceded by whitespace (or at start
     * of the scalar). YAML rule. */
    if (c == '#' && (p->p == start || (p->p[-1] == ' ' || p->p[-1] == '\t')))
      break;
    if (terminators) {
      const char* t = terminators;
      bool hit = false;
      while (*t) {
        if (*t == c) { hit = true; break; }
        t++;
      }
      if (hit)
        break;
    }
    if (c != ' ' && c != '\t')
      last_nonspace = p->p + 1;
    p->p++;
  }
  *out_str = start;
  *out_len = (size_t)(last_nonspace - start);
  return true;
}

/* Skip spaces/tabs (but not newlines). */
static void skip_inline_ws(parser_t* p)
{
  while (p->p < p->end && (*p->p == ' ' || *p->p == '\t'))
    p->p++;
}

/* Skip from current position to and past the next '\n', incrementing line. */
static void skip_to_eol(parser_t* p)
{
  while (p->p < p->end && *p->p != '\n')
    p->p++;
  if (p->p < p->end) {
    p->p++;
    p->line++;
  }
}

/* Parse a scalar (quoted or plain).  For plain, `terminators` lists extra
 * characters that end the scalar (e.g., ":" when reading a key, "" when
 * reading a value). */
static bool read_scalar(parser_t* p, const char* terminators,
                        const char** out_str, size_t* out_len, bool* out_quoted)
{
  if (p->p < p->end && *p->p == '"') {
    *out_quoted = true;
    return read_quoted(p, out_str, out_len);
  }
  *out_quoted = false;
  return read_plain(p, terminators, out_str, out_len);
}

/* Forward decl. */
static bool process_value(parser_t* p, u32 indent);

/* Process the rest of a line that begins (after current cursor) with either:
 *   - a "key:" or "key: value" mapping entry (opening a MAP at `indent` if
 *     the top frame isn't already a MAP at `indent`), or
 *   - a plain/quoted scalar (when the enclosing container is a SEQ that
 *     takes scalar items).
 *
 * Called from line-start (after indent measurement) and from within "- "
 * processing (where indent = column-after-"- ").
 */
static bool process_value(parser_t* p, u32 indent)
{
  /* Speculatively read a scalar with ':' as a terminator.  If we land on
   * ':' followed by whitespace/EOL, this was a key.  Otherwise it's a
   * standalone scalar (sequence item).
   *
   * Special case: if the very first byte is '"', read_quoted consumes the
   * whole quoted string; if a ':' follows, it's still a key. */
  const char* save_p    = p->p;
  const u32   save_line = p->line;

  const char* tok_str = NULL;
  size_t      tok_len = 0;
  bool        tok_quoted = false;
  if (!read_scalar(p, ":", &tok_str, &tok_len, &tok_quoted))
    return false;

  /* Is this a key?  Need ':' next, then whitespace or EOL. */
  bool is_key = false;
  if (p->p < p->end && *p->p == ':') {
    if (p->p + 1 >= p->end ||
        p->p[1] == ' ' || p->p[1] == '\t' || p->p[1] == '\n' || p->p[1] == '\r') {
      is_key = true;
    }
  }

  if (!is_key) {
    /* Pure scalar value; it's a sequence item.  We must already be inside
     * a SEQ; if the caller was a "- " descent we definitely are. */
    if (p->depth == 0 || p->stack[p->depth - 1].kind != CTN_SEQ) {
      /* Restore and complain. */
      p->p    = save_p;
      p->line = save_line;
      return fail(p, "scalar value outside any sequence");
    }
    if (!emit(p, YAML_EVENT_SCALAR, tok_str, tok_len, tok_quoted))
      return false;
    /* Skip trailing whitespace + comment to end of line. */
    skip_inline_ws(p);
    if (p->p < p->end && *p->p == '#') {
      /* eat comment */
      while (p->p < p->end && *p->p != '\n')
        p->p++;
    }
    return true;
  }

  /* It's a key.  Open a MAP at this indent if not already on one. */
  if (p->depth == 0 || p->stack[p->depth - 1].kind != CTN_MAP ||
      p->stack[p->depth - 1].indent != indent) {
    if (!push_ctn(p, CTN_MAP, indent))
      return false;
  }

  if (!emit(p, YAML_EVENT_KEY, tok_str, tok_len, tok_quoted))
    return false;

  /* Consume the ':'. */
  p->p++;
  skip_inline_ws(p);

  /* What follows? */
  if (p->p >= p->end || *p->p == '\n' || *p->p == '\r' ||
      (*p->p == '#' /* comment after key with no value */)) {
    /* Empty value; expect a nested container on a deeper indented line.
     * Skip to EOL; the main loop will detect the deeper indent. */
    while (p->p < p->end && *p->p != '\n')
      p->p++;
    return true;
  }

  /* Inline value: read one scalar to end of line (no terminators; ':' in
   * a value is fine). */
  const char* val_str = NULL;
  size_t      val_len = 0;
  bool        val_quoted = false;
  if (!read_scalar(p, NULL, &val_str, &val_len, &val_quoted))
    return false;
  if (!emit(p, YAML_EVENT_SCALAR, val_str, val_len, val_quoted))
    return false;

  /* Trailing whitespace + optional comment. */
  skip_inline_ws(p);
  if (p->p < p->end && *p->p == '#') {
    while (p->p < p->end && *p->p != '\n')
      p->p++;
  }
  return true;
}

bool yaml_parse(const char* input, size_t len, yaml_event_cb_t cb, void* user,
                yaml_parse_error_t* out_err)
{
  if (out_err) {
    out_err->message = NULL;
    out_err->line    = 0;
  }
  if (!input || !cb) {
    if (out_err) {
      out_err->message = "null input or callback";
      out_err->line    = 0;
    }
    return false;
  }

  parser_t st;
  st.p       = input;
  st.end     = input + len;
  st.line    = 1;
  st.depth   = 0;
  st.cb      = cb;
  st.user    = user;
  st.err     = out_err;
  st.aborted = false;
  parser_t* p = &st;

  while (p->p < p->end) {
    const char* line_start = p->p;
    /* Count indent (spaces/tabs treated as 1 column each; we only see
     * single-column for relative comparison). */
    u32 indent = 0;
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t')) {
      indent++;
      p->p++;
    }
    if (p->p >= p->end) break;
    if (*p->p == '\n') {
      /* Blank line. */
      p->p++;
      p->line++;
      continue;
    }
    if (*p->p == '\r') {
      p->p++;
      continue;
    }
    if (*p->p == '#') {
      /* Full-line comment. */
      skip_to_eol(p);
      continue;
    }
    if (*p->p == '-' && (p->p + 1 == p->end || p->p[1] == ' ' || p->p[1] == '\n' || p->p[1] == '\t')) {
      /* Sequence-item line.  Ensure SEQ at this indent. */
      if (!pop_to(p, indent))
        return false;
      if (p->depth == 0 || p->stack[p->depth - 1].kind != CTN_SEQ ||
          p->stack[p->depth - 1].indent != indent) {
        if (!push_ctn(p, CTN_SEQ, indent))
          return false;
      }
      /* Consume "- " (or "-" at EOL meaning nested container next line). */
      p->p++; /* past '-' */
      if (p->p < p->end && (*p->p == ' ' || *p->p == '\t')) {
        /* Item on same line at column = indent + 2 (we treat "- " width
         * as 2 for nesting purposes; the inner key column is wherever
         * the next non-space char actually lives). */
        skip_inline_ws(p);
        if (p->p < p->end && *p->p != '\n' && *p->p != '\r' && *p->p != '#') {
          /* The inner item starts at the column of the current cursor.
           * For nested map indent we use the actual column. */
          const u32 inner_indent = (u32)(p->p - line_start);
          if (!process_value(p, inner_indent))
            return false;
        }
      }
      /* Skip to EOL. */
      while (p->p < p->end && *p->p != '\n')
        p->p++;
      if (p->p < p->end) {
        p->p++;
        p->line++;
      }
      continue;
    }

    if (!pop_to(p, indent))
      return false;
    if (!process_value(p, indent))
      return false;

    /* Advance past EOL. */
    while (p->p < p->end && *p->p != '\n')
      p->p++;
    if (p->p < p->end) {
      p->p++;
      p->line++;
    }
  }

  /* Close all open containers. */
  while (p->depth > 0) {
    if (!pop_ctn(p))
      return false;
  }
  return !p->aborted;
}

size_t yaml_unescape_scalar(const char* src, size_t src_len, char* out, size_t out_size)
{
  if (out_size == 0)
    return 0;
  if (!src || src_len == 0) {
    out[0] = '\0';
    return 0;
  }
  size_t w = 0;
  const size_t cap = out_size - 1;
  for (size_t i = 0; i < src_len && w < cap; i++) {
    char c = src[i];
    if (c == '\\' && i + 1 < src_len) {
      const char n = src[i + 1];
      if (n == '"' || n == '\\') {
        out[w++] = n;
        i++;
        continue;
      }
      /* Unknown escape: pass through verbatim (the backslash + the next
       * only \" and \\. */
    }
    out[w++] = c;
  }
  out[w] = '\0';
  return w;
}
