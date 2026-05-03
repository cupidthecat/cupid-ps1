 /*
 *
 * Unit tests for util/yaml_parser.  The parser is event-driven; tests
 * compare the produced event stream against an expected sequence using a
 * small generic matcher.  Real-resource test loads gamedb.yaml from
 * data/resources and validates a 100-entry truncation parses cleanly.
 */

#include "tests/test_harness.h"

#include "util/yaml_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  yaml_event_kind_t kind;
  const char*       str; /* expected exact text, or NULL to skip text check */
  size_t            len;
} expect_t;

typedef struct {
  const expect_t* want;
  size_t          want_count;
  size_t          cursor;
  bool            ok;
  char            err[256];
} match_ctx_t;

static bool match_cb(const yaml_event_t* ev, void* user)
{
  match_ctx_t* c = (match_ctx_t*)user;
  if (c->cursor >= c->want_count) {
    snprintf(c->err, sizeof(c->err),
             "extra event #%zu kind=%d", c->cursor, (int)ev->kind);
    c->ok = false;
    return false;
  }
  const expect_t* e = &c->want[c->cursor];
  if (ev->kind != e->kind) {
    snprintf(c->err, sizeof(c->err),
             "event %zu kind mismatch: got %d, want %d (line %u)",
             c->cursor, (int)ev->kind, (int)e->kind, ev->line);
    c->ok = false;
    return false;
  }
  if (e->str) {
    if (ev->len != e->len || memcmp(ev->str, e->str, e->len) != 0) {
      snprintf(c->err, sizeof(c->err),
               "event %zu text mismatch: got '%.*s', want '%.*s'",
               c->cursor, (int)ev->len, ev->str ? ev->str : "(null)",
               (int)e->len, e->str);
      c->ok = false;
      return false;
    }
  }
  c->cursor++;
  return true;
}

#define MB { YAML_EVENT_MAP_BEGIN, NULL, 0 }
#define ME { YAML_EVENT_MAP_END,   NULL, 0 }
#define SB { YAML_EVENT_SEQ_BEGIN, NULL, 0 }
#define SE { YAML_EVENT_SEQ_END,   NULL, 0 }
#define K(s)  { YAML_EVENT_KEY,    (s), sizeof(s) - 1 }
#define SC(s) { YAML_EVENT_SCALAR, (s), sizeof(s) - 1 }

static void run_check(const char* yaml, const expect_t* want, size_t n,
                      const char* test_label)
{
  match_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.want       = want;
  ctx.want_count = n;
  ctx.ok         = true;

  yaml_parse_error_t err;
  bool ok = yaml_parse(yaml, strlen(yaml), match_cb, &ctx, &err);
  if (!ok && !ctx.ok) {
    fprintf(stderr, "%s: parse error line %u: %s\n   event mismatch: %s\n",
            test_label, err.line, err.message ? err.message : "(null)", ctx.err);
  } else if (!ok) {
    fprintf(stderr, "%s: parse error line %u: %s\n",
            test_label, err.line, err.message ? err.message : "(null)");
  } else if (!ctx.ok) {
    fprintf(stderr, "%s: %s\n", test_label, ctx.err);
  } else if (ctx.cursor != n) {
    fprintf(stderr, "%s: event count mismatch: got %zu, want %zu\n",
            test_label, ctx.cursor, n);
    ctx.ok = false;
  }
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ctx.ok);
  EXPECT_EQ(ctx.cursor, n);
}

TEST(YamlParser, simple_map)
{
  const char* y =
    "name: Crash\n"
    "year: 1996\n";
  const expect_t want[] = {
    MB,
    K("name"),  SC("Crash"),
    K("year"),  SC("1996"),
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "simple_map");
}

TEST(YamlParser, quoted_scalar)
{
  const char* y = "name: \"hello world\"\n";
  const expect_t want[] = {
    MB,
    K("name"), SC("hello world"),
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "quoted_scalar");
}

TEST(YamlParser, unescape_helper_roundtrip)
{
  /* Raw scalar bytes after parse: 'Crash \"the\" Bandi\\cot'  (verbatim
   * input bytes between the surrounding quotes). */
  const char* raw = "Crash \\\"the\\\" Bandi\\\\cot";
  const size_t raw_len = strlen(raw);
  char out[64];
  size_t n = yaml_unescape_scalar(raw, raw_len, out, sizeof(out));
  /* Expected decoded: Crash "the" Bandi\cot */
  EXPECT_STREQ(out, "Crash \"the\" Bandi\\cot");
  EXPECT_EQ(n, strlen("Crash \"the\" Bandi\\cot"));

  /* Unknown escape sequence should pass through verbatim. */
  const char* raw2 = "x\\ny";
  n = yaml_unescape_scalar(raw2, strlen(raw2), out, sizeof(out));
  EXPECT_STREQ(out, "x\\ny");
  EXPECT_EQ(n, strlen("x\\ny"));

  /* Empty/null safety. */
  out[0] = 'x';
  EXPECT_EQ(yaml_unescape_scalar(NULL, 0, out, sizeof(out)), (size_t)0);
  EXPECT_EQ(out[0], '\0');
  EXPECT_EQ(yaml_unescape_scalar("ignored", 7, out, 0), (size_t)0);
}

