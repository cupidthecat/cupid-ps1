/* cupid-ps1 generated legacy CD record fixtures and conversion checks. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/spu/spu.h"
#include "core/system/system.h"

enum { LEGACY_CAPACITY = 128 * 1024, LEGACY_SECTOR_BYTES = 2340 };
typedef struct {
  u8*    data;
  size_t size;
  u32    version;
  size_t predictors, old_count, read_index, write_index, sector_data[8], sector_size[8], audio_count;
} legacy_t;
static void legacy_u8(legacy_t* r, u8 value) {
  EXPECT_TRUE(r->size < LEGACY_CAPACITY);
  r->data[r->size++] = value;
}
static void legacy_u16(legacy_t* r, u16 value) {
  legacy_u8(r, (u8)value);
  legacy_u8(r, (u8)(value >> 8));
}
static void legacy_u32(legacy_t* r, u32 value) {
  for (u32 i = 0; i < 4; i++)
    legacy_u8(r, (u8)(value >> (i * 8)));
}
static void legacy_zeros(legacy_t* r, u32 count) {
  while (count--)
    legacy_u8(r, 0);
}
static u8 legacy_sector_byte(u32 sector, u32 offset) {
  return (u8)(offset * 3 + sector * 29 + 7);
}
/* This writer states the historical wire layout independently of the current
 * serializer. It does not call the production legacy WRITE path. */
