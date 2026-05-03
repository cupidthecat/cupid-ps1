/*
 * translated to C: snake_case, `_t` typedef suffix, struct fields where C++
 * had class methods.  std::optional<T> becomes a `bool has_X; T X;` pair.
 * std::bitset becomes a `u8 bits[(N + 7) / 8]` array, accessed via the
 * inline helpers at the bottom of this file.
 *
 * The lifetime of all string fields in `game_database_entry_t` is bound to
 * the loader's owned data buffer (`s_state.db_data`), so callers must NOT
 * free the bytes pointed to by `serial.str`, `title.str`, etc.  The strings
 * are NOT NUL-terminated; use the paired `_len` field.
 */

#ifndef CUPID_CORE_GAME_DATABASE_H
#define CUPID_CORE_GAME_DATABASE_H

#include "common/types.h"

#include "core/settings.h" /* display_crop_mode_t, display_deinterlacing_mode_t,
                              gpu_line_detect_mode_t */
#include "core/types.h"    /* CONTROLLER_TYPE_COUNT */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cd_image;

/* Compatibility rating                                                   */

typedef enum {
  GAME_DATABASE_COMPAT_RATING_UNKNOWN              = 0,
  GAME_DATABASE_COMPAT_RATING_DOESNT_BOOT          = 1,
  GAME_DATABASE_COMPAT_RATING_CRASHES_IN_INTRO     = 2,
  GAME_DATABASE_COMPAT_RATING_CRASHES_IN_GAME      = 3,
  GAME_DATABASE_COMPAT_RATING_GRAPHICAL_AUDIO_ISSUES = 4,
  GAME_DATABASE_COMPAT_RATING_NO_ISSUES            = 5,
  GAME_DATABASE_COMPAT_RATING_COUNT                = 6,
} game_database_compat_rating_t;

/* Trait                                                                  */
/* Order MUST match s_trait_names[] in game_database.c so that linear */
/* lookup against the YAML string maps back to the right enum value.    */

typedef enum {
  GAME_DATABASE_TRAIT_FORCE_INTERPRETER,
  GAME_DATABASE_TRAIT_FORCE_SOFTWARE_RENDERER,
  GAME_DATABASE_TRAIT_FORCE_SOFTWARE_RENDERER_FOR_READBACKS,
     /*
     but the YAML resource file uses the literal "ForceRoundTextureCoordinates"
     (see s_trait_names[]).  We keep the long, descriptive enum name for
     readability; the YAML <-> enum mapping is via the table, not the name. */
  GAME_DATABASE_TRAIT_FORCE_ROUND_UPSCALED_TEXTURE_COORDINATES,
  GAME_DATABASE_TRAIT_FORCE_SHADER_BLENDING,
  GAME_DATABASE_TRAIT_FORCE_FULL_TRUE_COLOR,
  GAME_DATABASE_TRAIT_FORCE_DEINTERLACING,
  GAME_DATABASE_TRAIT_FORCE_FULL_BOOT,
  GAME_DATABASE_TRAIT_DISABLE_AUTO_ANALOG_MODE,
  GAME_DATABASE_TRAIT_DISABLE_MULTITAP,
  GAME_DATABASE_TRAIT_DISABLE_FAST_FORWARD_MEMORY_CARD_ACCESS,
  GAME_DATABASE_TRAIT_DISABLE_CDROM_READ_SPEEDUP,
  GAME_DATABASE_TRAIT_DISABLE_CDROM_SEEK_SPEEDUP,
  GAME_DATABASE_TRAIT_DISABLE_CDROM_SPEEDUP_ON_MDEC,
  GAME_DATABASE_TRAIT_DISABLE_TRUE_COLOR,
  GAME_DATABASE_TRAIT_DISABLE_FULL_TRUE_COLOR,
  GAME_DATABASE_TRAIT_DISABLE_UPSCALING,
  GAME_DATABASE_TRAIT_DISABLE_TEXTURE_FILTERING,
  GAME_DATABASE_TRAIT_DISABLE_SPRITE_TEXTURE_FILTERING,
  GAME_DATABASE_TRAIT_DISABLE_SCALED_DITHERING,
  GAME_DATABASE_TRAIT_DISABLE_SCALED_INTERLACING,
  GAME_DATABASE_TRAIT_DISABLE_ALL_BORDERS_CROP,
  GAME_DATABASE_TRAIT_DISABLE_WIDESCREEN,
  GAME_DATABASE_TRAIT_DISABLE_PGXP,
  GAME_DATABASE_TRAIT_DISABLE_PGXP_CULLING,
  GAME_DATABASE_TRAIT_DISABLE_PGXP_TEXTURE_CORRECTION,
  GAME_DATABASE_TRAIT_DISABLE_PGXP_COLOR_CORRECTION,
  GAME_DATABASE_TRAIT_DISABLE_PGXP_DEPTH_BUFFER,
  GAME_DATABASE_TRAIT_DISABLE_PGXP_ON_2D_POLYGONS,
  GAME_DATABASE_TRAIT_FORCE_PGXP_VERTEX_CACHE,
  GAME_DATABASE_TRAIT_FORCE_PGXP_CPU_MODE,
  GAME_DATABASE_TRAIT_FORCE_RECOMPILER_ICACHE,
  GAME_DATABASE_TRAIT_FORCE_CDROM_SUBQ_SKEW,
  GAME_DATABASE_TRAIT_IS_LIBCRYPT_PROTECTED,

  GAME_DATABASE_TRAIT_COUNT
} game_database_trait_t;

