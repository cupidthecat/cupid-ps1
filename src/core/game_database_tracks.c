/*
 * GameDatabase TrackHashes: lazy loader + open-addressing MD5 hash map.
 *
 * (EnsureTrackHashesMapLoaded / LoadTrackHashes / GetTrackHashesMap).
 *
 * The discdb.yaml resource (~30K MD5 hashes across ~10K serials) is parsed
 * once on the first lookup.  Each MD5 -> (serial, revision_str, revision)
 * mapping is inserted into a power-of-two-sized open-addressing table
 * keyed on the full 16-byte MD5.  serial / revision_str are strdup'd so
 * the parsed YAML buffer can be freed after load.
 *
 * Hash function: take the first 8 bytes of MD5 as a host-order u64.  MD5
 * is high entropy so this is uniform without further mixing.  Collisions
 * resolved by linear probing with full 16-byte memcmp.
 */

#include "core/game_database.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/timer.h"
#include "common/types.h"

#include "util/cd_image_hasher.h"
#include "util/yaml_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GameDatabase);

#define DISCDB_YAML_PATH       "data/resources/discdb.yaml"
#define INITIAL_BUCKET_COUNT   65536u   /* power of two; ~30K entries -> load 0.46 */
#define LOAD_FACTOR_NUM        7        /* grow when entries_count > buckets * 7/10 */
#define LOAD_FACTOR_DEN        10
#define MAX_TS_DEPTH           16

/* Hash table                                                              */

typedef struct {
  u8                          md5[16];
  game_database_track_data_t  data;
  bool                        occupied;
} track_bucket_t;

static struct {
  bool             loaded;       /* set true on first call to ensure-loaded */
  track_bucket_t*  buckets;
  u32              bucket_count; /* power of two */
  u32              entries_count;
  u32              max_probe;    /* longest probe chain observed (sanity) */
} s_tracks;

static bool table_alloc(u32 bucket_count)
{
  s_tracks.buckets = (track_bucket_t*)calloc(bucket_count, sizeof(track_bucket_t));
  if (!s_tracks.buckets) {
    ERROR_LOG("Failed to allocate track-hash bucket table (%u buckets).", bucket_count);
    return false;
  }
  s_tracks.bucket_count = bucket_count;
  s_tracks.entries_count = 0;
  s_tracks.max_probe = 0;
  return true;
}

/* Insert into the current table.  Steals ownership of `data.serial` /
 * `data.revision_str` on success.  On duplicate MD5 we keep the first
 */
static bool table_insert(const u8 md5[16], game_database_track_data_t data)
{
  u64 h;
  memcpy(&h, md5, sizeof(h));
  const u32 mask = s_tracks.bucket_count - 1u;
  u32 i = (u32)(h & mask);
  for (u32 probe = 0; probe < s_tracks.bucket_count; probe++) {
    track_bucket_t* b = &s_tracks.buckets[i];
    if (!b->occupied) {
      memcpy(b->md5, md5, 16);
      b->data     = data;
      b->occupied = true;
      s_tracks.entries_count++;
      if (probe > s_tracks.max_probe) s_tracks.max_probe = probe;
      return true;
    }
    if (memcmp(b->md5, md5, 16) == 0) {
      free(data.serial);
      free(data.revision_str);
      return true;
    }
    i = (i + 1u) & mask;
  }
  return false;
}

static bool table_grow(void)
{
  track_bucket_t* old = s_tracks.buckets;
  const u32 old_count = s_tracks.bucket_count;
  const u32 new_count = old_count * 2u;

  s_tracks.buckets = (track_bucket_t*)calloc(new_count, sizeof(track_bucket_t));
  if (!s_tracks.buckets) {
    ERROR_LOG("Failed to grow track-hash bucket table to %u.", new_count);
    s_tracks.buckets = old;
    return false;
  }
  s_tracks.bucket_count = new_count;
  s_tracks.entries_count = 0;
  s_tracks.max_probe = 0;

  for (u32 i = 0; i < old_count; i++) {
    if (old[i].occupied)
      table_insert(old[i].md5, old[i].data);
  }
  free(old);
  return true;
}

static bool maybe_grow(void)
{
  if ((u64)(s_tracks.entries_count + 1u) * (u64)LOAD_FACTOR_DEN >
      (u64)s_tracks.bucket_count * (u64)LOAD_FACTOR_NUM) {
    return table_grow();
  }
  return true;
}