static legacy_t legacy_fixture(u32 version, u32 old_bytes) {
  legacy_t r = {.data = malloc(LEGACY_CAPACITY), .version = version};
  EXPECT_NOT_NULL(r.data);
  legacy_u16(&r, 0xFFFF); /* No pending command. */
  if (version >= 53) legacy_u16(&r, 0xFFFF);
  legacy_u8(&r, 0);
  legacy_u8(&r, 0x18);
  legacy_u8(&r, 0x10);
  legacy_u8(&r, 0x20);
  legacy_u8(&r, 0); /* Historical double-speed flag. */
  legacy_u8(&r, 0x1F);
  legacy_u8(&r, 0);
  if (version >= 57) legacy_u32(&r, 0);
  legacy_u8(&r, 0);
  legacy_zeros(&r, 3);
  legacy_u32(&r, 15);
  legacy_u32(&r, 15);
  legacy_u32(&r, 15);
  if (version >= 49) {
    legacy_u32(&r, 15);
    legacy_u32(&r, 0);
  }
  if (version >= 54) legacy_u32(&r, 0);
  legacy_zeros(&r, 5 + 4 + 1);      /* Seek/audio flags, XA file/channel and latch. */
  legacy_zeros(&r, 4 + 4 + 1 + 12); /* Header, subheader, valid flag and SubQ. */
  legacy_u8(&r, 0xFF);
  legacy_u8(&r, 0);
  legacy_u8(&r, 0);
  if (version >= 49) legacy_u8(&r, 0);
  for (u32 matrix = 0; matrix < 2; matrix++) {
    legacy_u8(&r, 128);
    legacy_u8(&r, 0);
    legacy_u8(&r, 0);
    legacy_u8(&r, 128);
  }
  r.predictors = r.size;
  legacy_zeros(&r, 4 * 4 + 2 * 32 * 2); /* Predictors and sample history. */
  legacy_u8(&r, 0);
  legacy_u8(&r, 6);
  legacy_u32(&r, 0);
  legacy_u32(&r, 0);
  legacy_u32(&r, 0);
  r.old_count = r.size;
  legacy_u32(&r, old_bytes);
  for (u32 i = 0; i < old_bytes; i++)
    legacy_u8(&r, 0xE3);
  r.read_index = r.size;
  legacy_u32(&r, 2);
  r.write_index = r.size;
  legacy_u32(&r, 5);
  for (u32 sector = 0; sector < 8; sector++) {
    r.sector_data[sector] = r.size;
    for (u32 i = 0; i < LEGACY_SECTOR_BYTES; i++)
      legacy_u8(&r, legacy_sector_byte(sector, i));
    r.sector_size[sector] = r.size;
    legacy_u32(&r, LEGACY_SECTOR_BYTES);
  }
  r.audio_count = r.size;
  legacy_u32(&r, 3);
  legacy_u32(&r, 0x1234FEDC);
  legacy_u32(&r, 0x80007FFF);
  legacy_u32(&r, 0xFFFF0001);
  legacy_u32(&r, 37);
  return r;
}
static void legacy_start(void) {
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  g_settings.cdrom_readahead_sectors = 0;
  spu_initialize();
  system_update_overclock();
  cdrom_initialize();
}
static void legacy_end(void) {
  cdrom_shutdown();
  spu_shutdown();
  finish_cpu();
}
static void legacy_load(const legacy_t* r) {
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, r->data, r->size, r->version);
  EXPECT_TRUE(cdrom_do_state(&sw));
  EXPECT_FALSE(sw.error);
  EXPECT_EQ(state_wrapper_get_position(&sw), r->size);
}
static legacy_t canonical_snapshot(void) {
  legacy_t r = {.data = malloc(LEGACY_CAPACITY), .version = SAVE_STATE_VERSION};
  EXPECT_NOT_NULL(r.data);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, r.data, LEGACY_CAPACITY, r.version);
  EXPECT_TRUE(cdrom_do_state(&sw));
  r.size = state_wrapper_get_position(&sw);
  return r;
}
static void canonical_equal(const legacy_t* saved) {
  legacy_load(saved);
  legacy_t after = canonical_snapshot();
  EXPECT_EQ(after.size, saved->size);
  EXPECT_EQ(memcmp(after.data, saved->data, saved->size), 0);
  free(after.data);
}
TEST(CdLegacyRecord, all_supported_fixed_layout_versions) {
  _Static_assert(sizeof(cd_image_subq_t) == 12, "Legacy subchannel record is twelve bytes");
  legacy_start();
  const u32 old_sizes[] = {0, 13, 2048, 2340};
  for (u32 version = 42; version < 65; version++)
    for (u32 n = 0; n < 4; n++) {
      const u32 old_bytes = old_sizes[n];
      legacy_t  r         = legacy_fixture(version, old_bytes);
      legacy_load(&r);
      EXPECT_EQ(cdrom_dbg_current_lba(), 15);
      EXPECT_EQ(cdrom_dbg_requested_lba(), 37);
      legacy_t current = canonical_snapshot();
      canonical_equal(&current);
      free(current.data);
      const u32 frames[] = {0x1234FEDC, 0x80007FFF, 0xFFFF0001};
      for (u32 i = 0; i < 3; i++) {
        s16 left, right;
        cdrom_get_audio_frame(&left, &right);
        EXPECT_EQ(left, (s16)(u16)frames[i]);
        EXPECT_EQ(right, (s16)(u16)(frames[i] >> 16));
      }
      s16 left = 1, right = 1;
      cdrom_get_audio_frame(&left, &right);
      EXPECT_EQ(left, 0);
      EXPECT_EQ(right, 0);
      const bool readable = old_bytes > 0 && old_bytes < LEGACY_SECTOR_BYTES;
      EXPECT_EQ((cdrom_read_register(0) >> 6) & 1, readable);
      if (readable)
        for (u32 i = LEGACY_SECTOR_BYTES - old_bytes; i < LEGACY_SECTOR_BYTES; i++)
          EXPECT_EQ(cdrom_read_register(2), legacy_sector_byte(2, i));
      free(r.data);
    }
  legacy_end();
}
static void legacy_reject(const legacy_t* bad, size_t length, const legacy_t* good) {
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bad->data, length, bad->version);
  EXPECT_FALSE(cdrom_do_state(&sw));
  EXPECT_TRUE(sw.error);
  legacy_t unchanged = canonical_snapshot();
  EXPECT_EQ(unchanged.size, good->size);
  EXPECT_EQ(memcmp(unchanged.data, good->data, good->size), 0);
  free(unchanged.data);
  canonical_equal(good);
}
TEST(CdLegacyRecord, malformed_fixed_buffer_metadata) {
  legacy_start();
  for (u32 version = 42; version < 65; version++) {
    legacy_t valid = legacy_fixture(version, 0);
    legacy_load(&valid);
    legacy_t good = canonical_snapshot();
    for (u32 kind = 0; kind < 4; kind++) {
      legacy_t     bad    = legacy_fixture(version, kind == 0 ? 2341 : 0);
      const size_t offset = kind == 1 ? bad.read_index : kind == 2 ? bad.write_index : bad.sector_size[3];
      if (kind != 0) {
        const u32 value = kind == 3 ? 2341 : 8;
        for (u32 i = 0; i < 4; i++)
          bad.data[offset + i] = (u8)(value >> (i * 8));
      }
      legacy_reject(&bad, bad.size, &good);
      free(bad.data);
    }
    free(good.data);
    free(valid.data);
  }
  legacy_end();
}
TEST(CdLegacyRecord, xa_predictor_limits_preserved) {
  legacy_start();
  legacy_t empty = canonical_snapshot();
  /* Locate the fixed prefix with empty sector and audio queues. Legacy
   * conversion below appends variable data without moving that prefix. */
  const size_t predictors = empty.size - (9 + 128 + 2 + 12 + 16 + 72 + 16);
  for (u32 i = 0; i < 16; i++)
    EXPECT_EQ(empty.data[predictors + i], 0);
  free(empty.data);
  const s32 samples[] = {-32768, -1, 0, 1, 32767};
  for (u32 version = 42; version < 65; version++) {
    legacy_t valid = legacy_fixture(version, 0);
    for (u32 n = 0; n < sizeof(samples) / sizeof(samples[0]); n++) {
      for (u32 slot = 0; slot < 4; slot++)
        for (u32 byte = 0; byte < 4; byte++)
          valid.data[valid.predictors + slot * 4 + byte] = (u8)((u32)samples[(n + slot) % 5] >> (byte * 8));
      legacy_load(&valid);
      legacy_t current = canonical_snapshot();
      EXPECT_EQ(memcmp(current.data + predictors, valid.data + valid.predictors, 16), 0);
      canonical_equal(&current);
      free(current.data);
    }
    free(valid.data);
  }
  legacy_end();
}

