/*
 * GameDatabase: name tables, state, YAML loader, lookups.
 *
 * The YAML loader walks the gamedb.yaml + discsets.yaml resource files via
 * util/yaml_parser.c and populates `s_state.entries`, `s_state.disc_sets`,
 * and `s_state.codes`.  Strings in entries/disc_sets are slices into the
 * owned `db_data` / `discsets_data` buffers, NOT copies; the buffers live
 * for the process lifetime.
 *
 * Binary cache, ApplySettings, TrackHashes, and GetSerialForDisc lookups
 * land in subsequent subagents and remain stubs in this file.
 */

#include "core/game_database.h"

#include "core/controller.h"
#include "core/settings.h"

#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/timer.h"

#include "util/cd_image.h"
#include "util/iso_reader.h"
#include "util/yaml_parser.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Pinned binary header so the binary cache file is byte-for-byte
 * interoperable. */
#define GAME_DATABASE_CACHE_SIGNATURE 0x45434C48u
#define GAME_DATABASE_CACHE_VERSION   33u

/* Resource paths (used both for the YAML loader and for mtime computation
   inside the cache header). */
#define GAMEDB_YAML_PATH    "data/resources/gamedb.yaml"
#define DISCSETS_YAML_PATH  "data/resources/discsets.yaml"
#define GAMEDB_CACHE_PATH   "gamedb.cache"

LOG_CHANNEL(GameDatabase);

/* Name tables                                                            */

static const char* const s_compatibility_rating_names[GAME_DATABASE_COMPAT_RATING_COUNT] = {
  "Unknown",
  "DoesntBoot",
  "CrashesInIntro",
  "CrashesInGame",
  "GraphicalAudioIssues",
  "NoIssues", 
};

static const char* const s_compatibility_rating_display_names[GAME_DATABASE_COMPAT_RATING_COUNT] = {
  "Unknown",
  "Doesn't Boot",
  "Crashes In Intro",
  "Crashes In-Game",
  "Graphical/Audio Issues",
  "No Issues", 
};

/* IMPORTANT: index i of s_trait_names MUST match enum value
   GAME_DATABASE_TRAIT_* for index i (game_database.h enum order).
   Fourth entry is "ForceRoundTextureCoordinates" (the YAML spelling),
   spelling); see header comment on the enum. */
static const char* const s_trait_names[GAME_DATABASE_TRAIT_COUNT] = {
  "ForceInterpreter",
  "ForceSoftwareRenderer",
  "ForceSoftwareRendererForReadbacks",
  "ForceRoundTextureCoordinates",
  "ForceShaderBlending",
  "ForceFullTrueColor",
  "ForceDeinterlacing",
  "ForceFullBoot",
  "DisableAutoAnalogMode",
  "DisableMultitap",
  "DisableFastForwardMemoryCardAccess",
  "DisableCDROMReadSpeedup",
  "DisableCDROMSeekSpeedup",
  "DisableCDROMSpeedupOnMDEC",
  "DisableTrueColor",
  "DisableFullTrueColor",
  "DisableUpscaling",
  "DisableTextureFiltering",
  "DisableSpriteTextureFiltering",
  "DisableScaledDithering",
  "DisableScaledInterlacing",
  "DisableAllBordersCrop",
  "DisableWidescreen",
  "DisablePGXP",
  "DisablePGXPCulling",
  "DisablePGXPTextureCorrection",
  "DisablePGXPColorCorrection",
  "DisablePGXPDepthBuffer",
  "DisablePGXPOn2DPolygons",
  "ForcePGXPVertexCache",
  "ForcePGXPCPUMode",
  "ForceRecompilerICache",
  "ForceCDROMSubQSkew",
  "IsLibCryptProtected", 
};

static const char* const s_trait_display_names[GAME_DATABASE_TRAIT_COUNT] = {
  "Force Interpreter",
  "Force Software Renderer",
  "Force Software Renderer For Readbacks",
  "Force Round Texture Coordinates",
  "Force Shader Blending",
  "Force Full True Color",
  "Force Deinterlacing",
  "Force Full Boot",
  "Disable Automatic Analog Mode",
  "Disable Multitap",
  "Disable Fast Forward Memory Card Access",
  "Disable CD-ROM Read Speedup",
  "Disable CD-ROM Seek Speedup",
  "Disable CD-ROM Speedup on MDEC",
  "Disable True Color",
  "Disable Full True Color",
  "Disable Upscaling",
  "Disable Texture Filtering",
  "Disable Sprite Texture Filtering",
  "Disable Scaled Dithering",
  "Disable Scaled Interlacing",
  "Disable All Borders Crop",
  "Disable Widescreen",
  "Disable PGXP",
  "Disable PGXP Culling",
  "Disable PGXP Texture Correction",
  "Disable PGXP Color Correction",
  "Disable PGXP Depth Buffer",
  "Disable PGXP on 2D Polygons",
  "Force PGXP Vertex Cache",
  "Force PGXP CPU Mode",
  "Force Recompiler ICache",
  "Force CD-ROM SubQ Skew",
  "Is LibCrypt Protected", 
};

static const char* const s_language_names[GAME_DATABASE_LANGUAGE_COUNT] = {
  "Catalan",  "Chinese",    "Czech",   "Danish",  "Dutch",   "English",
  "Finnish",  "French",     "German",  "Greek",   "Hebrew",  "Iranian",
  "Italian",  "Japanese",   "Korean",  "Norwegian",
  "Polish",   "Portuguese", "Russian", "Spanish", "Swedish", "Turkish", 
};

   /* (the language names are already display-friendly).  We mirror them so
   callers have a single accessor parallel to traits / compat ratings. */
static const char* const s_language_display_names[GAME_DATABASE_LANGUAGE_COUNT] = {
  "Catalan",  "Chinese",    "Czech",   "Danish",  "Dutch",   "English",
  "Finnish",  "French",     "German",  "Greek",   "Hebrew",  "Iranian",
  "Italian",  "Japanese",   "Korean",  "Norwegian",
  "Polish",   "Portuguese", "Russian", "Spanish", "Swedish", "Turkish", 
};

/* Module state                                                           */

typedef struct {
  const char* code;        /* slice into db_data, NOT NUL-terminated */
  u16         code_len;
  u32         entry_index; /* index into entries[] */
} code_entry_t;

typedef struct {
  bool loaded;
  bool track_hashes_loaded;

  /* Owned backing buffers.  String fields in entries[]/disc_sets[] point
     into these; do NOT free them while entries[] is alive. */
  u8*    db_data;          size_t db_data_len;
  u8*    discsets_data;    size_t discsets_data_len;
  u8*    discdb_data;      size_t discdb_data_len;

  /* When the binary cache load path is taken, the entire cache file is read
     into this buffer and entries[] / disc_sets[] / codes[] string fields are
     pointers into it (zero-copy).  When the YAML path is taken, this stays
     NULL and strings live in db_data / discsets_data instead. */
  u8*    cache_data;       size_t cache_data_len;

  game_database_entry_t*    entries;
  size_t                    entries_count;
  size_t                    entries_capacity;

  game_database_disc_set_t* disc_sets;
  size_t                    disc_sets_count;
  size_t                    disc_sets_capacity;

  /* Sorted by `code` for bsearch. */
  code_entry_t* codes;
  size_t        codes_count;
  size_t        codes_capacity;

  /* Track-hash storage owned by game_database_tracks.c (filled by a later
     subagent).  Kept here so the lookup helpers can find it. */
  void*  tracks;
  size_t tracks_count;
} state_t;

static state_t s_state;

static bool load_gamedb_yaml(void);

/* Generic helpers                                                        */

static bool slice_eq_cstr(const char* s, size_t n, const char* cstr)
{
  if (!s || !cstr) return false;
  size_t l = strlen(cstr);
  return l == n && memcmp(s, cstr, n) == 0;
}

static int slice_cmp(const char* a, size_t alen, const char* b, size_t blen)
{
  size_t m = (alen < blen) ? alen : blen;
  int r = (m > 0) ? memcmp(a, b, m) : 0;
  if (r) return r;
  if (alen < blen) return -1;
  if (alen > blen) return  1;
  return 0;
}

/* Copy a slice into a stack buffer, NUL-terminate.  Returns false if the
   slice doesn't fit.  Used to call C-string-only library helpers. */
static bool slice_to_cstr(const char* s, size_t n, char* out, size_t out_size)
{
  if (n >= out_size) return false;
  if (n > 0) memcpy(out, s, n);
  out[n] = '\0';
  return true;
}

/* Bool scalar parse, accepting the YAML 1.1 conventional spellings used */
static bool parse_bool_scalar(const char* s, size_t n, bool* out)
{
  if (slice_eq_cstr(s, n, "true")  || slice_eq_cstr(s, n, "True")  ||
      slice_eq_cstr(s, n, "TRUE")  || slice_eq_cstr(s, n, "yes")   ||
      slice_eq_cstr(s, n, "Yes")   || slice_eq_cstr(s, n, "1")) {
    *out = true;  return true;
  }
  if (slice_eq_cstr(s, n, "false") || slice_eq_cstr(s, n, "False") ||
      slice_eq_cstr(s, n, "FALSE") || slice_eq_cstr(s, n, "no")    ||
      slice_eq_cstr(s, n, "No")    || slice_eq_cstr(s, n, "0")) {
    *out = false; return true;
  }
  return false;
}

static bool parse_long_scalar(const char* s, size_t n, long min, long max, long* out)
{
  char buf[32];
  if (!slice_to_cstr(s, n, buf, sizeof(buf))) return false;
  char* endp = NULL;
  long v = strtol(buf, &endp, 10);
  if (!endp || *endp != '\0' || endp == buf) return false;
  if (v < min || v > max) return false;
  *out = v;
  return true;
}

static bool parse_u32_scalar(const char* s, size_t n, u32* out)
{
  long v = 0;
  if (!parse_long_scalar(s, n, 0, 0x7FFFFFFFL, &v)) return false;
  *out = (u32)v;
  return true;
}

static bool parse_u8_scalar(const char* s, size_t n, u8* out)
{
  long v = 0;
  if (!parse_long_scalar(s, n, 0, 255, &v)) return false;
  *out = (u8)v;
  return true;
}

static bool parse_s16_scalar(const char* s, size_t n, s16* out)
{
  long v = 0;
  if (!parse_long_scalar(s, n, -32768, 32767, &v)) return false;
  *out = (s16)v;
  return true;
}

static bool parse_s8_scalar(const char* s, size_t n, s8* out)
{
  long v = 0;
  if (!parse_long_scalar(s, n, -128, 127, &v)) return false;
  *out = (s8)v;
  return true;
}

static bool parse_float_scalar(const char* s, size_t n, float* out)
{
  char buf[64];
  if (!slice_to_cstr(s, n, buf, sizeof(buf))) return false;
  char* endp = NULL;
  double v = strtod(buf, &endp);
  if (!endp || *endp != '\0' || endp == buf) return false;
  *out = (float)v;
  return true;
}