/* Language                                                               */
/* Order MUST match s_language_names[] in game_database.c. */

typedef enum {
  GAME_DATABASE_LANGUAGE_CATALAN,
  GAME_DATABASE_LANGUAGE_CHINESE,
  GAME_DATABASE_LANGUAGE_CZECH,
  GAME_DATABASE_LANGUAGE_DANISH,
  GAME_DATABASE_LANGUAGE_DUTCH,
  GAME_DATABASE_LANGUAGE_ENGLISH,
  GAME_DATABASE_LANGUAGE_FINNISH,
  GAME_DATABASE_LANGUAGE_FRENCH,
  GAME_DATABASE_LANGUAGE_GERMAN,
  GAME_DATABASE_LANGUAGE_GREEK,
  GAME_DATABASE_LANGUAGE_HEBREW,
  GAME_DATABASE_LANGUAGE_IRANIAN,
  GAME_DATABASE_LANGUAGE_ITALIAN,
  GAME_DATABASE_LANGUAGE_JAPANESE,
  GAME_DATABASE_LANGUAGE_KOREAN,
  GAME_DATABASE_LANGUAGE_NORWEGIAN,
  GAME_DATABASE_LANGUAGE_POLISH,
  GAME_DATABASE_LANGUAGE_PORTUGUESE,
  GAME_DATABASE_LANGUAGE_RUSSIAN,
  GAME_DATABASE_LANGUAGE_SPANISH,
  GAME_DATABASE_LANGUAGE_SWEDISH,
  GAME_DATABASE_LANGUAGE_TURKISH,

  GAME_DATABASE_LANGUAGE_COUNT
} game_database_language_t;

/* Sentinel masks                                                         */

/* Bit set in `supported_controllers` to indicate the entry permits */
#define GAME_DATABASE_SUPPORTS_MULTITAP_BIT ((u16)(1u << (u16)CONTROLLER_TYPE_COUNT))

#define GAME_DATABASE_TRAIT_BYTES    (((GAME_DATABASE_TRAIT_COUNT)    + 7) / 8)
#define GAME_DATABASE_LANGUAGE_BYTES (((GAME_DATABASE_LANGUAGE_COUNT) + 7) / 8)

/* Disc set entry                                                         */

typedef struct {
  const char* title;            u32 title_len;
  const char* sort_title;       u32 sort_title_len;
  const char* localized_title;  u32 localized_title_len;
  const char* save_title;       u32 save_title_len;

  /* Parallel arrays: `serials[i]` (length `serial_lens[i]`) names the i-th
     disc.  Both arrays have `serials_count` entries.  Both heap-owned by the
     loader. */
  const char**  serials;
  u32*          serial_lens;
  u32           serials_count;
} game_database_disc_set_t;

/* Entry                                                                  */