/* YAML parsing                                                            */

/* State machine pattern follows gamedb_cb in game_database.c: each `KEY`
 * sets a "VAL_*" state on top of stack, the next event opens / consumes
 * the value, then the state returns to the parent KEY-expectation state. */
typedef enum {
  TS_TOP_MAP,              /* expect MAP_BEGIN at depth 0 */
  TS_TOP_KEY,              /* inside top-level map: expect KEY = serial, or MAP_END */
  TS_ENTRY_AWAIT_BODY,     /* just consumed serial KEY; expect MAP_BEGIN */
  TS_ENTRY_KEY,            /* inside entry body: expect KEY (name, trackData, ...) */
  TS_ENTRY_VAL_TRACKDATA,  /* expect SEQ_BEGIN for the trackData seq */
  TS_ENTRY_VAL_UNKNOWN,    /* unknown entry-level key value: scalar or container */
  TS_TRACKDATA_SEQ,        /* inside trackData seq: expect MAP_BEGIN per item, or SEQ_END */
  TS_REVISION_KEY,         /* inside one revision map: expect KEY (version, tracks, ...) */
  TS_REVISION_VAL_VERSION, /* expect SCALAR for the version string */
  TS_REVISION_VAL_TRACKS,  /* expect SEQ_BEGIN for the tracks seq */
  TS_REVISION_VAL_UNKNOWN, /* unknown revision-level key value */
  TS_TRACKS_SEQ,           /* inside tracks seq: expect MAP_BEGIN per track, or SEQ_END */
  TS_TRACK_KEY,            /* inside one track map: expect KEY (md5, size, ...) */
  TS_TRACK_VAL_MD5,        /* expect SCALAR for the md5 hex string */
  TS_TRACK_VAL_UNKNOWN,    /* unknown track-level key value */
  TS_SKIP,                 /* swallow a container value */
} ts_state_t;

typedef struct {
  ts_state_t state_stack[MAX_TS_DEPTH];
  u32        state_top;
  u32        skip_depth;     /* nested container count while in TS_SKIP */

  /* Per-entry transient strings (malloc'd, NUL-terminated). */
  char* cur_serial;
  char* cur_revision_str;    /* NULL if no version: key seen */
  u32   cur_revision;        /* 0-based index of trackData revision */

  u32   serials_count;
  bool  oom;
} ts_ctx_t;

static void ts_push(ts_ctx_t* c, ts_state_t s)
{
  if (c->state_top + 1u >= MAX_TS_DEPTH) {
    ERROR_LOG("track-hashes state stack overflow");
    return;
  }
  c->state_top++;
  c->state_stack[c->state_top] = s;
}

static void ts_pop(ts_ctx_t* c)
{
  if (c->state_top == 0) return;
  c->state_top--;
}

static ts_state_t ts_cur(const ts_ctx_t* c) { return c->state_stack[c->state_top]; }
static void       ts_set(ts_ctx_t* c, ts_state_t s) { c->state_stack[c->state_top] = s; }