/* "YYYY-MM-DD" -> u64 unix seconds (UTC).  Returns true on success. */
static bool parse_release_date(const char* s, size_t n, u64* out)
{
  if (n != 10) return false;
  if (s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < 10; i++) {
    if (i == 4 || i == 7) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
  int m = (s[5]-'0')*10   + (s[6]-'0');
  int d = (s[8]-'0')*10   + (s[9]-'0');
  if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon  = m - 1;
  t.tm_mday = d;
  t.tm_isdst = 0;
  time_t v = timegm(&t);
  if (v == (time_t)-1) return false;
  *out = (u64)v;
  return true;
}

/* Linear-scan name tables. */
static int lookup_trait_by_name(const char* s, size_t n)
{
  for (u32 i = 0; i < (u32)GAME_DATABASE_TRAIT_COUNT; i++) {
    if (slice_eq_cstr(s, n, s_trait_names[i])) return (int)i;
  }
  return -1;
}

static int lookup_language_by_name(const char* s, size_t n)
{
  for (u32 i = 0; i < (u32)GAME_DATABASE_LANGUAGE_COUNT; i++) {
    if (slice_eq_cstr(s, n, s_language_names[i])) return (int)i;
  }
  return -1;
}

static int lookup_compat_rating_by_name(const char* s, size_t n)
{
  for (u32 i = 0; i < (u32)GAME_DATABASE_COMPAT_RATING_COUNT; i++) {
    if (slice_eq_cstr(s, n, s_compatibility_rating_names[i])) return (int)i;
  }
  return -1;
}

/* Growable arrays                                                        */

static bool grow_entries(void)
{
  if (s_state.entries_count < s_state.entries_capacity) return true;
  size_t newcap = s_state.entries_capacity ? s_state.entries_capacity * 2 : 1024;
  void* p = realloc(s_state.entries, newcap * sizeof(*s_state.entries));
  if (!p) return false;
  s_state.entries          = (game_database_entry_t*)p;
  s_state.entries_capacity = newcap;
  return true;
}

static bool grow_disc_sets(void)
{
  if (s_state.disc_sets_count < s_state.disc_sets_capacity) return true;
  size_t newcap = s_state.disc_sets_capacity ? s_state.disc_sets_capacity * 2 : 256;
  void* p = realloc(s_state.disc_sets, newcap * sizeof(*s_state.disc_sets));
  if (!p) return false;
  s_state.disc_sets          = (game_database_disc_set_t*)p;
  s_state.disc_sets_capacity = newcap;
  return true;
}

static bool grow_codes(void)
{
  if (s_state.codes_count < s_state.codes_capacity) return true;
  size_t newcap = s_state.codes_capacity ? s_state.codes_capacity * 2 : 1024;
  void* p = realloc(s_state.codes, newcap * sizeof(*s_state.codes));
  if (!p) return false;
  s_state.codes          = (code_entry_t*)p;
  s_state.codes_capacity = newcap;
  return true;
}

/* gamedb.yaml state machine                                              */
/*
 * The yaml_parser hands us KEY/SCALAR/MAP_BEGIN/SEQ_BEGIN/MAP_END/SEQ_END
 * events.  We track the current logical position via a depth counter and a
 * small `path` of "what are we waiting on next?" states.
 *
 * Top-level shape (gamedb.yaml):
 *   MAP_BEGIN                 depth 0 -> 1
 *     KEY <serial>            (start a new entry)
 *     MAP_BEGIN               depth 1 -> 2 (entry body)
 *       KEY <field>           dispatched by field name
 *       SCALAR or MAP_BEGIN or SEQ_BEGIN according to field
 *       ...
 *     MAP_END                 depth 2 -> 1
 *     KEY <next serial>
 *     ...
 *   MAP_END                   depth 1 -> 0
 *
 * State enum encodes "what KEY context am I in?" so SCALAR/MAP_BEGIN/SEQ_BEGIN
 * can dispatch on the recently-parsed key. */

typedef enum {
  /* Initial state at the top of gamedb.yaml. */
  GS_TOP_MAP,                /* expecting top-level MAP_BEGIN */
  GS_TOP_KEY,                /* in top-level map, expecting KEY (serial) */
  GS_ENTRY_AWAIT_BODY,       /* after top-level KEY, expecting MAP_BEGIN */
  GS_ENTRY_KEY,              /* in entry body, expecting KEY (field name) */

  /* Sub-keys: after seeing KEY <X>, we expect SCALAR / MAP_BEGIN / SEQ_BEGIN. */
  GS_ENTRY_VAL_NAME,
  GS_ENTRY_VAL_SORTNAME,
  GS_ENTRY_VAL_LOCALIZED,
  GS_ENTRY_VAL_SAVENAME,
  GS_ENTRY_VAL_LIBCRYPT,
  GS_ENTRY_VAL_CONTROLLERS,  /* expects SEQ_BEGIN */
  GS_ENTRY_VAL_TRAITS,       /* expects SEQ_BEGIN */
  GS_ENTRY_VAL_CODES,        /* expects SEQ_BEGIN */
  GS_ENTRY_VAL_COMPAT,       /* expects MAP_BEGIN */
  GS_ENTRY_VAL_METADATA,     /* expects MAP_BEGIN */
  GS_ENTRY_VAL_SETTINGS,     /* expects MAP_BEGIN */
  GS_ENTRY_VAL_UNKNOWN,      /* skip one value or container */

  /* Inside controllers/traits/codes seq: each SCALAR is a list item. */
  GS_IN_CONTROLLERS,
  GS_IN_TRAITS,
  GS_IN_CODES,
  /* Skipping nested container (depth-counted). */
  GS_SKIP,

  /* compatibility submap. */
  GS_COMPAT_KEY,
  GS_COMPAT_VAL_RATING,
  GS_COMPAT_VAL_VERSION,
  GS_COMPAT_VAL_COMMENTS,
  GS_COMPAT_VAL_UNKNOWN,

  /* metadata submap. */
  GS_META_KEY,
  GS_META_VAL_GENRE,
  GS_META_VAL_DEVELOPER,
  GS_META_VAL_PUBLISHER,
  GS_META_VAL_RELEASEDATE,
  GS_META_VAL_MINPLAYERS,
  GS_META_VAL_MAXPLAYERS,
  GS_META_VAL_MINBLOCKS,
  GS_META_VAL_MAXBLOCKS,
  GS_META_VAL_LANGUAGES,
  GS_META_VAL_MULTITAP,
  GS_META_VAL_VIBRATION,
  GS_META_VAL_LINKCABLE,
  GS_META_VAL_UNKNOWN,
  GS_IN_LANGUAGES,

  /* settings submap. */
  GS_SETTINGS_KEY,
  GS_SETTINGS_VAL,           /* generic: dispatch via remembered field name */
  GS_SETTINGS_VAL_UNKNOWN,
} gs_state_t;

/* Settings sub-key identifier (separate so multi-line state stack stays small). */
typedef enum {
  SF_NONE,
  SF_DISPLAY_ACTIVE_START_OFFSET,
  SF_DISPLAY_ACTIVE_END_OFFSET,
  SF_DISPLAY_LINE_START_OFFSET,
  SF_DISPLAY_LINE_END_OFFSET,
  SF_DISPLAY_CROP_MODE,
  SF_DISPLAY_DEINTERLACING_MODE,
  SF_GPU_LINE_DETECT_MODE,
  SF_CPU_OVERCLOCK_PERCENT,
  SF_DMA_MAX_SLICE_TICKS,
  SF_DMA_HALT_TICKS,
  SF_CDROM_MAX_SEEK_SPEEDUP_CYCLES,
  SF_CDROM_MAX_READ_SPEEDUP_CYCLES,
  SF_GPU_FIFO_SIZE,
  SF_GPU_MAX_RUN_AHEAD,
  SF_GPU_PGXP_TOLERANCE,
  SF_GPU_PGXP_DEPTH_THRESHOLD,
  SF_GPU_PGXP_PRESERVE_PROJ_FP,
} settings_field_t;

#define MAX_GS_DEPTH 16

typedef struct {
  /* Stack of states (state at index i is the state we transition back to
     when the matching MAP_END / SEQ_END pops at this depth). */
  gs_state_t  state_stack[MAX_GS_DEPTH];
  u32         state_top;     /* index of current state */

  /* Logical depth: incremented on MAP_BEGIN/SEQ_BEGIN, decremented on END. */
  u32         depth;

  /* Skip-mode: when entering an unknown container, count nested containers
   * so we know when to stop skipping. */
  u32         skip_depth;

  /* Currently-being-built entry (s_state.entries[entries_count - 1]). */
  game_database_entry_t* cur_entry;
  bool                   controllers_first; /* first controller seen for this entry */

  /* Current settings field being parsed. */
  settings_field_t cur_setting;

  /* Bool flag: did the current entry have a `codes:` block at all? */
  bool entry_has_codes_block;
  u32  entry_codes_added;   /* count of codes added in current `codes:` */

  /* Owned-buffer base/end for slice validation (not strictly required). */
  const char* buf_base;
  const char* buf_end;
} gs_ctx_t;

static void gs_push(gs_ctx_t* c, gs_state_t s)
{
  if (c->state_top + 1 >= MAX_GS_DEPTH) {
    ERROR_LOG("game_database state stack overflow");
    return;
  }
  c->state_top++;
  c->state_stack[c->state_top] = s;
}

static void gs_pop(gs_ctx_t* c)
{
  if (c->state_top > 0) c->state_top--;
}

static gs_state_t gs_cur(const gs_ctx_t* c) { return c->state_stack[c->state_top]; }

static void gs_set(gs_ctx_t* c, gs_state_t s) { c->state_stack[c->state_top] = s; }

/* Begin a new entry from the just-emitted KEY <serial>. */
static bool gs_begin_entry(gs_ctx_t* c, const char* serial, size_t serial_len)
{
  if (!grow_entries()) return false;
  game_database_entry_t* e = &s_state.entries[s_state.entries_count];
  memset(e, 0, sizeof(*e));
  e->serial                = serial;
  e->serial_len            = (u32)serial_len;
  e->supported_controllers = (u16)~0u;
  s_state.entries_count++;
  c->cur_entry              = e;
  c->controllers_first      = true;
  c->entry_has_codes_block  = false;
  c->entry_codes_added      = 0;
  c->cur_setting            = SF_NONE;
  return true;
}

/* Add (code, current_entry) to s_state.codes.  Skips dups (linear here, but
   dedup happens lazily during the post-load index build via warning). */
static bool gs_add_code(const char* code, size_t code_len)
{
  if (code_len == 0) return false;
  if (!grow_codes()) return false;
  code_entry_t* e = &s_state.codes[s_state.codes_count];
  e->code        = code;
  e->code_len    = (u16)code_len;
  e->entry_index = (u32)(s_state.entries_count - 1); /* pre-sort index; remapped later */
  s_state.codes_count++;
  return true;
}

/* Dispatch a settings KEY name to a settings_field_t.  Returns SF_NONE if
 * unknown (unknown fields are skipped). */
static settings_field_t classify_settings_key(const char* k, size_t n)
{
  if (slice_eq_cstr(k, n, "displayActiveStartOffset"))   return SF_DISPLAY_ACTIVE_START_OFFSET;
  if (slice_eq_cstr(k, n, "displayActiveEndOffset"))     return SF_DISPLAY_ACTIVE_END_OFFSET;
  if (slice_eq_cstr(k, n, "displayLineStartOffset"))     return SF_DISPLAY_LINE_START_OFFSET;
  if (slice_eq_cstr(k, n, "displayLineEndOffset"))       return SF_DISPLAY_LINE_END_OFFSET;
  if (slice_eq_cstr(k, n, "displayCropMode"))            return SF_DISPLAY_CROP_MODE;
  if (slice_eq_cstr(k, n, "displayDeinterlacingMode"))   return SF_DISPLAY_DEINTERLACING_MODE;
  if (slice_eq_cstr(k, n, "gpuLineDetectMode"))          return SF_GPU_LINE_DETECT_MODE;
  if (slice_eq_cstr(k, n, "cpuOverclockPercent"))        return SF_CPU_OVERCLOCK_PERCENT;
  if (slice_eq_cstr(k, n, "dmaMaxSliceTicks"))           return SF_DMA_MAX_SLICE_TICKS;
  if (slice_eq_cstr(k, n, "dmaHaltTicks"))               return SF_DMA_HALT_TICKS;
  if (slice_eq_cstr(k, n, "cdromMaxSeekSpeedupCycles"))  return SF_CDROM_MAX_SEEK_SPEEDUP_CYCLES;
  if (slice_eq_cstr(k, n, "cdromMaxReadSpeedupCycles"))  return SF_CDROM_MAX_READ_SPEEDUP_CYCLES;
  if (slice_eq_cstr(k, n, "gpuFIFOSize"))                return SF_GPU_FIFO_SIZE;
  if (slice_eq_cstr(k, n, "gpuMaxRunAhead"))             return SF_GPU_MAX_RUN_AHEAD;
  if (slice_eq_cstr(k, n, "gpuPGXPTolerance"))           return SF_GPU_PGXP_TOLERANCE;
  if (slice_eq_cstr(k, n, "gpuPGXPDepthThreshold"))      return SF_GPU_PGXP_DEPTH_THRESHOLD;
  if (slice_eq_cstr(k, n, "gpuPGXPPreserveProjFP"))      return SF_GPU_PGXP_PRESERVE_PROJ_FP;
  return SF_NONE;
}

/* Apply a settings SCALAR value using c->cur_setting. */
static void apply_settings_value(gs_ctx_t* c, const char* s, size_t n)
{
  game_database_entry_t* e = c->cur_entry;
  if (!e) return;
  char buf[64];

  switch (c->cur_setting) {
    case SF_DISPLAY_ACTIVE_START_OFFSET:
      if (parse_s16_scalar(s, n, &e->display_active_start_offset))
        e->has_display_active_start_offset = true;
      break;
    case SF_DISPLAY_ACTIVE_END_OFFSET:
      if (parse_s16_scalar(s, n, &e->display_active_end_offset))
        e->has_display_active_end_offset = true;
      break;
    case SF_DISPLAY_LINE_START_OFFSET:
      if (parse_s8_scalar(s, n, &e->display_line_start_offset))
        e->has_display_line_start_offset = true;
      break;
    case SF_DISPLAY_LINE_END_OFFSET:
      if (parse_s8_scalar(s, n, &e->display_line_end_offset))
        e->has_display_line_end_offset = true;
      break;
    case SF_DISPLAY_CROP_MODE:
      if (slice_to_cstr(s, n, buf, sizeof(buf)) &&
          settings_parse_display_crop_mode_name(buf, &e->display_crop_mode))
        e->has_display_crop_mode = true;
      break;
    case SF_DISPLAY_DEINTERLACING_MODE:
      if (slice_to_cstr(s, n, buf, sizeof(buf)) &&
          settings_parse_display_deinterlacing_mode_name(buf, &e->display_deinterlacing_mode))
        e->has_display_deinterlacing_mode = true;
      break;
    case SF_GPU_LINE_DETECT_MODE:
      if (slice_to_cstr(s, n, buf, sizeof(buf)) &&
          settings_parse_line_detect_mode_name(buf, &e->gpu_line_detect_mode))
        e->has_gpu_line_detect_mode = true;
      break;
    case SF_CPU_OVERCLOCK_PERCENT:
      if (parse_u8_scalar(s, n, &e->cpu_overclock))
        e->has_cpu_overclock = true;
      break;
    case SF_DMA_MAX_SLICE_TICKS:
      if (parse_u32_scalar(s, n, &e->dma_max_slice_ticks))
        e->has_dma_max_slice_ticks = true;
      break;
    case SF_DMA_HALT_TICKS:
      if (parse_u32_scalar(s, n, &e->dma_halt_ticks))
        e->has_dma_halt_ticks = true;
      break;
    case SF_CDROM_MAX_SEEK_SPEEDUP_CYCLES:
      if (parse_u32_scalar(s, n, &e->cdrom_max_seek_speedup_cycles))
        e->has_cdrom_max_seek_speedup_cycles = true;
      break;
    case SF_CDROM_MAX_READ_SPEEDUP_CYCLES:
      if (parse_u32_scalar(s, n, &e->cdrom_max_read_speedup_cycles))
        e->has_cdrom_max_read_speedup_cycles = true;
      break;
    case SF_GPU_FIFO_SIZE:
      if (parse_u32_scalar(s, n, &e->gpu_fifo_size))
        e->has_gpu_fifo_size = true;
      break;
    case SF_GPU_MAX_RUN_AHEAD:
      if (parse_u32_scalar(s, n, &e->gpu_max_run_ahead))
        e->has_gpu_max_run_ahead = true;
      break;
    case SF_GPU_PGXP_TOLERANCE:
      if (parse_float_scalar(s, n, &e->gpu_pgxp_tolerance))
        e->has_gpu_pgxp_tolerance = true;
      break;
    case SF_GPU_PGXP_DEPTH_THRESHOLD:
      if (parse_float_scalar(s, n, &e->gpu_pgxp_depth_threshold))
        e->has_gpu_pgxp_depth_threshold = true;
      break;
    case SF_GPU_PGXP_PRESERVE_PROJ_FP: {
      bool b;
      if (parse_bool_scalar(s, n, &b)) {
        e->gpu_pgxp_preserve_proj_fp     = b;
        e->has_gpu_pgxp_preserve_proj_fp = true;
      }
      break;
    }
    case SF_NONE:
      /* unknown / unsupported; ignore */
      break;
  }
  c->cur_setting = SF_NONE;
}

/* The single yaml_parser callback for gamedb.yaml. */
static bool gamedb_cb(const yaml_event_t* ev, void* user)
{
  gs_ctx_t* c = (gs_ctx_t*)user;

  /* Track depth changes. */
  if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) c->depth++;
  else if (ev->kind == YAML_EVENT_MAP_END   || ev->kind == YAML_EVENT_SEQ_END) c->depth--;

  /* If we're skipping an unknown container, just shadow events. */
  gs_state_t st = gs_cur(c);
  if (st == GS_SKIP) {
    if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) c->skip_depth++;
    else if (ev->kind == YAML_EVENT_MAP_END || ev->kind == YAML_EVENT_SEQ_END) {
      if (c->skip_depth == 0) {
        gs_pop(c);   /* end of skipped container */
      } else {
        c->skip_depth--;
      }
    }
    return true;
  }

  switch (st) {

  case GS_TOP_MAP:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) { gs_set(c, GS_TOP_KEY); }
    return true;

  case GS_TOP_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      if (!gs_begin_entry(c, ev->str, ev->len)) return false;
      gs_set(c, GS_ENTRY_AWAIT_BODY);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      gs_pop(c); /* leave top-level */
    }
    return true;

  case GS_ENTRY_AWAIT_BODY:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      gs_set(c, GS_TOP_KEY);   /* return here after entry body is closed */
      gs_push(c, GS_ENTRY_KEY);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      /* malformed: top-level scalar value; back to expecting next KEY */
      gs_set(c, GS_TOP_KEY);
    }
    return true;

  case GS_ENTRY_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      const char* k = ev->str;
      const size_t n = ev->len;
      if      (slice_eq_cstr(k, n, "name"))           gs_set(c, GS_ENTRY_VAL_NAME);
      else if (slice_eq_cstr(k, n, "sortName"))       gs_set(c, GS_ENTRY_VAL_SORTNAME);
      else if (slice_eq_cstr(k, n, "localizedName"))  gs_set(c, GS_ENTRY_VAL_LOCALIZED);
      else if (slice_eq_cstr(k, n, "saveName"))       gs_set(c, GS_ENTRY_VAL_SAVENAME);
      else if (slice_eq_cstr(k, n, "libcrypt"))       gs_set(c, GS_ENTRY_VAL_LIBCRYPT);
      else if (slice_eq_cstr(k, n, "controllers"))    gs_set(c, GS_ENTRY_VAL_CONTROLLERS);
      else if (slice_eq_cstr(k, n, "traits"))         gs_set(c, GS_ENTRY_VAL_TRAITS);
      else if (slice_eq_cstr(k, n, "codes"))          { gs_set(c, GS_ENTRY_VAL_CODES); c->entry_has_codes_block = true; }
      else if (slice_eq_cstr(k, n, "compatibility"))  gs_set(c, GS_ENTRY_VAL_COMPAT);
      else if (slice_eq_cstr(k, n, "metadata"))       gs_set(c, GS_ENTRY_VAL_METADATA);
      else if (slice_eq_cstr(k, n, "settings"))       gs_set(c, GS_ENTRY_VAL_SETTINGS);
      else                                            gs_set(c, GS_ENTRY_VAL_UNKNOWN);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      /* Entry body closed; if no `codes:` was present, register the */
      if (!c->entry_has_codes_block && c->cur_entry) {
        gs_add_code(c->cur_entry->serial, c->cur_entry->serial_len);
      }
      c->cur_entry = NULL;
      gs_pop(c); /* back to GS_TOP_KEY */
    }
    return true;

  case GS_ENTRY_VAL_NAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->title     = ev->str;
      c->cur_entry->title_len = (u32)ev->len;
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_SORTNAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->sort_title     = ev->str;
      c->cur_entry->sort_title_len = (u32)ev->len;
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_LOCALIZED:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->localized_title     = ev->str;
      c->cur_entry->localized_title_len = (u32)ev->len;
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_SAVENAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->save_title     = ev->str;
      c->cur_entry->save_title_len = (u32)ev->len;
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_LIBCRYPT:
    if (ev->kind == YAML_EVENT_SCALAR) {
      bool b = false;
      if (parse_bool_scalar(ev->str, ev->len, &b) && b) {
        const u32 i = (u32)GAME_DATABASE_TRAIT_IS_LIBCRYPT_PROTECTED;
        c->cur_entry->trait_bits[i >> 3] |= (u8)(1u << (i & 7u));
      }
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;

  case GS_ENTRY_VAL_CONTROLLERS:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_IN_CONTROLLERS);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      /* Malformed inline value; ignore. */
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;

  case GS_ENTRY_VAL_TRAITS:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_IN_TRAITS);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;

  case GS_ENTRY_VAL_CODES:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_IN_CODES);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;

  case GS_ENTRY_VAL_COMPAT:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_COMPAT_KEY);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_METADATA:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_META_KEY);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;
  case GS_ENTRY_VAL_SETTINGS:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_SETTINGS_KEY);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    }
    return true;

  case GS_ENTRY_VAL_UNKNOWN:
    /* Could be SCALAR (just consume) or container (skip). */
    if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_ENTRY_KEY);
    } else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_ENTRY_KEY);
      gs_push(c, GS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case GS_IN_CONTROLLERS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      char buf[64];
      if (slice_to_cstr(ev->str, ev->len, buf, sizeof(buf))) {
        const controller_info_t* ci = controller_get_info_for_name(buf);
        if (ci) {
          if (c->controllers_first) {
            c->cur_entry->supported_controllers = 0;
            c->controllers_first = false;
          }
          c->cur_entry->supported_controllers |= (u16)(1u << (u16)ci->type);
        }
      }
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      gs_pop(c);
    }
    return true;

  case GS_IN_TRAITS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      int idx = lookup_trait_by_name(ev->str, ev->len);
      if (idx >= 0) {
        c->cur_entry->trait_bits[idx >> 3] |= (u8)(1u << (idx & 7));
      }
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      gs_pop(c);
    }
    return true;

  case GS_IN_CODES:
    if (ev->kind == YAML_EVENT_SCALAR) {
      gs_add_code(ev->str, ev->len);
      c->entry_codes_added++;
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      /* If the codes block was empty, fall back to the implicit serial code. */
      if (c->entry_codes_added == 0 && c->cur_entry) {
        gs_add_code(c->cur_entry->serial, c->cur_entry->serial_len);
      }
      gs_pop(c);
    }
    return true;

  case GS_COMPAT_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      if      (slice_eq_cstr(ev->str, ev->len, "rating"))        gs_set(c, GS_COMPAT_VAL_RATING);
      else if (slice_eq_cstr(ev->str, ev->len, "versionTested")) gs_set(c, GS_COMPAT_VAL_VERSION);
      else if (slice_eq_cstr(ev->str, ev->len, "comments"))      gs_set(c, GS_COMPAT_VAL_COMMENTS);
      else                                                       gs_set(c, GS_COMPAT_VAL_UNKNOWN);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      gs_pop(c);
    }
    return true;
  case GS_COMPAT_VAL_RATING:
    if (ev->kind == YAML_EVENT_SCALAR) {
      int idx = lookup_compat_rating_by_name(ev->str, ev->len);
      if (idx >= 0)
        c->cur_entry->compatibility = (game_database_compat_rating_t)idx;
      gs_set(c, GS_COMPAT_KEY);
    }
    return true;
  case GS_COMPAT_VAL_VERSION:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->compatibility_version_tested     = ev->str;
      c->cur_entry->compatibility_version_tested_len = (u32)ev->len;
      gs_set(c, GS_COMPAT_KEY);
    }
    return true;
  case GS_COMPAT_VAL_COMMENTS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->compatibility_comments     = ev->str;
      c->cur_entry->compatibility_comments_len = (u32)ev->len;
      gs_set(c, GS_COMPAT_KEY);
    }
    return true;
  case GS_COMPAT_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) gs_set(c, GS_COMPAT_KEY);
    else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_COMPAT_KEY);
      gs_push(c, GS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case GS_META_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      const char* k = ev->str;
      const size_t n = ev->len;
      if      (slice_eq_cstr(k, n, "genre"))       gs_set(c, GS_META_VAL_GENRE);
      else if (slice_eq_cstr(k, n, "developer"))   gs_set(c, GS_META_VAL_DEVELOPER);
      else if (slice_eq_cstr(k, n, "publisher"))   gs_set(c, GS_META_VAL_PUBLISHER);
      else if (slice_eq_cstr(k, n, "releaseDate")) gs_set(c, GS_META_VAL_RELEASEDATE);
      else if (slice_eq_cstr(k, n, "minPlayers"))  gs_set(c, GS_META_VAL_MINPLAYERS);
      else if (slice_eq_cstr(k, n, "maxPlayers"))  gs_set(c, GS_META_VAL_MAXPLAYERS);
      else if (slice_eq_cstr(k, n, "minBlocks"))   gs_set(c, GS_META_VAL_MINBLOCKS);
      else if (slice_eq_cstr(k, n, "maxBlocks"))   gs_set(c, GS_META_VAL_MAXBLOCKS);
      else if (slice_eq_cstr(k, n, "languages"))   gs_set(c, GS_META_VAL_LANGUAGES);
      else if (slice_eq_cstr(k, n, "multitap"))    gs_set(c, GS_META_VAL_MULTITAP);
      else if (slice_eq_cstr(k, n, "vibration"))   gs_set(c, GS_META_VAL_VIBRATION);
      else if (slice_eq_cstr(k, n, "linkCable"))   gs_set(c, GS_META_VAL_LINKCABLE);
      else                                         gs_set(c, GS_META_VAL_UNKNOWN);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      gs_pop(c);
    }
    return true;
  case GS_META_VAL_GENRE:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->genre     = ev->str;
      c->cur_entry->genre_len = (u32)ev->len;
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_DEVELOPER:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->developer     = ev->str;
      c->cur_entry->developer_len = (u32)ev->len;
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_PUBLISHER:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur_entry->publisher     = ev->str;
      c->cur_entry->publisher_len = (u32)ev->len;
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_RELEASEDATE:
    if (ev->kind == YAML_EVENT_SCALAR) {
      u64 d = 0;
      if (parse_release_date(ev->str, ev->len, &d))
        c->cur_entry->release_date = d;
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_MINPLAYERS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      parse_u8_scalar(ev->str, ev->len, &c->cur_entry->min_players);
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_MAXPLAYERS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      parse_u8_scalar(ev->str, ev->len, &c->cur_entry->max_players);
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_MINBLOCKS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      parse_u8_scalar(ev->str, ev->len, &c->cur_entry->min_blocks);
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_MAXBLOCKS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      parse_u8_scalar(ev->str, ev->len, &c->cur_entry->max_blocks);
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_LANGUAGES:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_META_KEY);
      gs_push(c, GS_IN_LANGUAGES);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_MULTITAP:
    if (ev->kind == YAML_EVENT_SCALAR) {
      bool b = false;
      if (parse_bool_scalar(ev->str, ev->len, &b) && b)
        c->cur_entry->supported_controllers |= GAME_DATABASE_SUPPORTS_MULTITAP_BIT;
      gs_set(c, GS_META_KEY);
    }
    return true;
  case GS_META_VAL_VIBRATION:
  case GS_META_VAL_LINKCABLE:
    if (ev->kind == YAML_EVENT_SCALAR) {
      gs_set(c, GS_META_KEY);  /* consumed; field unused in entry struct */
    }
    return true;
  case GS_META_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) gs_set(c, GS_META_KEY);
    else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_META_KEY);
      gs_push(c, GS_SKIP);
      c->skip_depth = 0;
    }
    return true;
  case GS_IN_LANGUAGES:
    if (ev->kind == YAML_EVENT_SCALAR) {
      int idx = lookup_language_by_name(ev->str, ev->len);
      if (idx >= 0)
        c->cur_entry->language_bits[idx >> 3] |= (u8)(1u << (idx & 7));
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      gs_pop(c);
    }
    return true;

  case GS_SETTINGS_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      c->cur_setting = classify_settings_key(ev->str, ev->len);
      gs_set(c, c->cur_setting == SF_NONE ? GS_SETTINGS_VAL_UNKNOWN : GS_SETTINGS_VAL);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      gs_pop(c);
    }
    return true;
  case GS_SETTINGS_VAL:
    if (ev->kind == YAML_EVENT_SCALAR) {
      apply_settings_value(c, ev->str, ev->len);
      gs_set(c, GS_SETTINGS_KEY);
    }
    return true;
  case GS_SETTINGS_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) gs_set(c, GS_SETTINGS_KEY);
    else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      gs_set(c, GS_SETTINGS_KEY);
      gs_push(c, GS_SKIP);
      c->skip_depth = 0;
    }
    return true;

  case GS_SKIP: /* handled above */
    return true;
  }
  return true;
}

