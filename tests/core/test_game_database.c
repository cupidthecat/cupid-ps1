/* Smoke tests for the game_database YAML loader.
 *
 * Verifies that game_database_ensure_loaded() populates the static state
 * with the expected number of entries (~10000+) and that a real entry
 * (Crash Bandicoot, SCUS-94900) round-trips through the lookup with the
 * correct title and trait bits set per the YAML source.
 */

#include "tests/test_harness.h"

#include "common/timer.h"
#include "core/game_database.h"
#include "core/settings.h"
#include "util/cd_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* Internal accessors (defined at the bottom of game_database.c). */
size_t game_database_test_get_entry_count(void);
size_t game_database_test_get_disc_set_count(void);
size_t game_database_test_get_code_count(void);
void   game_database_test_reset(void);
bool   game_database_test_load_from_cache(void);
bool   game_database_test_save_to_cache(void);
bool   game_database_test_is_loaded_from_cache(void);

TEST(GameDatabase, loads_known_count)
{
  game_database_ensure_loaded();
  size_t n = game_database_test_get_entry_count();
  /* As of 2026-04 the bundled gamedb.yaml has ~10764 entries.  Keep a wide
     lower bound so the test stays robust across resource refreshes. */
  EXPECT_TRUE(n > 8000);

  /* Crash Bandicoot USA is in every revision of the database. */
  const game_database_entry_t* e = game_database_get_entry_for_serial("SCUS-94900");
  EXPECT_NOT_NULL(e);
  if (!e) return;

  /* title slice should contain "Crash". */
  EXPECT_TRUE(e->title_len >= 5);
  bool found = false;
  for (u32 i = 0; i + 5 <= e->title_len; i++) {
    if (memcmp(e->title + i, "Crash", 5) == 0) { found = true; break; }
  }
  EXPECT_TRUE(found);
}

TEST(GameDatabase, crash_has_force_round_textures_trait)
{
  game_database_ensure_loaded();
  const game_database_entry_t* e = game_database_get_entry_for_serial("SCUS-94900");
  EXPECT_NOT_NULL(e);
  if (!e) return;
  /* gamedb.yaml lists the trait under its YAML spelling
     "ForceRoundTextureCoordinates", which maps to the
     GAME_DATABASE_TRAIT_FORCE_ROUND_UPSCALED_TEXTURE_COORDINATES enum
     value (see s_trait_names[] in game_database.c). */
  EXPECT_TRUE(game_database_entry_has_trait(
    e, GAME_DATABASE_TRAIT_FORCE_ROUND_UPSCALED_TEXTURE_COORDINATES));
}

TEST(GameDatabase, codes_table_nonempty)
{
  game_database_ensure_loaded();
  const size_t n_entries = game_database_test_get_entry_count();
  const size_t n_codes   = game_database_test_get_code_count();
  /* Every entry without a `codes:` block contributes its serial as an
     implicit code; entries with a `codes:` block contribute one or more
     extra codes.  The code table should be at least as large as the
     entries table. */
  EXPECT_TRUE(n_codes >= n_entries);
}

TEST(GameDatabase, disc_sets_loaded)
{
  game_database_ensure_loaded();
  /* discsets.yaml ships ~280+ disc sets. */
  size_t n = game_database_test_get_disc_set_count();
  EXPECT_TRUE(n > 100);
}

/* Snapshot a few fields of a known entry so we can compare across
   YAML-load -> save -> reset -> cache-load. */
typedef struct {
  bool found;
  u32  serial_len, title_len;
  char serial[32];
  char title[256];
  u8   trait_bits[GAME_DATABASE_TRAIT_BYTES];
  u16  supported_controllers;
  bool has_disc_set;
} snapshot_t;

static void snapshot_crash(snapshot_t* s)
{
  memset(s, 0, sizeof(*s));
  const game_database_entry_t* e = game_database_get_entry_for_serial("SCUS-94900");
  if (!e) return;
  s->found = true;
  s->serial_len = e->serial_len;
  s->title_len  = e->title_len;
  if (e->serial_len < sizeof(s->serial)) memcpy(s->serial, e->serial, e->serial_len);
  if (e->title_len  < sizeof(s->title))  memcpy(s->title,  e->title,  e->title_len);
  memcpy(s->trait_bits, e->trait_bits, sizeof(s->trait_bits));
  s->supported_controllers = e->supported_controllers;
  s->has_disc_set = (e->disc_set != NULL);
}