TEST(CdLegacyRecord, invalid_xa_predictors_rejected) {
  legacy_start();
  const s32 invalid[] = {-32769, 32768, INT32_MIN, INT32_MAX};
  for (u32 version = 42; version < 65; version++) {
    legacy_t valid = legacy_fixture(version, 0);
    legacy_load(&valid);
    legacy_t good = canonical_snapshot();
    for (u32 channel = 0; channel < 4; channel++)
      for (u32 n = 0; n < sizeof(invalid) / sizeof(invalid[0]); n++) {
        for (u32 i = 0; i < 4; i++)
          valid.data[valid.predictors + channel * 4 + i] = (u8)((u32)invalid[n] >> (i * 8));
        legacy_reject(&valid, valid.size, &good);
        memset(valid.data + valid.predictors + channel * 4, 0, 4);
      }
    free(good.data);
    free(valid.data);
  }
  legacy_end();
}

TEST(CdLegacyRecord, truncated_fixed_payloads_then_valid_restore) {
  legacy_start();
  for (u32 version = 42; version < 65; version++) {
    legacy_t r = legacy_fixture(version, 13);
    legacy_load(&r);
    legacy_t good = canonical_snapshot();
    legacy_reject(&r, 0, &good);
    legacy_reject(&r, r.old_count + 3, &good);
    legacy_reject(&r, r.read_index - 1, &good);
    legacy_reject(&r, r.write_index + 3, &good);
    for (u32 i = 0; i < 8; i++) {
      legacy_reject(&r, r.sector_data[i] + LEGACY_SECTOR_BYTES - 1, &good);
      legacy_reject(&r, r.sector_size[i] + 3, &good);
    }
    legacy_reject(&r, r.audio_count + 3, &good);
    legacy_reject(&r, r.size - 1, &good);
    free(good.data);
    free(r.data);
  }
  legacy_end();
}

TEST(CdLegacyRecord, writes_preserve_loaded_partial_sector) {
  legacy_start();
  legacy_t fixture = legacy_fixture(64, 13);
  legacy_load(&fixture);
  legacy_t before = canonical_snapshot();
  u8*      output = malloc(LEGACY_CAPACITY);
  EXPECT_NOT_NULL(output);
  for (u32 version = 42; version < 71; version++) {
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, output, LEGACY_CAPACITY, version);
    const bool success = cdrom_do_state(&sw);
    if (version >= 65) EXPECT_TRUE(success);
    EXPECT_EQ(success, !sw.error);
    legacy_t after = canonical_snapshot();
    EXPECT_EQ(after.size, before.size);
    EXPECT_EQ(memcmp(after.data, before.data, before.size), 0);
    free(after.data);
  }
  free(output);
  free(before.data);
  free(fixture.data);
  legacy_end();
}