/* discsets.yaml state machine                                            */

typedef enum {
  DS_TOP,                  /* expect SEQ_BEGIN */
  DS_TOP_ITEM_OR_END,      /* in top SEQ, expect MAP_BEGIN or SEQ_END */
  DS_ITEM_KEY,             /* in item map, expect KEY or MAP_END */
  DS_ITEM_VAL_NAME,
  DS_ITEM_VAL_SORTNAME,
  DS_ITEM_VAL_LOCALIZED,
  DS_ITEM_VAL_SAVENAME,
  DS_ITEM_VAL_SERIALS,     /* expect SEQ_BEGIN */
  DS_ITEM_VAL_UNKNOWN,
  DS_IN_SERIALS,
  DS_SKIP,
} ds_state_t;

#define MAX_DS_DEPTH 8

typedef struct {
  ds_state_t state_stack[MAX_DS_DEPTH];
  u32        state_top;
  u32        depth;
  u32        skip_depth;

  game_database_disc_set_t* cur;       /* in-progress disc set */

  /* Growable per-set serials buffer. */
  const char** serials;
  u32*         serial_lens;
  size_t       serials_count;
  size_t       serials_capacity;
} ds_ctx_t;

static void ds_push(ds_ctx_t* c, ds_state_t s)
{
  if (c->state_top + 1 >= MAX_DS_DEPTH) return;
  c->state_top++;
  c->state_stack[c->state_top] = s;
}
static void ds_pop(ds_ctx_t* c) { if (c->state_top > 0) c->state_top--; }
static ds_state_t ds_cur(const ds_ctx_t* c) { return c->state_stack[c->state_top]; }
static void ds_set(ds_ctx_t* c, ds_state_t s) { c->state_stack[c->state_top] = s; }