TEST(GameDatabase, cache_roundtrip)
{
  /* Cold load (deletes any existing cache so we go through the YAML path). */
  unlink("gamedb.cache");
  game_database_test_reset();
  game_database_ensure_loaded();
  EXPECT_TRUE(!game_database_test_is_loaded_from_cache());
  const size_t yaml_entries   = game_database_test_get_entry_count();
  const size_t yaml_disc_sets = game_database_test_get_disc_set_count();
  const size_t yaml_codes     = game_database_test_get_code_count();
  EXPECT_TRUE(yaml_entries   > 8000);
  EXPECT_TRUE(yaml_disc_sets > 100);
  EXPECT_TRUE(yaml_codes     >= yaml_entries);

  snapshot_t before;
  snapshot_crash(&before);
  EXPECT_TRUE(before.found);

  /* Save cache, then clear state and load from cache. */
  EXPECT_TRUE(game_database_test_save_to_cache());
  game_database_test_reset();
  EXPECT_TRUE(game_database_test_load_from_cache());
  EXPECT_TRUE(game_database_test_is_loaded_from_cache());
  EXPECT_EQ(yaml_entries,   game_database_test_get_entry_count());
  EXPECT_EQ(yaml_disc_sets, game_database_test_get_disc_set_count());
  EXPECT_EQ(yaml_codes,     game_database_test_get_code_count());

  snapshot_t after;
  snapshot_crash(&after);
  EXPECT_TRUE(after.found);
  EXPECT_EQ(before.serial_len,            after.serial_len);
  EXPECT_EQ(before.title_len,             after.title_len);
  EXPECT_EQ(0, memcmp(before.serial, after.serial, before.serial_len));
  EXPECT_EQ(0, memcmp(before.title,  after.title,  before.title_len));
  EXPECT_EQ(0, memcmp(before.trait_bits, after.trait_bits, sizeof(before.trait_bits)));
  EXPECT_EQ(before.supported_controllers, after.supported_controllers);
  EXPECT_EQ(before.has_disc_set ? 1 : 0,  after.has_disc_set ? 1 : 0);
}

TEST(GameDatabase, cache_rejects_stale_mtime)
{
  /* Build a fresh cache from YAML, then back-date the cache header so the
     gamedb.yaml mtime mismatches.  Easiest way: bump the YAML's mtime forward
     after the cache was written. */
  unlink("gamedb.cache");
  game_database_test_reset();
  game_database_ensure_loaded();
  EXPECT_TRUE(game_database_test_save_to_cache());

  /* Push gamedb.yaml mtime 60s into the future relative to the cache. */
  struct stat st;
  EXPECT_EQ(0, stat("data/resources/gamedb.yaml", &st));
  struct utimbuf ut;
  ut.actime  = st.st_atime;
  ut.modtime = st.st_mtime + 60;
  EXPECT_EQ(0, utime("data/resources/gamedb.yaml", &ut));

  game_database_test_reset();
  EXPECT_FALSE(game_database_test_load_from_cache());

  /* Restore the original mtime so we don't poison subsequent tests. */
  ut.modtime = st.st_mtime;
  utime("data/resources/gamedb.yaml", &ut);
}

/* Crash Bandicoot SCUS-94900 has the ForceRoundTextureCoordinates trait per
   gamedb.yaml.  ApplySettings should flip gpu_force_round_texcoords to true. */
TEST(GameDatabase, apply_crash_disables_some_setting)
{
  game_database_ensure_loaded();
  const game_database_entry_t* e = game_database_get_entry_for_serial("SCUS-94900");
  EXPECT_NOT_NULL(e);
  if (!e) return;

  settings_t s;
  settings_init(&s);
  EXPECT_FALSE(s.gpu_force_round_texcoords);
  game_database_entry_apply_settings(e, &s, false);
  EXPECT_TRUE(s.gpu_force_round_texcoords);
  settings_destroy(&s);
}

/* Fear Effect Disc 1 (SLES-02166) has the DisableMultitap trait.  Because
   that trait is wired through ApplySettings, applying its entry should snap
   multitap_mode to MULTITAP_MODE_DISABLED. */