typedef struct game_database_entry {
  /* Backing bytes live in s_state.db_data; do NOT free or NUL-terminate. */
  const char* serial;                        u32 serial_len;
  const char* title;                         u32 title_len;
  const char* sort_title;                    u32 sort_title_len;
  const char* localized_title;               u32 localized_title_len;
  const char* save_title;                    u32 save_title_len;
  const char* genre;                         u32 genre_len;
  const char* developer;                     u32 developer_len;
  const char* publisher;                     u32 publisher_len;
  const char* compatibility_version_tested;  u32 compatibility_version_tested_len;
  const char* compatibility_comments;        u32 compatibility_comments_len;

  const game_database_disc_set_t* disc_set;  /* NULL if not part of a set */

  u64 release_date;                          /* seconds since Unix epoch  */
  u8  min_players;
  u8  max_players;
  u8  min_blocks;
  u8  max_blocks;

  /* Bit i (i in [0, CONTROLLER_TYPE_COUNT)) set => entry supports the
     controller type with that enum value.  Bit at CONTROLLER_TYPE_COUNT is
     GAME_DATABASE_SUPPORTS_MULTITAP_BIT. */
  u16 supported_controllers;

  game_database_compat_rating_t compatibility;

  u8 trait_bits   [GAME_DATABASE_TRAIT_BYTES];
  u8 language_bits[GAME_DATABASE_LANGUAGE_BYTES];

  /* Optional overrides: each field has a `has_X` companion bool.  When false,
     the value field is undefined and ApplySettings should not touch the
     corresponding settings field. */
  bool has_display_active_start_offset;       s16 display_active_start_offset;
  bool has_display_active_end_offset;         s16 display_active_end_offset;
  bool has_display_line_start_offset;         s8  display_line_start_offset;
  bool has_display_line_end_offset;           s8  display_line_end_offset;

  bool has_display_crop_mode;                 display_crop_mode_t           display_crop_mode;
  bool has_display_deinterlacing_mode;        display_deinterlacing_mode_t  display_deinterlacing_mode;
  bool has_gpu_line_detect_mode;              gpu_line_detect_mode_t        gpu_line_detect_mode;

  bool has_cpu_overclock;                     u8  cpu_overclock;
  bool has_dma_max_slice_ticks;               u32 dma_max_slice_ticks;
  bool has_dma_halt_ticks;                    u32 dma_halt_ticks;
  bool has_cdrom_max_seek_speedup_cycles;     u32 cdrom_max_seek_speedup_cycles;
  bool has_cdrom_max_read_speedup_cycles;     u32 cdrom_max_read_speedup_cycles;
  bool has_gpu_fifo_size;                     u32 gpu_fifo_size;
  bool has_gpu_max_run_ahead;                 u32 gpu_max_run_ahead;

  bool has_gpu_pgxp_tolerance;                float gpu_pgxp_tolerance;
  bool has_gpu_pgxp_depth_threshold;          float gpu_pgxp_depth_threshold;
  bool has_gpu_pgxp_preserve_proj_fp;         bool  gpu_pgxp_preserve_proj_fp;
} game_database_entry_t;

/* TrackHashes (lazy-loaded)                                              */

typedef struct {
  /* Both heap-owned, NUL-terminated. */
  char* serial;
  char* revision_str;
  u32   revision;
} game_database_track_data_t;

/* Public API                                                             */

void game_database_ensure_loaded(void);

const game_database_entry_t* game_database_get_entry_for_serial(const char* serial);
const game_database_entry_t* game_database_get_entry_for_id(const char* code, u32 code_len);
const game_database_entry_t* game_database_get_entry_for_id_and_hash(const char* id, u64 hash);
const game_database_entry_t* game_database_get_entry_for_disc(struct cd_image* image);

/* Both return malloc'd, NUL-terminated strings.  Caller frees.  NULL on miss. */
char* game_database_get_serial_for_disc(struct cd_image* image);
char* game_database_get_serial_for_path(const char* path);

const char* game_database_get_trait_name             (game_database_trait_t t);
const char* game_database_get_trait_display_name     (game_database_trait_t t);
const char* game_database_get_compat_rating_name     (game_database_compat_rating_t r);
const char* game_database_get_compat_rating_display_name(game_database_compat_rating_t r);
const char* game_database_get_language_name          (game_database_language_t l);
const char* game_database_get_language_display_name  (game_database_language_t l);

bool game_database_parse_language_name    (const char* str, u32 len, game_database_language_t*     out);
bool game_database_parse_trait_name       (const char* str, u32 len, game_database_trait_t*        out);
bool game_database_parse_compat_rating_name(const char* str, u32 len, game_database_compat_rating_t* out);

void game_database_entry_apply_settings(const game_database_entry_t* e,
                                        settings_t* settings,
                                        bool display_osd_messages);

/* TrackHashes; lazy. */
void                              game_database_ensure_track_hashes_loaded(void);
const game_database_track_data_t* game_database_lookup_track_hash(const u8 md5[16]);

/* Inline helpers                                                         */

static inline bool game_database_entry_has_trait(const game_database_entry_t* e,
                                                 game_database_trait_t t)
{
  const u32 i = (u32)t;
  return (e->trait_bits[i >> 3] & (u8)(1u << (i & 7u))) != 0;
}

static inline bool game_database_entry_has_language(const game_database_entry_t* e,
                                                    game_database_language_t l)
{
  const u32 i = (u32)l;
  return (e->language_bits[i >> 3] & (u8)(1u << (i & 7u))) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_GAME_DATABASE_H */