static bool ds_grow_serials(ds_ctx_t* c)
{
  if (c->serials_count < c->serials_capacity) return true;
  size_t newcap = c->serials_capacity ? c->serials_capacity * 2 : 4;
  void* p1 = realloc(c->serials,     newcap * sizeof(*c->serials));
  void* p2 = realloc(c->serial_lens, newcap * sizeof(*c->serial_lens));
  if (!p1 || !p2) return false;
  c->serials          = (const char**)p1;
  c->serial_lens      = (u32*)p2;
  c->serials_capacity = newcap;
  return true;
}

static bool ds_begin_item(ds_ctx_t* c)
{
  if (!grow_disc_sets()) return false;
  game_database_disc_set_t* d = &s_state.disc_sets[s_state.disc_sets_count];
  memset(d, 0, sizeof(*d));
  s_state.disc_sets_count++;
  c->cur = d;
  c->serials_count = 0;
  return true;
}

static void ds_finalize_item(ds_ctx_t* c)
{
  if (!c->cur) return;
  if (c->serials_count == 0) {

    s_state.disc_sets_count--;
    c->cur = NULL;
    return;
  }
  /* Trim and assign. */
  c->cur->serials       = (const char**)malloc(c->serials_count * sizeof(*c->cur->serials));
  c->cur->serial_lens   = (u32*)malloc(c->serials_count * sizeof(*c->cur->serial_lens));
  if (!c->cur->serials || !c->cur->serial_lens) {
    /* Allocation failed; drop. */
    free(c->cur->serials);     c->cur->serials = NULL;
    free(c->cur->serial_lens); c->cur->serial_lens = NULL;
    s_state.disc_sets_count--;
    c->cur = NULL;
    return;
  }
  memcpy((void*)c->cur->serials, c->serials, c->serials_count * sizeof(*c->serials));
  memcpy(c->cur->serial_lens,    c->serial_lens, c->serials_count * sizeof(*c->serial_lens));
  c->cur->serials_count = (u32)c->serials_count;
  c->cur = NULL;
}