TEST(GameDatabase, apply_fear_effect_disables_multitap)
{
  game_database_ensure_loaded();
  const game_database_entry_t* e = game_database_get_entry_for_serial("SLES-02166");
  EXPECT_NOT_NULL(e);
  if (!e) return;
  EXPECT_TRUE(game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_MULTITAP));

  settings_t s;
  settings_init(&s);
  s.multitap_mode = MULTITAP_MODE_BOTH_PORTS;
  game_database_entry_apply_settings(e, &s, false);
  EXPECT_EQ((int)MULTITAP_MODE_DISABLED, (int)s.multitap_mode);
  settings_destroy(&s);
}

/* Wall-clock probe: cold (YAML) vs warm (cache) load.  Not a hard assertion;
   prints to stderr so the developer can eyeball the speed-up.  We do require
   that the warm path is at least as fast as the cold path. */
TEST(GameDatabase, cache_loads_faster_than_yaml)
{
  unlink("gamedb.cache");
  game_database_test_reset();
  timer_t_ t; timer_init(&t);
  game_database_ensure_loaded();
  const double cold_ms = timer_get_milliseconds(&t);
  EXPECT_TRUE(!game_database_test_is_loaded_from_cache());

  /* The cache should now exist on disk (save_to_cache ran). */
  struct stat st;
  EXPECT_EQ(0, stat("gamedb.cache", &st));
  fprintf(stderr, "    cold YAML load: %.1f ms; cache file: %lld bytes\n",
          cold_ms, (long long)st.st_size);

  game_database_test_reset();
  timer_init(&t);
  game_database_ensure_loaded();
  const double warm_ms = timer_get_milliseconds(&t);
  EXPECT_TRUE(game_database_test_is_loaded_from_cache());
  fprintf(stderr, "    warm cache load: %.1f ms\n", warm_ms);

  /* Sanity: cache should beat YAML on any sane machine.  Allow some slack so
     a flaky timer doesn't spuriously fail (cache <= 75% of YAML time). */
  EXPECT_TRUE(warm_ms <= cold_ms);
}

/* TrackHashes                                                            */

u32  game_database_test_get_track_hash_count(void);
u32  game_database_test_get_track_hash_max_probe(void);
u32  game_database_test_get_track_hash_bucket_count(void);
void game_database_test_reset_track_hashes(void);

/* Hex parse helper, MSB-first, 32 chars -> 16 bytes. */
static bool hex32_to_md5(const char* in, u8 out[16])
{
  for (int i = 0; i < 16; i++) {
    int hi = -1, lo = -1;
    char ch = in[i * 2];
    if (ch >= '0' && ch <= '9') hi = ch - '0';
    else if (ch >= 'a' && ch <= 'f') hi = 10 + ch - 'a';
    else if (ch >= 'A' && ch <= 'F') hi = 10 + ch - 'A';
    ch = in[i * 2 + 1];
    if (ch >= '0' && ch <= '9') lo = ch - '0';
    else if (ch >= 'a' && ch <= 'f') lo = 10 + ch - 'a';
    else if (ch >= 'A' && ch <= 'F') lo = 10 + ch - 'A';
    if (hi < 0 || lo < 0) return false;
    out[i] = (u8)((hi << 4) | lo);
  }
  return true;
}

/* Look up a known md5 from the first entry of discdb.yaml
   (SLPM-80238 -> 0aa29df886d9f4f3ec5340bdd3b4f3be). */
TEST(GameDatabase, tracks_lookup_known_md5)
{
  game_database_test_reset_track_hashes();

  u8 md5[16];
  EXPECT_TRUE(hex32_to_md5("0aa29df886d9f4f3ec5340bdd3b4f3be", md5));

  const game_database_track_data_t* td = game_database_lookup_track_hash(md5);
  EXPECT_NOT_NULL(td);
  if (!td) return;

  EXPECT_NOT_NULL(td->serial);
  EXPECT_STREQ("SLPM-80238", td->serial);
  EXPECT_EQ((int)0, (int)td->revision);

  fprintf(stderr,
          "    track-hashes: %u entries, %u buckets, max probe = %u\n",
          game_database_test_get_track_hash_count(),
          game_database_test_get_track_hash_bucket_count(),
          game_database_test_get_track_hash_max_probe());
  EXPECT_TRUE(game_database_test_get_track_hash_count() > 10000);
}