TEST(YamlParser, quoted_with_escapes_in_stream)
{
  /* Verify the parser preserves raw escape bytes (no decoding inside the
   * scanner) so callers can decide whether to decode. */
  const char* y = "name: \"a\\\"b\\\\c\"\n";
  const expect_t want[] = {
    MB,
    K("name"),
    SC("a\\\"b\\\\c"),  /* raw bytes between the quotes */
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "quoted_with_escapes_in_stream");
}

TEST(YamlParser, nested_map_in_seq)
{
  const char* y =
    "- name: A\n"
    "  year: 1\n"
    "- name: B\n"
    "  year: 2\n";
  const expect_t want[] = {
    SB,
    MB, K("name"), SC("A"), K("year"), SC("1"), ME,
    MB, K("name"), SC("B"), K("year"), SC("2"), ME,
    SE,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "nested_map_in_seq");
}

TEST(YamlParser, top_level_serial_map)
{
  /* gamedb.yaml shape -- serial keys with nested object values, including
   * sub-sequences for controllers and traits. */
  const char* y =
    "SCUS-94900:\n"
    "  name: \"Crash Bandicoot\"\n"
    "  controllers:\n"
    "    - DigitalController\n"
    "    - AnalogController\n"
    "  traits:\n"
    "    - DisableAutoAnalogMode\n"
    "SLUS-00001:\n"
    "  name: \"Other\"\n";
  const expect_t want[] = {
    MB,
      K("SCUS-94900"),
      MB,
        K("name"),        SC("Crash Bandicoot"),
        K("controllers"), SB, SC("DigitalController"), SC("AnalogController"), SE,
        K("traits"),      SB, SC("DisableAutoAnalogMode"), SE,
      ME,
      K("SLUS-00001"),
      MB,
        K("name"),        SC("Other"),
      ME,
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "top_level_serial_map");
}

TEST(YamlParser, top_level_seq)
{
  /* discsets.yaml shape. */
  const char* y =
    "- name: \"3x3 Eyes\"\n"
    "  serials:\n"
    "    - SLPS-00071\n"
    "    - SLPS-00072\n"
    "- name: \"Other\"\n"
    "  serials:\n"
    "    - SLPS-01497\n";
  const expect_t want[] = {
    SB,
      MB,
        K("name"),    SC("3x3 Eyes"),
        K("serials"), SB, SC("SLPS-00071"), SC("SLPS-00072"), SE,
      ME,
      MB,
        K("name"),    SC("Other"),
        K("serials"), SB, SC("SLPS-01497"), SE,
      ME,
    SE,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "top_level_seq");
}

TEST(YamlParser, unicode_passthrough)
{
  /* Japanese title bytes from real gamedb.yaml -- verify byte-exact
   * passthrough into the SCALAR event. */
  const char* y = "name: \"\xE9\xAB\x98\xE6\xA0\xA1\xE9\x87\x8E\xE7\x90\x83\"\n";
  const expect_t want[] = {
    MB,
    K("name"),
    SC("\xE9\xAB\x98\xE6\xA0\xA1\xE9\x87\x8E\xE7\x90\x83"),
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "unicode_passthrough");
}

TEST(YamlParser, comment_skip)
{
  const char* y =
    "# top-level comment\n"
    "name: x  # trailing comment\n"
    "# another\n"
    "year: 1996\n";
  const expect_t want[] = {
    MB,
    K("name"), SC("x"),
    K("year"), SC("1996"),
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]), "comment_skip");
}

TEST(YamlParser, hash_inside_quoted_string_not_a_comment)
{
  /* A '#' inside quotes must be preserved verbatim. */
  const char* y = "title: \"foo # bar\"\n";
  const expect_t want[] = {
    MB,
    K("title"), SC("foo # bar"),
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]),
            "hash_inside_quoted_string_not_a_comment");
}

TEST(YamlParser, key_with_no_value_then_nested_map)
{
  /* gamedb.yaml-style: "metadata:" followed by indented sub-keys. */
  const char* y =
    "metadata:\n"
    "  publisher: Acme\n"
    "  developer: Bob\n";
  const expect_t want[] = {
    MB,
    K("metadata"),
    MB,
      K("publisher"), SC("Acme"),
      K("developer"), SC("Bob"),
    ME,
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]),
            "key_with_no_value_then_nested_map");
}

TEST(YamlParser, deep_nesting_in_discdb_shape)
{
  /* discdb.yaml shape: serial -> trackData (seq) -> tracks (seq) -> object. */
  const char* y =
    "SLPM-80238:\n"
    "  name: \"Demo\"\n"
    "  trackData:\n"
    "    - tracks:\n"
    "        - size: 443733024\n"
    "          md5: \"0aa29df886d9f4f3ec5340bdd3b4f3be\"\n";
  const expect_t want[] = {
    MB,
      K("SLPM-80238"),
      MB,
        K("name"), SC("Demo"),
        K("trackData"),
        SB,
          MB,
            K("tracks"),
            SB,
              MB,
                K("size"), SC("443733024"),
                K("md5"),  SC("0aa29df886d9f4f3ec5340bdd3b4f3be"),
              ME,
            SE,
          ME,
        SE,
      ME,
    ME,
  };
  run_check(y, want, sizeof(want) / sizeof(want[0]),
            "deep_nesting_in_discdb_shape");
}