static char* dup_slice(const char* s, size_t len)
{
  char* out = (char*)malloc(len + 1u);
  if (!out) return NULL;
  if (len > 0) memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

static void reset_entry(ts_ctx_t* c)
{
  free(c->cur_serial);       c->cur_serial       = NULL;
  free(c->cur_revision_str); c->cur_revision_str = NULL;
  c->cur_revision = 0;
}

static bool tracks_cb(const yaml_event_t* ev, void* user)
{
  ts_ctx_t* c = (ts_ctx_t*)user;
  if (c->oom) return false;

  /* In skip mode: shadow nested containers; pop on the matching close. */
  ts_state_t st = ts_cur(c);
  if (st == TS_SKIP) {
    if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      c->skip_depth++;
    } else if (ev->kind == YAML_EVENT_MAP_END || ev->kind == YAML_EVENT_SEQ_END) {
      if (c->skip_depth == 0) {
        ts_pop(c);   /* end of skipped container */
      } else {
        c->skip_depth--;
      }
    }
    return true;
  }

  switch (st) {

  case TS_TOP_MAP:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) ts_set(c, TS_TOP_KEY);
    return true;

  case TS_TOP_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      reset_entry(c);
      c->cur_serial = dup_slice(ev->str, ev->len);
      if (!c->cur_serial) { c->oom = true; return false; }
      ts_set(c, TS_ENTRY_AWAIT_BODY);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      ts_pop(c);  /* leave top-level map */
    }
    return true;

  case TS_ENTRY_AWAIT_BODY:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      ts_set(c, TS_TOP_KEY);   /* return here after entry body closes */
      ts_push(c, TS_ENTRY_KEY);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      /* malformed: top-level value scalar; back to expecting next KEY */
      ts_set(c, TS_TOP_KEY);
    }
    return true;

  case TS_ENTRY_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      if (ev->len == 9 && memcmp(ev->str, "trackData", 9) == 0) {
        ts_set(c, TS_ENTRY_VAL_TRACKDATA);
      } else {
        ts_set(c, TS_ENTRY_VAL_UNKNOWN);
      }
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      /* Entry body closed. */
      c->serials_count++;
      ts_pop(c);   /* back to TS_TOP_KEY */
    }
    return true;

  case TS_ENTRY_VAL_TRACKDATA:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      c->cur_revision = 0;
      ts_set(c, TS_ENTRY_KEY);   /* return here when the seq closes */
      ts_push(c, TS_TRACKDATA_SEQ);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      ts_set(c, TS_ENTRY_KEY);
    }
    return true;

  case TS_ENTRY_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) {
      ts_set(c, TS_ENTRY_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_ENTRY_KEY);
      ts_push(c, TS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case TS_TRACKDATA_SEQ:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      free(c->cur_revision_str);
      c->cur_revision_str = NULL;
      ts_push(c, TS_REVISION_KEY);
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      ts_pop(c);  /* back to TS_ENTRY_KEY */
    }
    return true;

  case TS_REVISION_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      if (ev->len == 7 && memcmp(ev->str, "version", 7) == 0) {
        ts_set(c, TS_REVISION_VAL_VERSION);
      } else if (ev->len == 6 && memcmp(ev->str, "tracks", 6) == 0) {
        ts_set(c, TS_REVISION_VAL_TRACKS);
      } else {
        ts_set(c, TS_REVISION_VAL_UNKNOWN);
      }
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      /* Revision item closed. */
      c->cur_revision++;
      free(c->cur_revision_str); c->cur_revision_str = NULL;
      ts_pop(c);  /* back to TS_TRACKDATA_SEQ */
    }
    return true;

  case TS_REVISION_VAL_VERSION:
    if (ev->kind == YAML_EVENT_SCALAR) {
      free(c->cur_revision_str);
      c->cur_revision_str = dup_slice(ev->str, ev->len);
      if (!c->cur_revision_str) { c->oom = true; return false; }
      ts_set(c, TS_REVISION_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_REVISION_KEY);
      ts_push(c, TS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case TS_REVISION_VAL_TRACKS:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_REVISION_KEY);  /* return here when the seq closes */
      ts_push(c, TS_TRACKS_SEQ);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      ts_set(c, TS_REVISION_KEY);
    }
    return true;

  case TS_REVISION_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) {
      ts_set(c, TS_REVISION_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_REVISION_KEY);
      ts_push(c, TS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case TS_TRACKS_SEQ:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      ts_push(c, TS_TRACK_KEY);
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      ts_pop(c);  /* back to TS_REVISION_KEY */
    }
    return true;

  case TS_TRACK_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      if (ev->len == 3 && memcmp(ev->str, "md5", 3) == 0) {
        ts_set(c, TS_TRACK_VAL_MD5);
      } else {
        ts_set(c, TS_TRACK_VAL_UNKNOWN);
      }
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      ts_pop(c);  /* back to TS_TRACKS_SEQ */
    }
    return true;

  case TS_TRACK_VAL_MD5:
    if (ev->kind == YAML_EVENT_SCALAR) {
      u8 md5[16];
      if (cd_image_hash_from_string(ev->str, (u32)ev->len, md5)) {
        if (!maybe_grow()) { c->oom = true; return false; }

        game_database_track_data_t data;
        data.serial       = c->cur_serial
                            ? dup_slice(c->cur_serial, strlen(c->cur_serial)) 
                            : dup_slice("", 0);
        data.revision_str = c->cur_revision_str
                            ? dup_slice(c->cur_revision_str, strlen(c->cur_revision_str)) 
                            : dup_slice("", 0);
        data.revision     = c->cur_revision;
        if (!data.serial || !data.revision_str) {
          free(data.serial); free(data.revision_str);
          c->oom = true;
          return false;
        }
        if (!table_insert(md5, data)) {
          free(data.serial); free(data.revision_str);
          ERROR_LOG("track-hash table insert failed (full?)");
          c->oom = true;
          return false;
        }
      } else {
        WARNING_LOG("invalid md5 in %s", c->cur_serial ? c->cur_serial : "(null)");
      }
      ts_set(c, TS_TRACK_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_TRACK_KEY);
      ts_push(c, TS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case TS_TRACK_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) {
      ts_set(c, TS_TRACK_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ts_set(c, TS_TRACK_KEY);
      ts_push(c, TS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case TS_SKIP:
    /* handled at top of function */
    return true;
  }

  return true;
}

/* Loader                                                                  */

static bool load_track_hashes(void)
{
  Error err = ERROR_INIT;
  timer_t_ load_timer;
  timer_init(&load_timer);

  u8* yaml_buf = NULL;
  size_t yaml_buf_len = 0;
  if (!fs_read_binary_file_path(DISCDB_YAML_PATH, &yaml_buf, &yaml_buf_len, &err)) {
    ERROR_LOG("Failed to read disc database: %s", Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }

  if (!table_alloc(INITIAL_BUCKET_COUNT)) {
    free(yaml_buf);
    return false;
  }

  ts_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.state_stack[0] = TS_TOP_MAP;
  ctx.state_top      = 0;

  yaml_parse_error_t perr;
  bool ok = yaml_parse((const char*)yaml_buf, yaml_buf_len, tracks_cb, &ctx, &perr);

  reset_entry(&ctx);
  free(yaml_buf);

  if (!ok) {
    ERROR_LOG("discdb.yaml parse error at line %u: %s", perr.line,
              perr.message ? perr.message : "(null)");
    return false;
  }
  if (ctx.oom) {
    ERROR_LOG("OOM while loading track hashes");
    return false;
  }

  const double ms = timer_get_milliseconds(&load_timer);
  INFO_LOG("Loaded %u track hashes from %u serials in %.0f ms.",
           (unsigned)s_tracks.entries_count, (unsigned)ctx.serials_count, ms);
  return s_tracks.entries_count > 0;
}

/* Public API                                                              */

void game_database_ensure_track_hashes_loaded(void)
{
  if (s_tracks.loaded) return;
  s_tracks.loaded = true;  /* set first so a parse failure doesn't loop */
  load_track_hashes();
}

const game_database_track_data_t* game_database_lookup_track_hash(const u8 md5[16])
{
  game_database_ensure_track_hashes_loaded();
  if (!s_tracks.buckets) return NULL;

  u64 h;
  memcpy(&h, md5, sizeof(h));
  const u32 mask = s_tracks.bucket_count - 1u;
  u32 i = (u32)(h & mask);
  for (u32 probe = 0; probe < s_tracks.bucket_count; probe++) {
    const track_bucket_t* b = &s_tracks.buckets[i];
    if (!b->occupied) return NULL;
    if (memcmp(b->md5, md5, 16) == 0) return &b->data;
    i = (i + 1u) & mask;
  }
  return NULL;
}

/* Test accessors (not in public header)                                   */

u32 game_database_test_get_track_hash_count(void);
u32 game_database_test_get_track_hash_max_probe(void);
u32 game_database_test_get_track_hash_bucket_count(void);
void game_database_test_reset_track_hashes(void);

u32 game_database_test_get_track_hash_count(void)        { return s_tracks.entries_count; }
u32 game_database_test_get_track_hash_max_probe(void)    { return s_tracks.max_probe; }
u32 game_database_test_get_track_hash_bucket_count(void) { return s_tracks.bucket_count; }

void game_database_test_reset_track_hashes(void)
{
  if (s_tracks.buckets) {
    for (u32 i = 0; i < s_tracks.bucket_count; i++) {
      if (s_tracks.buckets[i].occupied) {
        free(s_tracks.buckets[i].data.serial);
        free(s_tracks.buckets[i].data.revision_str);
      }
    }
    free(s_tracks.buckets);
  }
  memset(&s_tracks, 0, sizeof(s_tracks));
}