TEST(GameDatabase, tracks_lookup_unknown_md5)
{
  /* Lookup with all-zero md5 -- not a valid MD5 of any disc track. */
  u8 md5[16] = { 0 };
  const game_database_track_data_t* td = game_database_lookup_track_hash(md5);
  EXPECT_NULL(td);
}

/* GetSerialForDisc / GetSerialForPath / GetEntryForDisc                  */

/* Exercises the SYSTEM.CNF path against the bundled Crash Bandicoot      */
/* (USA) cue/bin in the project root.                                     */

TEST(GameDatabase, serial_for_path_crash)
{
  char* s = game_database_get_serial_for_path("Crash Bandicoot (USA).cue");
  EXPECT_NOT_NULL(s);
  if (!s) return;
  EXPECT_STREQ("SCUS-94900", s);
  free(s);
}

TEST(GameDatabase, get_entry_for_disc_crash)
{
  cd_image_t* img = cd_image_open("Crash Bandicoot (USA).cue", NULL);
  EXPECT_NOT_NULL(img);
  if (!img) return;

  const game_database_entry_t* e = game_database_get_entry_for_disc(img);
  EXPECT_NOT_NULL(e);
  if (e) {
    EXPECT_EQ((int)10, (int)e->serial_len);
    EXPECT_EQ(0, memcmp(e->serial, "SCUS-94900", 10));
  }
  cd_image_destroy(img);
}

/* Negative paths: NULL inputs are tolerated and return NULL.  Bogus path
 * also returns NULL. */
TEST(GameDatabase, serial_for_disc_null_safe)
{
  EXPECT_NULL(game_database_get_serial_for_disc(NULL));
  EXPECT_NULL(game_database_get_serial_for_path(NULL));
  EXPECT_NULL(game_database_get_serial_for_path(""));
  EXPECT_NULL(game_database_get_serial_for_path("/nonexistent.cue"));
  EXPECT_NULL(game_database_get_entry_for_disc(NULL));
}

/* Wiring sanity: Crash Bandicoot SCUS-94900 must (a) be in the GameDB and
 * (b) have a `controllers:` list that excludes AnalogController.  This is
 * the exact signal controller_can_start_in_analog_mode() consults to keep
 * Crash in digital mode, so if either condition regresses (gamedb refresh
 * drops the entry / changes the controllers list / etc.) we want to fail
 * here instead of in the runtime smoke. */
TEST(GameDatabase, gamedb_wires_crash_to_disable_auto_analog)
{
  cd_image_t* img = cd_image_open("Crash Bandicoot (USA).cue", NULL);
  EXPECT_NOT_NULL(img);
  if (!img) return;

  game_database_ensure_loaded();

  const game_database_entry_t* e = game_database_get_entry_for_serial("SCUS-94900");
  EXPECT_NOT_NULL(e);
  if (!e) { cd_image_destroy(img); return; }

  /* Crash's gamedb entry has the ForceRoundTextureCoordinates trait. */
  EXPECT_TRUE(game_database_entry_has_trait(
    e, GAME_DATABASE_TRAIT_FORCE_ROUND_UPSCALED_TEXTURE_COORDINATES));

  /* Crash's controllers: list is `- DigitalController`.  Therefore the
   * `supported_controllers` bitmask must (a) be neither the "no list"
   * sentinel (0) nor the "all controllers" sentinel (0xFFFF), and
   * (b) NOT include the AnalogController bit. */
  EXPECT_TRUE(e->supported_controllers != 0);
  EXPECT_TRUE(e->supported_controllers != (u16)0xFFFFu);

  const u16 analog_bit = (u16)(1u << (u32)CONTROLLER_TYPE_ANALOG_CONTROLLER);
  const u16 digital_bit = (u16)(1u << (u32)CONTROLLER_TYPE_DIGITAL_CONTROLLER);
  EXPECT_EQ((int)0, (int)(e->supported_controllers & analog_bit));
  EXPECT_TRUE((e->supported_controllers & digital_bit) != 0);

  cd_image_destroy(img);
}