static bool discsets_cb(const yaml_event_t* ev, void* user)
{
  ds_ctx_t* c = (ds_ctx_t*)user;

  if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) c->depth++;
  else if (ev->kind == YAML_EVENT_MAP_END   || ev->kind == YAML_EVENT_SEQ_END) c->depth--;

  ds_state_t st = ds_cur(c);
  if (st == DS_SKIP) {
    if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) c->skip_depth++;
    else if (ev->kind == YAML_EVENT_MAP_END || ev->kind == YAML_EVENT_SEQ_END) {
      if (c->skip_depth == 0) ds_pop(c);
      else                    c->skip_depth--;
    }
    return true;
  }

  switch (st) {
  case DS_TOP:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) ds_set(c, DS_TOP_ITEM_OR_END);
    return true;
  case DS_TOP_ITEM_OR_END:
    if (ev->kind == YAML_EVENT_MAP_BEGIN) {
      if (!ds_begin_item(c)) return false;
      ds_push(c, DS_ITEM_KEY);
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      ds_pop(c);
    }
    return true;
  case DS_ITEM_KEY:
    if (ev->kind == YAML_EVENT_KEY) {
      const char* k = ev->str;
      const size_t n = ev->len;
      if      (slice_eq_cstr(k, n, "name"))           ds_set(c, DS_ITEM_VAL_NAME);
      else if (slice_eq_cstr(k, n, "sortName"))       ds_set(c, DS_ITEM_VAL_SORTNAME);
      else if (slice_eq_cstr(k, n, "localizedName"))  ds_set(c, DS_ITEM_VAL_LOCALIZED);
      else if (slice_eq_cstr(k, n, "saveName"))       ds_set(c, DS_ITEM_VAL_SAVENAME);
      else if (slice_eq_cstr(k, n, "serials"))        ds_set(c, DS_ITEM_VAL_SERIALS);
      else                                            ds_set(c, DS_ITEM_VAL_UNKNOWN);
    } else if (ev->kind == YAML_EVENT_MAP_END) {
      ds_finalize_item(c);
      ds_pop(c);  /* back to DS_TOP_ITEM_OR_END */
    }
    return true;
  case DS_ITEM_VAL_NAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur->title = ev->str; c->cur->title_len = (u32)ev->len;
      ds_set(c, DS_ITEM_KEY);
    }
    return true;
  case DS_ITEM_VAL_SORTNAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur->sort_title = ev->str; c->cur->sort_title_len = (u32)ev->len;
      ds_set(c, DS_ITEM_KEY);
    }
    return true;
  case DS_ITEM_VAL_LOCALIZED:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur->localized_title = ev->str; c->cur->localized_title_len = (u32)ev->len;
      ds_set(c, DS_ITEM_KEY);
    }
    return true;
  case DS_ITEM_VAL_SAVENAME:
    if (ev->kind == YAML_EVENT_SCALAR) {
      c->cur->save_title = ev->str; c->cur->save_title_len = (u32)ev->len;
      ds_set(c, DS_ITEM_KEY);
    }
    return true;
  case DS_ITEM_VAL_SERIALS:
    if (ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ds_set(c, DS_ITEM_KEY);
      ds_push(c, DS_IN_SERIALS);
    } else if (ev->kind == YAML_EVENT_SCALAR) {
      ds_set(c, DS_ITEM_KEY);
    }
    return true;
  case DS_ITEM_VAL_UNKNOWN:
    if (ev->kind == YAML_EVENT_SCALAR) ds_set(c, DS_ITEM_KEY);
    else if (ev->kind == YAML_EVENT_MAP_BEGIN || ev->kind == YAML_EVENT_SEQ_BEGIN) {
      ds_set(c, DS_ITEM_KEY);
      ds_push(c, DS_SKIP);
      c->skip_depth = 0;
    }
    return true;
  case DS_IN_SERIALS:
    if (ev->kind == YAML_EVENT_SCALAR) {
      if (ds_grow_serials(c)) {
        c->serials[c->serials_count]      = ev->str;
        c->serial_lens[c->serials_count]  = (u32)ev->len;
        c->serials_count++;
      }
    } else if (ev->kind == YAML_EVENT_SEQ_END) {
      ds_pop(c);
    }
    return true;

  case DS_SKIP: /* handled above */
    return true;
  }
  return true;
}

/* Sort + index build                                                     */

static int compare_entries_by_serial(const void* a, const void* b)
{
  const game_database_entry_t* ea = (const game_database_entry_t*)a;
  const game_database_entry_t* eb = (const game_database_entry_t*)b;
  return slice_cmp(ea->serial, ea->serial_len, eb->serial, eb->serial_len);
}

static int compare_codes(const void* a, const void* b)
{
  const code_entry_t* ca = (const code_entry_t*)a;
  const code_entry_t* cb = (const code_entry_t*)b;
  return slice_cmp(ca->code, ca->code_len, cb->code, cb->code_len);
}

/* bsearch helper: locate an entry by serial slice. */
static const game_database_entry_t* find_entry_by_serial_slice(const char* s, size_t n)
{
  if (s_state.entries_count == 0) return NULL;
  size_t lo = 0, hi = s_state.entries_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = slice_cmp(s_state.entries[mid].serial, s_state.entries[mid].serial_len, s, n);
    if (c < 0)      lo = mid + 1;
    else if (c > 0) hi = mid;
    else            return &s_state.entries[mid];
  }
  return NULL;
}

static u32 find_entry_index_by_serial_slice(const char* s, size_t n, bool* out_found)
{
  *out_found = false;
  size_t lo = 0, hi = s_state.entries_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = slice_cmp(s_state.entries[mid].serial, s_state.entries[mid].serial_len, s, n);
    if (c < 0)      lo = mid + 1;
    else if (c > 0) hi = mid;
    else            { *out_found = true; return (u32)mid; }
  }
  return 0;
}

static void bind_disc_sets_to_entries(void)
{
  for (size_t i = 0; i < s_state.disc_sets_count; i++) {
    game_database_disc_set_t* dset = &s_state.disc_sets[i];
    for (u32 j = 0; j < dset->serials_count; j++) {
      const char* s = dset->serials[j];
      const u32   n = dset->serial_lens[j];
      bool found = false;
      u32 idx = find_entry_index_by_serial_slice(s, n, &found);
      if (!found) {
        WARNING_LOG("Serial in disc set does not exist in game database");
        continue;
      }
      game_database_entry_t* e = &s_state.entries[idx];
      if (e->disc_set != NULL) {

        WARNING_LOG("Serial is already part of another disc set");
        continue;
      }
      e->disc_set = dset;
    }
  }
}

/* Binary cache                                                           */
/*
 *
 *   header:
 *     u32 signature (= GAME_DATABASE_CACHE_SIGNATURE)
 *     u32 version   (= GAME_DATABASE_CACHE_VERSION)
 *     u64 gamedb_yaml_mtime    (unix epoch seconds)
 *     u64 discsets_yaml_mtime
 *     u32 num_disc_sets
 *     u32 num_entries
 *     u32 num_codes
 *
 *   num_disc_sets disc-set records:
 *     size-prefixed: title, sort_title, localized_title, save_title
 *     u32 num_serials
 *     num_serials size-prefixed strings
 *
 *   num_entries entry records (see save/load below for the field order).
 *
 *   num_codes code records:
 *     size-prefixed string code
 *     u32 entry_index
 *
 * Strings on the load path are pointers into s_state.cache_data (zero-copy
 * BinarySpanReader); the cache_data buffer is held until module teardown.
 */

/* Returns mtime as unix-epoch-seconds, or 0 if the file is missing. */
static u64 get_yaml_mtime(const char* path)
{
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (u64)st.st_mtime;
}

static bool cache_read_string(binary_span_reader_t* r, const char** out_str, u32* out_len)
{
  return binary_span_reader_read_size_prefixed(r, out_str, out_len);
}

/* Read a `bool has_value` byte; if true, read the value into `*v`.
   Each XXX_OPT macro corresponds to a typed read. */
#define CACHE_READ_OPT(reader, has_field, val_field, read_fn)                  \
  do {                                                                         \
    bool _has_v;                                                               \
    if (!binary_span_reader_read_bool((reader), &_has_v)) return false;        \
    (has_field) = _has_v;                                                      \
    if (_has_v) {                                                              \
      if (!read_fn((reader), &(val_field))) return false;                      \
    }                                                                          \
  } while (0)

/* Same as CACHE_READ_OPT, but the destination is an enum stored as u8 in the
   cache.  We read into a u8 temporary and cast. */
#define CACHE_READ_OPT_ENUM(reader, has_field, val_field, enum_t)              \
  do {                                                                         \
    bool _has_v;                                                               \
    if (!binary_span_reader_read_bool((reader), &_has_v)) return false;        \
    (has_field) = _has_v;                                                      \
    if (_has_v) {                                                              \
      u8 _t;                                                                   \
      if (!binary_span_reader_read_u8((reader), &_t)) return false;            \
      (val_field) = (enum_t)_t;                                                \
    }                                                                          \
  } while (0)

/* Symmetric write helpers. */
#define CACHE_WRITE_OPT(writer, has_field, val_field, write_fn)                \
  do {                                                                         \
    if (!binary_file_writer_write_bool((writer), (has_field))) return false;   \
    if ((has_field)) {                                                         \
      if (!write_fn((writer), (val_field))) return false;                      \
    }                                                                          \
  } while (0)

#define CACHE_WRITE_OPT_ENUM(writer, has_field, val_field)                     \
  do {                                                                         \
    if (!binary_file_writer_write_bool((writer), (has_field))) return false;   \
    if ((has_field)) {                                                         \
      if (!binary_file_writer_write_u8((writer), (u8)(val_field))) return false;\
    }                                                                          \
  } while (0)