typedef struct {
  size_t depth;
  size_t top_keys;     /* keys at depth 1 (inside top-level map) */
  size_t total_events;
} count_ctx_t;

static bool count_cb(const yaml_event_t* ev, void* user)
{
  count_ctx_t* c = (count_ctx_t*)user;
  c->total_events++;
  if (ev->kind == YAML_EVENT_KEY && c->depth == 1)
    c->top_keys++;
  if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN)
    c->depth++;
  else if (ev->kind == YAML_EVENT_MAP_END || ev->kind == YAML_EVENT_SEQ_END)
    c->depth--;
  return true;
}

static char* read_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char* buf = (char*)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) { free(buf); return NULL; }
  buf[sz] = '\0';
  *out_len = (size_t)sz;
  return buf;
}

TEST(YamlParser, real_gamedb_first_100)
{
  /* Read the full file, then truncate at the start of the 101st top-level
   * entry.  Top-level entries in gamedb.yaml begin with a non-space byte at
   * column 0.  A line beginning with '#' is a comment, not an entry.
   * Method: walk newlines, count line-starts where the next char is not
   * space/tab/newline/'#'; truncate at the 101st. */
  size_t len = 0;
  char* buf = read_file("data/resources/gamedb.yaml", &len);
  EXPECT_NOT_NULL(buf);
  if (!buf) return;

  size_t entry_count = 0;
  size_t cut = len;
  /* Position 0 is a line-start. */
  size_t i = 0;
  while (i < len) {
    /* Is i a line-start that begins an entry? */
    char c = buf[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '#') {
      entry_count++;
      if (entry_count == 101) { cut = i; break; }
    }
    /* Advance to next line-start. */
    while (i < len && buf[i] != '\n') i++;
    if (i < len) i++;
  }

  count_ctx_t ctx = { .depth = 0, .top_keys = 0, .total_events = 0 };
  yaml_parse_error_t err;
  bool ok = yaml_parse(buf, cut, count_cb, &ctx, &err);
  if (!ok) {
    fprintf(stderr, "real_gamedb_first_100: parse error line %u: %s\n",
            err.line, err.message ? err.message : "(null)");
  }
  EXPECT_TRUE(ok);
  EXPECT_EQ(ctx.top_keys, (size_t)100);
  free(buf);
}

TEST(YamlParser, real_gamedb_full_count)
{
  /* Sanity: parse the entire gamedb.yaml; expect ~10000+ top-level keys. */
  size_t len = 0;
  char* buf = read_file("data/resources/gamedb.yaml", &len);
  EXPECT_NOT_NULL(buf);
  if (!buf) return;

  count_ctx_t ctx = { .depth = 0, .top_keys = 0, .total_events = 0 };
  yaml_parse_error_t err;
  bool ok = yaml_parse(buf, len, count_cb, &ctx, &err);
  if (!ok) {
    fprintf(stderr, "real_gamedb_full_count: parse error line %u: %s\n",
            err.line, err.message ? err.message : "(null)");
  }
  EXPECT_TRUE(ok);
  /* Real value at time of writing: 10764. */
  EXPECT_TRUE(ctx.top_keys >= 10000);
  EXPECT_TRUE(ctx.top_keys <= 12000);
  free(buf);
}

TEST(YamlParser, real_discsets_full)
{
  size_t len = 0;
  char* buf = read_file("data/resources/discsets.yaml", &len);
  EXPECT_NOT_NULL(buf);
  if (!buf) return;
  count_ctx_t ctx = { .depth = 0, .top_keys = 0, .total_events = 0 };
  yaml_parse_error_t err;
  bool ok = yaml_parse(buf, len, count_cb, &ctx, &err);
  if (!ok)
    fprintf(stderr, "discsets parse error line %u: %s\n",
            err.line, err.message ? err.message : "(null)");
  EXPECT_TRUE(ok);
  /* Top-level is a SEQ, so no top-level KEY events; just confirm we got
   * non-trivial events out. */
  EXPECT_TRUE(ctx.total_events >= 1000);
  free(buf);
}

TEST(YamlParser, real_discdb_full)
{
  size_t len = 0;
  char* buf = read_file("data/resources/discdb.yaml", &len);
  EXPECT_NOT_NULL(buf);
  if (!buf) return;
  count_ctx_t ctx = { .depth = 0, .top_keys = 0, .total_events = 0 };
  yaml_parse_error_t err;
  bool ok = yaml_parse(buf, len, count_cb, &ctx, &err);
  if (!ok)
    fprintf(stderr, "discdb parse error line %u: %s\n",
            err.line, err.message ? err.message : "(null)");
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ctx.top_keys >= 5000);
  free(buf);
}