static bool load_from_cache(void)
{
  timer_t_ load_timer;
  timer_init(&load_timer);

  /* Slurp the cache file.  Missing file is the common boot path; not an error. */
  Error err = ERROR_INIT;
  u8* data = NULL;
  size_t len = 0;
  if (!fs_read_binary_file_path(GAMEDB_CACHE_PATH, &data, &len, &err)) {
    DEV_LOG("Failed to read cache, loading full database: %s",
            Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }

  binary_span_reader_t r;
  binary_span_reader_init(&r, data, len);

  /* Header. */
  u32 signature = 0, version = 0;
  u32 num_disc_sets = 0, num_entries = 0, num_codes = 0;
  u64 file_gamedb_ts = 0, file_discsets_ts = 0;
  if (!binary_span_reader_read_u32(&r, &signature)        ||
      !binary_span_reader_read_u32(&r, &version)          ||
      !binary_span_reader_read_u64(&r, &file_gamedb_ts)   ||
      !binary_span_reader_read_u64(&r, &file_discsets_ts) ||
      !binary_span_reader_read_u32(&r, &num_disc_sets)    ||
      !binary_span_reader_read_u32(&r, &num_entries)      ||
      !binary_span_reader_read_u32(&r, &num_codes)) 
  {
    DEV_LOG("Cache header is corrupted or version mismatch.");
    free(data);
    return false;
  }
  if (signature != GAME_DATABASE_CACHE_SIGNATURE ||
      version   != GAME_DATABASE_CACHE_VERSION)
  {
    DEV_LOG("Cache header is corrupted or version mismatch.");
    free(data);
    return false;
  }

  const u64 gamedb_ts   = get_yaml_mtime(GAMEDB_YAML_PATH);
  const u64 discsets_ts = get_yaml_mtime(DISCSETS_YAML_PATH);
  if (gamedb_ts != file_gamedb_ts || discsets_ts != file_discsets_ts) {
    DEV_LOG("Cache is out of date, recreating.");
    free(data);
    return false;
  }

  /* Pre-allocate so realloc can't move buffers mid-load (entry pointers
     into disc_sets must stay stable). */
  if (num_entries) {
    s_state.entries = (game_database_entry_t*)calloc(num_entries, sizeof(*s_state.entries));
    if (!s_state.entries) { free(data); return false; }
    s_state.entries_capacity = num_entries;
  }
  if (num_disc_sets) {
    s_state.disc_sets = (game_database_disc_set_t*)calloc(num_disc_sets, sizeof(*s_state.disc_sets));
    if (!s_state.disc_sets) { free(s_state.entries); s_state.entries = NULL; free(data); return false; }
    s_state.disc_sets_capacity = num_disc_sets;
  }
  if (num_codes) {
    s_state.codes = (code_entry_t*)calloc(num_codes, sizeof(*s_state.codes));
    if (!s_state.codes) {
      free(s_state.entries);   s_state.entries = NULL;
      free(s_state.disc_sets); s_state.disc_sets = NULL;
      free(data);
      return false;
    }
    s_state.codes_capacity = num_codes;
  }

  /* Disc set records. */
  for (u32 i = 0; i < num_disc_sets; i++) {
    game_database_disc_set_t* d = &s_state.disc_sets[i];
    u32 num_serials = 0;
    if (!cache_read_string(&r, &d->title,           &d->title_len)           ||
        !cache_read_string(&r, &d->sort_title,      &d->sort_title_len)      ||
        !cache_read_string(&r, &d->localized_title, &d->localized_title_len) ||
        !cache_read_string(&r, &d->save_title,      &d->save_title_len)      ||
        !binary_span_reader_read_u32(&r, &num_serials)) 
    {
      DEV_LOG("Cache disc set entry is corrupted.");
      goto fail;
    }
    if (num_serials > 0) {
      d->serials     = (const char**)malloc(num_serials * sizeof(*d->serials));
      d->serial_lens = (u32*)malloc(num_serials * sizeof(*d->serial_lens));
      if (!d->serials || !d->serial_lens) {
        free((void*)d->serials);    d->serials     = NULL;
        free(d->serial_lens);       d->serial_lens = NULL;
        DEV_LOG("Out of memory loading disc set serials.");
        goto fail;
      }
      for (u32 j = 0; j < num_serials; j++) {
        if (!cache_read_string(&r, &d->serials[j], &d->serial_lens[j])) {
          DEV_LOG("Cache disc set entry is corrupted.");
          goto fail;
        }
      }
      d->serials_count = num_serials;
    }
    s_state.disc_sets_count++;
  }

  /* Entry records. */
  for (u32 i = 0; i < num_entries; i++) {
    game_database_entry_t* e = &s_state.entries[i];
    s32 disc_set_index = -1;
    u8  compatibility  = 0;

    if (!cache_read_string(&r, &e->serial,                       &e->serial_len)                       ||
        !cache_read_string(&r, &e->title,                        &e->title_len)                        ||
        !cache_read_string(&r, &e->sort_title,                   &e->sort_title_len)                   ||
        !cache_read_string(&r, &e->localized_title,              &e->localized_title_len)              ||
        !cache_read_string(&r, &e->save_title,                   &e->save_title_len)                   ||
        !cache_read_string(&r, &e->genre,                        &e->genre_len)                        ||
        !cache_read_string(&r, &e->developer,                    &e->developer_len)                    ||
        !cache_read_string(&r, &e->publisher,                    &e->publisher_len)                    ||
        !cache_read_string(&r, &e->compatibility_version_tested, &e->compatibility_version_tested_len) ||
        !cache_read_string(&r, &e->compatibility_comments,       &e->compatibility_comments_len)       ||
        !binary_span_reader_read_s32(&r, &disc_set_index) ||
        (disc_set_index >= 0 && (u32)disc_set_index >= num_disc_sets) ||
        !binary_span_reader_read_u64(&r, &e->release_date) ||
        !binary_span_reader_read_u8 (&r, &e->min_players)  ||
        !binary_span_reader_read_u8 (&r, &e->max_players)  ||
        !binary_span_reader_read_u8 (&r, &e->min_blocks)   ||
        !binary_span_reader_read_u8 (&r, &e->max_blocks)   ||
        !binary_span_reader_read_u16(&r, &e->supported_controllers) ||
        !binary_span_reader_read_u8 (&r, &compatibility)   ||
        compatibility >= (u8)GAME_DATABASE_COMPAT_RATING_COUNT ||
        !binary_span_reader_read(&r, e->trait_bits,    GAME_DATABASE_TRAIT_BYTES) ||
        !binary_span_reader_read(&r, e->language_bits, GAME_DATABASE_LANGUAGE_BYTES)) 
    {
      DEV_LOG("Cache entry is corrupted.");
      goto fail;
    }
   
    e->compatibility = (game_database_compat_rating_t)compatibility;
    e->disc_set      = (disc_set_index >= 0)
                         ? &s_state.disc_sets[(u32)disc_set_index]
                         : NULL;

    /* Per-entry settings overrides. */
    CACHE_READ_OPT     (&r, e->has_display_active_start_offset,
                            e->display_active_start_offset, binary_span_reader_read_s16);
    CACHE_READ_OPT     (&r, e->has_display_active_end_offset,
                            e->display_active_end_offset,   binary_span_reader_read_s16);
    CACHE_READ_OPT     (&r, e->has_display_line_start_offset,
                            e->display_line_start_offset,   binary_span_reader_read_s8);
    CACHE_READ_OPT     (&r, e->has_display_line_end_offset,
                            e->display_line_end_offset,     binary_span_reader_read_s8);
    CACHE_READ_OPT_ENUM(&r, e->has_display_crop_mode,
                            e->display_crop_mode,           display_crop_mode_t);
    CACHE_READ_OPT_ENUM(&r, e->has_display_deinterlacing_mode,
                            e->display_deinterlacing_mode,  display_deinterlacing_mode_t);
    CACHE_READ_OPT     (&r, e->has_dma_max_slice_ticks,
                            e->dma_max_slice_ticks,         binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_dma_halt_ticks,
                            e->dma_halt_ticks,              binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_cdrom_max_seek_speedup_cycles,
                            e->cdrom_max_seek_speedup_cycles, binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_cdrom_max_read_speedup_cycles,
                            e->cdrom_max_read_speedup_cycles, binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_gpu_fifo_size,
                            e->gpu_fifo_size,               binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_gpu_max_run_ahead,
                            e->gpu_max_run_ahead,           binary_span_reader_read_u32);
    CACHE_READ_OPT     (&r, e->has_gpu_pgxp_tolerance,
                            e->gpu_pgxp_tolerance,          binary_span_reader_read_float);
    CACHE_READ_OPT     (&r, e->has_gpu_pgxp_depth_threshold,
                            e->gpu_pgxp_depth_threshold,    binary_span_reader_read_float);
    CACHE_READ_OPT     (&r, e->has_gpu_pgxp_preserve_proj_fp,
                            e->gpu_pgxp_preserve_proj_fp,   binary_span_reader_read_bool);
    CACHE_READ_OPT_ENUM(&r, e->has_gpu_line_detect_mode,
                            e->gpu_line_detect_mode,        gpu_line_detect_mode_t);
    CACHE_READ_OPT     (&r, e->has_cpu_overclock,
                            e->cpu_overclock,               binary_span_reader_read_u8);

    s_state.entries_count++;
  }

  /* Code records. */
  for (u32 i = 0; i < num_codes; i++) {
    code_entry_t* ce = &s_state.codes[i];
    const char* str = NULL;
    u32 slen = 0;
    u32 idx  = 0;
    if (!cache_read_string(&r, &str, &slen) ||
        !binary_span_reader_read_u32(&r, &idx) ||
        idx >= num_entries) 
    {
      DEV_LOG("Cache code entry is corrupted.");
      goto fail;
    }
    ce->code        = str;
    ce->code_len    = (u16)slen;
    ce->entry_index = idx;
    s_state.codes_count++;
  }

  /* Hand the slurped buffer to the module: string fields above point into it. */
  s_state.cache_data     = data;
  s_state.cache_data_len = len;

  const double ms = timer_get_milliseconds(&load_timer);
  INFO_LOG("Database load (cache) of %u entries took %.0f ms.",
           (unsigned)s_state.entries_count, ms);
  return true;

fail:
  /* Roll back partial state.  Any per-disc-set serials arrays we've allocated
   * so far need to be freed too. */
  for (size_t i = 0; i < s_state.disc_sets_count; i++) {
    free((void*)s_state.disc_sets[i].serials);
    free(s_state.disc_sets[i].serial_lens);
  }
  free(s_state.entries);   s_state.entries = NULL;
  s_state.entries_count = s_state.entries_capacity = 0;
  free(s_state.disc_sets); s_state.disc_sets = NULL;
  s_state.disc_sets_count = s_state.disc_sets_capacity = 0;
  free(s_state.codes);     s_state.codes = NULL;
  s_state.codes_count = s_state.codes_capacity = 0;
  free(data);
  return false;
}

static bool save_to_cache(void)
{
  /* Write to a temp sibling and rename atomically.  rename() on POSIX is
     atomic when the source and destination live on the same filesystem; the
     temp lives next to the cache for that reason. */
  char tmp_path[256];
  const int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GAMEDB_CACHE_PATH);
  if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
    WARNING_LOG("Cache temp path too long; skipping save.");
    return false;
  }

  Error err = ERROR_INIT;
  FILE* fp = fs_open_file(tmp_path, "wb", &err);
  if (!fp) {
    WARNING_LOG("Failed to open cache file for writing: %s",
                Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }

  binary_file_writer_t w;
  binary_file_writer_init(&w, fp);

  const u64 gamedb_ts   = get_yaml_mtime(GAMEDB_YAML_PATH);
  const u64 discsets_ts = get_yaml_mtime(DISCSETS_YAML_PATH);

  binary_file_writer_write_u32(&w, GAME_DATABASE_CACHE_SIGNATURE);
  binary_file_writer_write_u32(&w, GAME_DATABASE_CACHE_VERSION);
  binary_file_writer_write_u64(&w, gamedb_ts);
  binary_file_writer_write_u64(&w, discsets_ts);
  binary_file_writer_write_u32(&w, (u32)s_state.disc_sets_count);
  binary_file_writer_write_u32(&w, (u32)s_state.entries_count);
  binary_file_writer_write_u32(&w, (u32)s_state.codes_count);

  for (size_t i = 0; i < s_state.disc_sets_count; i++) {
    const game_database_disc_set_t* d = &s_state.disc_sets[i];
    binary_file_writer_write_size_prefixed(&w, d->title,           d->title_len);
    binary_file_writer_write_size_prefixed(&w, d->sort_title,      d->sort_title_len);
    binary_file_writer_write_size_prefixed(&w, d->localized_title, d->localized_title_len);
    binary_file_writer_write_size_prefixed(&w, d->save_title,      d->save_title_len);
    binary_file_writer_write_u32(&w, d->serials_count);
    for (u32 j = 0; j < d->serials_count; j++) {
      binary_file_writer_write_size_prefixed(&w, d->serials[j], d->serial_lens[j]);
    }
  }

  for (size_t i = 0; i < s_state.entries_count; i++) {
    const game_database_entry_t* e = &s_state.entries[i];
    binary_file_writer_write_size_prefixed(&w, e->serial,                       e->serial_len);
    binary_file_writer_write_size_prefixed(&w, e->title,                        e->title_len);
    binary_file_writer_write_size_prefixed(&w, e->sort_title,                   e->sort_title_len);
    binary_file_writer_write_size_prefixed(&w, e->localized_title,              e->localized_title_len);
    binary_file_writer_write_size_prefixed(&w, e->save_title,                   e->save_title_len);
    binary_file_writer_write_size_prefixed(&w, e->genre,                        e->genre_len);
    binary_file_writer_write_size_prefixed(&w, e->developer,                    e->developer_len);
    binary_file_writer_write_size_prefixed(&w, e->publisher,                    e->publisher_len);
    binary_file_writer_write_size_prefixed(&w, e->compatibility_version_tested, e->compatibility_version_tested_len);
    binary_file_writer_write_size_prefixed(&w, e->compatibility_comments,       e->compatibility_comments_len);

    /* disc_set_index: pointer-arithmetic offset into our disc_sets[] array, or -1. */
    s32 disc_set_index = -1;
    if (e->disc_set) {
      disc_set_index = (s32)(e->disc_set - s_state.disc_sets);
    }
    binary_file_writer_write_s32(&w, disc_set_index);
    binary_file_writer_write_u64(&w, e->release_date);
    binary_file_writer_write_u8 (&w, e->min_players);
    binary_file_writer_write_u8 (&w, e->max_players);
    binary_file_writer_write_u8 (&w, e->min_blocks);
    binary_file_writer_write_u8 (&w, e->max_blocks);
    binary_file_writer_write_u16(&w, e->supported_controllers);
    binary_file_writer_write_u8 (&w, (u8)e->compatibility);
    binary_file_writer_write    (&w, e->trait_bits,    GAME_DATABASE_TRAIT_BYTES);
    binary_file_writer_write    (&w, e->language_bits, GAME_DATABASE_LANGUAGE_BYTES);

    CACHE_WRITE_OPT     (&w, e->has_display_active_start_offset,
                             e->display_active_start_offset, binary_file_writer_write_s16);
    CACHE_WRITE_OPT     (&w, e->has_display_active_end_offset,
                             e->display_active_end_offset,   binary_file_writer_write_s16);
    CACHE_WRITE_OPT     (&w, e->has_display_line_start_offset,
                             e->display_line_start_offset,   binary_file_writer_write_s8);
    CACHE_WRITE_OPT     (&w, e->has_display_line_end_offset,
                             e->display_line_end_offset,     binary_file_writer_write_s8);
    CACHE_WRITE_OPT_ENUM(&w, e->has_display_crop_mode,
                             e->display_crop_mode);
    CACHE_WRITE_OPT_ENUM(&w, e->has_display_deinterlacing_mode,
                             e->display_deinterlacing_mode);
    CACHE_WRITE_OPT     (&w, e->has_dma_max_slice_ticks,
                             e->dma_max_slice_ticks,         binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_dma_halt_ticks,
                             e->dma_halt_ticks,              binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_cdrom_max_seek_speedup_cycles,
                             e->cdrom_max_seek_speedup_cycles, binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_cdrom_max_read_speedup_cycles,
                             e->cdrom_max_read_speedup_cycles, binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_gpu_fifo_size,
                             e->gpu_fifo_size,               binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_gpu_max_run_ahead,
                             e->gpu_max_run_ahead,           binary_file_writer_write_u32);
    CACHE_WRITE_OPT     (&w, e->has_gpu_pgxp_tolerance,
                             e->gpu_pgxp_tolerance,          binary_file_writer_write_float);
    CACHE_WRITE_OPT     (&w, e->has_gpu_pgxp_depth_threshold,
                             e->gpu_pgxp_depth_threshold,    binary_file_writer_write_float);
    CACHE_WRITE_OPT     (&w, e->has_gpu_pgxp_preserve_proj_fp,
                             e->gpu_pgxp_preserve_proj_fp,   binary_file_writer_write_bool);
    CACHE_WRITE_OPT_ENUM(&w, e->has_gpu_line_detect_mode,
                             e->gpu_line_detect_mode);
    CACHE_WRITE_OPT     (&w, e->has_cpu_overclock,
                             e->cpu_overclock,               binary_file_writer_write_u8);
  }

  for (size_t i = 0; i < s_state.codes_count; i++) {
    const code_entry_t* ce = &s_state.codes[i];
    binary_file_writer_write_size_prefixed(&w, ce->code, (u32)ce->code_len);
    binary_file_writer_write_u32(&w, ce->entry_index);
  }

  if (!binary_file_writer_is_good(&w)) {
    fclose(fp);
    unlink(tmp_path);
    WARNING_LOG("Write error while building game database cache; discarded.");
    return false;
  }
  if (fclose(fp) != 0) {
    unlink(tmp_path);
    WARNING_LOG("fclose() failed while building game database cache; discarded.");
    return false;
  }

  /* Atomic rename. */
  if (rename(tmp_path, GAMEDB_CACHE_PATH) != 0) {
    unlink(tmp_path);
    WARNING_LOG("Failed to rename temp cache into place.");
    return false;
  }
  return true;
}

/* Top-level loader                                                       */

static bool load_gamedb_yaml(void)
{
  Error err = ERROR_INIT;
  timer_t_ load_timer;
  timer_init(&load_timer);

  {
    u8* data = NULL; size_t len = 0;
    if (!fs_read_binary_file_path(GAMEDB_YAML_PATH, &data, &len, &err)) {
      ERROR_LOG("Failed to read game database: %s", Error_get_description(&err));
      Error_destroy(&err);
      return false;
    }
    s_state.db_data     = data;
    s_state.db_data_len = len;
  }

  {
    u8* data = NULL; size_t len = 0;
    if (!fs_read_binary_file_path(DISCSETS_YAML_PATH, &data, &len, &err)) {
      ERROR_LOG("Failed to read disc set database: %s", Error_get_description(&err));
      Error_destroy(&err);
      return false;
    }
    s_state.discsets_data     = data;
    s_state.discsets_data_len = len;
  }

  {
    gs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state_stack[0] = GS_TOP_MAP;
    ctx.state_top      = 0;
    ctx.buf_base       = (const char*)s_state.db_data;
    ctx.buf_end        = (const char*)s_state.db_data + s_state.db_data_len;

    yaml_parse_error_t perr;
    if (!yaml_parse((const char*)s_state.db_data, s_state.db_data_len,
                    gamedb_cb, &ctx, &perr)) {
      ERROR_LOG("gamedb.yaml parse error at line %u: %s", perr.line,
                perr.message ? perr.message : "(null)");
      return false;
    }
  }

  if (s_state.entries_count == 0) {
    ERROR_LOG("Game database is empty.");
    return false;
  }

  /* Build a (pre-sort -> post-sort) index map by recording each entry's
     original index in a sidecar, sorting both, then patching codes[]. */
  {
    /* Save an array of the original entry slice (serial + len) so we can
     * remap codes' pre-sort entry_index. */
    typedef struct { const char* s; u32 n; } sk_t;
    sk_t* pre = (sk_t*)malloc(s_state.entries_count * sizeof(*pre));
    if (!pre) {
      ERROR_LOG("OOM building code remap table");
      return false;
    }
    for (size_t i = 0; i < s_state.entries_count; i++) {
      pre[i].s = s_state.entries[i].serial;
      pre[i].n = s_state.entries[i].serial_len;
    }

    qsort(s_state.entries, s_state.entries_count, sizeof(*s_state.entries),
          compare_entries_by_serial);

    /* For each code, replace its (pre-sort) entry_index with the post-sort
       index found via bsearch on the sorted entries. */
    for (size_t i = 0; i < s_state.codes_count; i++) {
      code_entry_t* ce = &s_state.codes[i];
      /* code_entry was created with entry_index = pre-sort row.  The serial
         of that row is pre[ce->entry_index]. */
      if (ce->entry_index >= s_state.entries_count) {
        /* shouldn't happen; defensive */
        continue;
      }
      const sk_t* src = &pre[ce->entry_index];
      bool found = false;
      u32 idx = find_entry_index_by_serial_slice(src->s, src->n, &found);
      ce->entry_index = found ? idx : 0;
    }
    free(pre);
  }

  qsort(s_state.codes, s_state.codes_count, sizeof(*s_state.codes), compare_codes);

  /* In-place dedup: if two consecutive codes are identical, keep first. */
  {
    size_t w = 0;
    for (size_t r = 0; r < s_state.codes_count; r++) {
      if (w > 0 && compare_codes(&s_state.codes[w - 1], &s_state.codes[r]) == 0) {
        WARNING_LOG("Duplicate code");
        continue;
      }
      if (w != r) s_state.codes[w] = s_state.codes[r];
      w++;
    }
    s_state.codes_count = w;
  }

  {
    ds_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state_stack[0] = DS_TOP;
    ctx.state_top      = 0;

    yaml_parse_error_t perr;
    bool ok = yaml_parse((const char*)s_state.discsets_data, s_state.discsets_data_len,
                         discsets_cb, &ctx, &perr);
    free(ctx.serials);
    free(ctx.serial_lens);
    if (!ok) {
      ERROR_LOG("discsets.yaml parse error at line %u: %s", perr.line,
                perr.message ? perr.message : "(null)");
      return false;
    }
  }

  /* Trim disc_sets buffer so realloc can't move it after binding. */
  if (s_state.disc_sets_count > 0 &&
      s_state.disc_sets_count < s_state.disc_sets_capacity) {
    void* p = realloc(s_state.disc_sets,
                      s_state.disc_sets_count * sizeof(*s_state.disc_sets));
    if (p) {
      s_state.disc_sets          = (game_database_disc_set_t*)p;
      s_state.disc_sets_capacity = s_state.disc_sets_count;
    }
  }

  bind_disc_sets_to_entries();

  s_state.loaded = true;

  const double ms = timer_get_milliseconds(&load_timer);
  INFO_LOG("Database load of %u entries took %.0f ms.",
           (unsigned)s_state.entries_count, ms);
  return true;
}

/* Public API                                                             */

void game_database_ensure_loaded(void)
{
  if (s_state.loaded) return;

  /* Fast path: read prebuilt binary cache.  load_from_cache returns false
     (silently, with no state mutation visible past return) when the cache is
     missing, header-corrupt, or stale w.r.t. the YAML mtimes. */
  if (load_from_cache()) {
    s_state.loaded = true;
    return;
  }

  /* Slow path: parse YAML, then opportunistically build the cache so the next
     boot takes the fast path.  Cache write failures are non-fatal. */
  if (load_gamedb_yaml()) {
    save_to_cache();
    s_state.loaded = true;
    return;
  }

  /* Both paths failed.  Mark loaded anyway so subsequent calls don't keep
   * retrying; lookups will return NULL. */
  s_state.loaded = true;
}

const game_database_entry_t* game_database_get_entry_for_serial(const char* serial)
{
  game_database_ensure_loaded();
  if (!serial) return NULL;
  return game_database_get_entry_for_id(serial, (u32)strlen(serial));
}

const game_database_entry_t* game_database_get_entry_for_id(const char* code, u32 code_len)
{
  game_database_ensure_loaded();
  if (!code || code_len == 0 || s_state.codes_count == 0) return NULL;
  /* bsearch s_state.codes (sorted by code text). */
  size_t lo = 0, hi = s_state.codes_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = slice_cmp(s_state.codes[mid].code, s_state.codes[mid].code_len,
                      code, code_len);
    if (c < 0)      lo = mid + 1;
    else if (c > 0) hi = mid;
    else {
      const u32 idx = s_state.codes[mid].entry_index;
      if (idx >= s_state.entries_count) return NULL;
      return &s_state.entries[idx];
    }
  }
  return NULL;
}

const game_database_entry_t* game_database_get_entry_for_id_and_hash(const char* id, u64 hash)
{
  (void)hash;
  return game_database_get_entry_for_id(id, id ? (u32)strlen(id) : 0u);
}

/* Disc -> serial extraction                                              */

/* the SCES_123.45 -> SCES-12345 normalization in GetGameDetailsFromImage. */

static char* normalize_serial(const char* raw, size_t raw_len)
{
  if (!raw || raw_len == 0) return NULL;
  char* out = (char*)malloc(raw_len + 1u);
  if (!out) return NULL;
  size_t op = 0;
  for (size_t i = 0; i < raw_len; i++) {
    const char c = raw[i];
    if (c == '.') continue;
    out[op++] = (c == '_') ? '-' : (char)toupper((unsigned char)c);
  }
  out[op] = '\0';
  if (op == 0) { free(out); return NULL; }
  return out;
}

/* Parses SYSTEM.CNF contents and returns a malloc'd serial id, or NULL.
 *
 * 0x0D from each token, finds the line whose key is case-insensitively
 * "BOOT", takes the value, strips the "cdrom:" / leading-slashes prefix
 * (with strip_subdirectories=true so the longest match wins), drops the
 * ";version" suffix, and normalizes to SCES-12345 form. */
static char* extract_serial_from_system_cnf(const u8* contents, size_t size)
{
  if (!contents || size == 0) return NULL;

  /* Locate "BOOT = <value>" line.  CR/LF tolerant; case-insensitive on
   * "BOOT". */
  char* raw_value = NULL;
  size_t i = 0;
  while (i < size && !raw_value) {
    /* Skip leading whitespace on this line. */
    while (i < size && (contents[i] == ' ' || contents[i] == '\t' ||
                        contents[i] == '\r' || contents[i] == '\n'))
      i++;
    if (i >= size) break;

    /* Read key up to '=', '\n' or '\r'. */
    size_t k_start = i;
    while (i < size && contents[i] != '=' &&
           contents[i] != '\n' && contents[i] != '\r')
      i++;
    size_t k_end = i;
    while (k_end > k_start && (contents[k_end-1] == ' ' || contents[k_end-1] == '\t'))
      k_end--;

    if (i < size && contents[i] == '=') {
      const bool is_boot =
        ((k_end - k_start) == 4 &&
         (contents[k_start  ] == 'B' || contents[k_start  ] == 'b') &&
         (contents[k_start+1] == 'O' || contents[k_start+1] == 'o') &&
         (contents[k_start+2] == 'O' || contents[k_start+2] == 'o') && 
         (contents[k_start+3] == 'T' || contents[k_start+3] == 't'));
      i++; /* consume '=' */
      while (i < size && (contents[i] == ' ' || contents[i] == '\t'))
        i++;
      size_t v_start = i;
      while (i < size && contents[i] != '\n' && contents[i] != '\r')
        i++;
      size_t v_end = i;
      while (v_end > v_start && (contents[v_end-1] == ' ' || contents[v_end-1] == '\t'))
        v_end--;

      if (is_boot && v_end > v_start) {
        const size_t len = v_end - v_start;
        raw_value = (char*)malloc(len + 1u);
        if (!raw_value) return NULL;
        memcpy(raw_value, contents + v_start, len);
        raw_value[len] = '\0';
      }
    }

    /* Advance past CR/LF run. */
    while (i < size && (contents[i] == '\r' || contents[i] == '\n'))
      i++;
  }

  if (!raw_value) return NULL;

  /* Strip leading "cdrom:\..." / "cdrom:..." down to the bare filename:
   * take the suffix after the last '\\', else the suffix after ':'. */
  char* base = raw_value;
  char* slash = strrchr(base, '\\');
  if (!slash) slash = strrchr(base, '/');
  if (slash) {
    base = slash + 1;
  } else {
    char* colon = strrchr(base, ':');
    if (colon) base = colon + 1;
  }

  /* Drop trailing ";version" marker. */
  char* semi = strchr(base, ';');
  if (semi) *semi = '\0';

  /* Don't bother on the BIOS-shell sentinel (PSX.EXE). */
  if (strcasecmp(base, "PSX.EXE") == 0) {
    free(raw_value);
    return NULL;
  }

  char* serial = normalize_serial(base, strlen(base));
  free(raw_value);
  return serial;
}

const game_database_entry_t* game_database_get_entry_for_disc(struct cd_image* image)
{
  if (!image) return NULL;
  game_database_ensure_loaded();
  char* serial = game_database_get_serial_for_disc(image);
  if (!serial) return NULL;
  const game_database_entry_t* e = game_database_get_entry_for_serial(serial);
  free(serial);
  return e;
}

char* game_database_get_serial_for_disc(struct cd_image* image)
{
  if (!image) return NULL;

  iso_reader_t iso;
  iso_reader_init(&iso);
  /* Audio-only / non-PS1 discs cleanly fail open here; no error log. */
  if (!iso_reader_open(&iso, image, /*track*/ 1u, NULL)) {
    iso_reader_destroy(&iso);
    return NULL;
  }

  u8*    contents = NULL;
  size_t size     = 0;
  if (!iso_reader_read_file_path(&iso, "SYSTEM.CNF", &contents, &size,
                                 ISO_READER_READ_MODE_DATA, NULL)) {
    iso_reader_destroy(&iso);
    return NULL;
  }

  char* serial = extract_serial_from_system_cnf(contents, size);
  free(contents);
  iso_reader_destroy(&iso);
  return serial;
}

char* game_database_get_serial_for_path(const char* path)
{
  if (!path || !path[0]) return NULL;
  cd_image_t* img = cd_image_open(path, NULL);
  if (!img) return NULL;
  char* s = game_database_get_serial_for_disc(img);
  cd_image_destroy(img);
  return s;
}

/* Name accessors; table-backed                                         */

const char* game_database_get_trait_name(game_database_trait_t t)
{
  if ((u32)t >= (u32)GAME_DATABASE_TRAIT_COUNT) return "";
  return s_trait_names[(u32)t];
}

const char* game_database_get_trait_display_name(game_database_trait_t t)
{
  if ((u32)t >= (u32)GAME_DATABASE_TRAIT_COUNT) return "";
  return s_trait_display_names[(u32)t];
}

const char* game_database_get_compat_rating_name(game_database_compat_rating_t r)
{
  if ((u32)r >= (u32)GAME_DATABASE_COMPAT_RATING_COUNT) return "";
  return s_compatibility_rating_names[(u32)r];
}

const char* game_database_get_compat_rating_display_name(game_database_compat_rating_t r)
{
  if ((u32)r >= (u32)GAME_DATABASE_COMPAT_RATING_COUNT) return "";
  return s_compatibility_rating_display_names[(u32)r];
}

const char* game_database_get_language_name(game_database_language_t l)
{
  if ((u32)l >= (u32)GAME_DATABASE_LANGUAGE_COUNT) return "";
  return s_language_names[(u32)l];
}

const char* game_database_get_language_display_name(game_database_language_t l)
{
  if ((u32)l >= (u32)GAME_DATABASE_LANGUAGE_COUNT) return "";
  return s_language_display_names[(u32)l];
}

/* Name -> enum reverse lookups                                           */

static bool name_table_lookup(const char* const* table, u32 count,
                              const char* str, u32 len, u32* out_idx)
{
  if (!str || len == 0 || !out_idx) return false;
  for (u32 i = 0; i < count; i++) {
    const char* candidate = table[i];
    if (strlen(candidate) == (size_t)len && memcmp(candidate, str, (size_t)len) == 0) {
      *out_idx = i;
      return true;
    }
  }
  return false;
}

bool game_database_parse_language_name(const char* str, u32 len, game_database_language_t* out)
{
  u32 idx = 0;
  if (!name_table_lookup(s_language_names, (u32)GAME_DATABASE_LANGUAGE_COUNT, str, len, &idx))
    return false;
  if (out) *out = (game_database_language_t)idx;
  return true;
}

bool game_database_parse_trait_name(const char* str, u32 len, game_database_trait_t* out)
{
  u32 idx = 0;
  if (!name_table_lookup(s_trait_names, (u32)GAME_DATABASE_TRAIT_COUNT, str, len, &idx))
    return false;
  if (out) *out = (game_database_trait_t)idx;
  return true;
}

bool game_database_parse_compat_rating_name(const char* str, u32 len,
                                            game_database_compat_rating_t* out)
{
  u32 idx = 0;
  if (!name_table_lookup(s_compatibility_rating_names,
                         (u32)GAME_DATABASE_COMPAT_RATING_COUNT, str, len, &idx))
    return false;
  if (out) *out = (game_database_compat_rating_t)idx;
  return true;
}

/* ApplySettings lives in game_database_apply.c.                          */

/* TrackHashes lives in game_database_tracks.c.                           */

/* Internal accessors for tests                                           */

/* Not declared in the public header; tests forward-declare these. */
size_t game_database_test_get_entry_count(void);
size_t game_database_test_get_disc_set_count(void);
size_t game_database_test_get_code_count(void);
void   game_database_test_reset(void);
bool   game_database_test_load_from_cache(void);
bool   game_database_test_save_to_cache(void);
bool   game_database_test_is_loaded_from_cache(void);

size_t game_database_test_get_entry_count(void)    { return s_state.entries_count; }
size_t game_database_test_get_disc_set_count(void) { return s_state.disc_sets_count; }
size_t game_database_test_get_code_count(void)     { return s_state.codes_count; }

/* Tear down all loader state and free owned buffers.  Used by the cache
   roundtrip test to swap between YAML and cache load paths within a single
   process. */
void game_database_test_reset(void)
{
  for (size_t i = 0; i < s_state.disc_sets_count; i++) {
    free((void*)s_state.disc_sets[i].serials);
    free(s_state.disc_sets[i].serial_lens);
  }
  free(s_state.entries);   s_state.entries   = NULL;
  free(s_state.disc_sets); s_state.disc_sets = NULL;
  free(s_state.codes);     s_state.codes     = NULL;
  free(s_state.db_data);       s_state.db_data       = NULL;
  free(s_state.discsets_data); s_state.discsets_data = NULL;
  free(s_state.cache_data);    s_state.cache_data    = NULL;

  s_state.entries_count = s_state.entries_capacity = 0;
  s_state.disc_sets_count = s_state.disc_sets_capacity = 0;
  s_state.codes_count = s_state.codes_capacity = 0;
  s_state.db_data_len = s_state.discsets_data_len = s_state.cache_data_len = 0;
  s_state.loaded = false;
}

bool game_database_test_load_from_cache(void)
{
  /* Mirror game_database_ensure_loaded(): on success, mark loaded so later
   * lookups don't re-enter the loader. */
  if (load_from_cache()) { s_state.loaded = true; return true; }
  return false;
}
bool game_database_test_save_to_cache(void)         { return save_to_cache();   }
bool game_database_test_is_loaded_from_cache(void)  { return s_state.cache_data != NULL; }
