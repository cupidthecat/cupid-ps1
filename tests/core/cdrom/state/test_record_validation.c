/* cupid-ps1 CD FIFO, sector-buffer and interpolation-state validation. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/spu/spu.h"
#include "core/system/system.h"

enum { RECORD_CAPACITY = 1024 * 1024, AUDIO_CAPACITY = 88200 };
typedef struct {
  u8*    data;
  size_t size;
  u32    version;
} record_t;

static const u32 versions[] = {65, 66, 67, 70, 71, 72, 78, 79, 80, 81, 84, 85, 92, 95, 96, 97};
static size_t    payload_end(const record_t* record) {
  return record->size - (record->version >= 97 ? 1u : 0u);
}
static size_t ring_offset(const record_t* record) {
  return payload_end(record) - (record->version >= 79 ? 110u : 94u);
}

static void store32(u8* bytes, u32 value) {
  for (u32 i = 0; i < 4; i++)
    bytes[i] = (u8)(value >> (i * 8));
}
static u32 load32(const u8* bytes) {
  return (u32)bytes[0] | (u32)bytes[1] << 8 | (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}
static record_t save_record(u32 version) {
  record_t record = {.data = malloc(RECORD_CAPACITY), .version = version};
  EXPECT_NOT_NULL(record.data);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, record.data, RECORD_CAPACITY, version);
  EXPECT_TRUE(cdrom_do_state(&sw));
  record.size = state_wrapper_get_position(&sw);
  return record;
}
static void restore_record(const record_t* record) {
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, record->data, record->size, record->version);
  EXPECT_TRUE(cdrom_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), record->size);
}
static void begin(void) {
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  g_settings.cdrom_readahead_sectors = 0;
  spu_initialize();
  system_update_overclock();
  cdrom_initialize();
}
static void end(void) {
  cdrom_shutdown();
  spu_shutdown();
  finish_cpu();
}
static void reject_and_recover(const record_t* good, const u8* bad, size_t length) {
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bad, length, good->version);
  EXPECT_FALSE(cdrom_do_state(&sw));
  EXPECT_TRUE(state_wrapper_has_error(&sw));
  record_t unchanged = save_record(good->version);
  EXPECT_EQ(unchanged.size, good->size);
  EXPECT_EQ(memcmp(unchanged.data, good->data, good->size), 0);
  free(unchanged.data);
  restore_record(good);
  record_t recovered = save_record(good->version);
  EXPECT_EQ(recovered.size, good->size);
  EXPECT_EQ(memcmp(recovered.data, good->data, good->size), 0);
  free(recovered.data);
}
/* These versions share the FIFO/sector suffix. Empty records have no variable
 * sector or queue data in that suffix. Assert its layout before modifying it. */
static void check_empty_suffix(const record_t* record) {
  EXPECT_TRUE(record->size >= 110);
  for (size_t i = ring_offset(record) + 2; i < ring_offset(record) + 14; i += 4)
    EXPECT_EQ(load32(record->data + i), 0);
  for (size_t i = payload_end(record) - 80; i < payload_end(record); i += 4)
    EXPECT_EQ(load32(record->data + i), 0);
  EXPECT_EQ(record->data[ring_offset(record)], 0);
  EXPECT_EQ(record->data[ring_offset(record) + 1], 6);
}
static void invalid_audio(u32 count) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    check_empty_suffix(&good);
    u8* bad = malloc(RECORD_CAPACITY);
    EXPECT_NOT_NULL(bad);
    const u32 supplied = AUDIO_CAPACITY + 1;
    memcpy(bad, good.data, payload_end(&good) - 8);
    store32(bad + payload_end(&good) - 8, count);
    for (u32 i = 0; i < supplied; i++)
      store32(bad + payload_end(&good) - 4 + (size_t)i * 4, 0xA5000000u + i);
    store32(bad + payload_end(&good) - 4 + (size_t)supplied * 4, 0x12345678);
    memcpy(bad + payload_end(&good) + (size_t)supplied * 4, good.data + payload_end(&good),
           good.size - payload_end(&good));
    reject_and_recover(&good, bad, good.size + (size_t)supplied * 4);
    free(bad);
    free(good.data);
  }
  end();
}
TEST(CdStateRecord, audio_capacity_plus_one) {
  invalid_audio(AUDIO_CAPACITY + 1);
}
TEST(CdStateRecord, audio_count_maximum_integer) {
  invalid_audio(UINT32_MAX);
}

static void invalid_byte_queue(u32 queue) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    check_empty_suffix(&good);
    const size_t offset = ring_offset(&good) + 2 + queue * 4;
    u8*          bad    = malloc(good.size + 16);
    EXPECT_NOT_NULL(bad);
    memcpy(bad, good.data, offset);
    store32(bad + offset, 17);
    memset(bad + offset + 4, 0x57, 16);
    memcpy(bad + offset + 20, good.data + offset + 4, good.size - offset - 4);
    reject_and_recover(&good, bad, good.size + 16);
    free(bad);
    free(good.data);
  }
  end();
}
TEST(CdStateRecord, parameter_count) {
  invalid_byte_queue(0);
}
TEST(CdStateRecord, response_count) {
  invalid_byte_queue(1);
}
TEST(CdStateRecord, async_response_count) {
  invalid_byte_queue(2);
}

enum { READ_INDEX, WRITE_INDEX, SECTOR_SIZE, SECTOR_POSITION, RING_INDEX, PHASE_HIGH };
static void invalid_metadata(u32 kind) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    check_empty_suffix(&good);
    u8* bad = malloc(good.size);
    EXPECT_NOT_NULL(bad);
    memcpy(bad, good.data, good.size);
    switch (kind) {
      case READ_INDEX:
        store32(bad + payload_end(&good) - 80, 8);
        break;
      case WRITE_INDEX:
        store32(bad + payload_end(&good) - 76, UINT32_MAX);
        break;
      case SECTOR_SIZE:
        /* Equal cursors avoid copying an oversized payload even in the frozen
         * baseline; the declared size itself still violates the buffer bound. */
        store32(bad + payload_end(&good) - 72, 2341);
        store32(bad + payload_end(&good) - 68, 2341);
        break;
      case SECTOR_POSITION:
        store32(bad + payload_end(&good) - 68, 1);
        break;
      case RING_INDEX:
        bad[ring_offset(&good)] = 32;
        break;
      case PHASE_HIGH:
        bad[ring_offset(&good) + 1] = 255;
        break;
    }
    reject_and_recover(&good, bad, good.size);
    free(bad);
    free(good.data);
  }
  end();
}
TEST(CdStateRecord, read_buffer_index) {
  invalid_metadata(READ_INDEX);
}
TEST(CdStateRecord, write_buffer_index) {
  invalid_metadata(WRITE_INDEX);
}
TEST(CdStateRecord, oversized_sector) {
  invalid_metadata(SECTOR_SIZE);
}
TEST(CdStateRecord, sector_cursor) {
  invalid_metadata(SECTOR_POSITION);
}
TEST(CdStateRecord, interpolation_ring_index) {
  invalid_metadata(RING_INDEX);
}
TEST(CdStateRecord, bounded_interpolation_counter_preserved) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    check_empty_suffix(&good);
    for (u32 phase = 0; phase <= 13; phase++) {
      good.data[ring_offset(&good) + 1] = (u8)phase;
      restore_record(&good);
      record_t after = save_record(version);
      EXPECT_EQ(after.size, good.size);
      EXPECT_EQ(memcmp(after.data, good.data, good.size), 0);
      free(after.data);
    }
    good.data[ring_offset(&good) + 1] = 6;
    restore_record(&good);
    free(good.data);
  }
  end();
}
TEST(CdStateRecord, oversized_interpolation_counter) {
  invalid_metadata(PHASE_HIGH);
}

TEST(CdStateRecord, every_short_record_then_valid_restore) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    for (size_t size = 0; size < good.size; size++)
      reject_and_recover(&good, good.data, size);
    free(good.data);
  }
  end();
}
TEST(CdStateRecord, sticky_error_preserves_component) {
  begin();
  record_t good = save_record(SAVE_STATE_VERSION);
  for (u32 writing = 0; writing < 2; writing++) {
    state_wrapper_t sw;
    if (writing)
      state_wrapper_init_write(&sw, good.data, good.size, SAVE_STATE_VERSION);
    else
      state_wrapper_init_read(&sw, good.data, good.size, SAVE_STATE_VERSION);
    sw.error = true;
    EXPECT_FALSE(cdrom_do_state(&sw));
    EXPECT_TRUE(sw.error);
    record_t after = save_record(SAVE_STATE_VERSION);
    EXPECT_EQ(after.size, good.size);
    EXPECT_EQ(memcmp(after.data, good.data, good.size), 0);
    free(after.data);
  }
  free(good.data);
  end();
}
TEST(CdStateRecord, full_audio_capacity_and_offset_record) {
  begin();
  for (u32 vi = 0; vi < sizeof(versions) / sizeof(versions[0]); vi++) {
    const u32 version = versions[vi];
    record_t  good    = save_record(version);
    check_empty_suffix(&good);
    const size_t length = good.size + (size_t)AUDIO_CAPACITY * 4;
    u8*          full   = malloc(length + 11);
    EXPECT_NOT_NULL(full);
    memset(full, 0xA5, 11);
    memcpy(full + 11, good.data, payload_end(&good) - 8);
    store32(full + 11 + payload_end(&good) - 8, AUDIO_CAPACITY);
    for (u32 i = 0; i < AUDIO_CAPACITY; i++)
      store32(full + 11 + payload_end(&good) - 4 + (size_t)i * 4, (i & 0xFFFFu) | ((~i & 0xFFFFu) << 16));
    store32(full + 11 + length - (good.size - payload_end(&good)) - 4, 0x12345678);
    memcpy(full + 11 + payload_end(&good) + (size_t)AUDIO_CAPACITY * 4, good.data + payload_end(&good),
           good.size - payload_end(&good));
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, full, length + 11, version);
    state_wrapper_set_position(&sw, 11);
    EXPECT_TRUE(cdrom_do_state(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), length + 11);
    EXPECT_EQ(cdrom_dbg_requested_lba(), 0x12345678);
    record_t loaded = save_record(version);
    EXPECT_EQ(loaded.size, length);
    EXPECT_EQ(memcmp(loaded.data, full + 11, length), 0);
    for (u32 i = 0; i < AUDIO_CAPACITY; i++) {
      s16 left, right;
      cdrom_get_audio_frame(&left, &right);
      EXPECT_EQ(left, (s16)(u16)i);
      EXPECT_EQ(right, (s16)(u16)~i);
    }
    s16 left = 1, right = 1;
    cdrom_get_audio_frame(&left, &right);
    EXPECT_EQ(left, 0);
    EXPECT_EQ(right, 0);
    free(loaded.data);
    free(full);
    restore_record(&good);
    free(good.data);
  }
  end();
}

TEST(CdStateRecord, older_writes_preserve_live_timestamps) {
  begin();
  record_t before = save_record(SAVE_STATE_VERSION);
  for (u32 version = 65; version < 71; version++) {
    record_t older = save_record(version);
    free(older.data);
    record_t after = save_record(SAVE_STATE_VERSION);
    EXPECT_EQ(after.size, before.size);
    EXPECT_EQ(memcmp(after.data, before.data, before.size), 0);
    free(after.data);
  }
  free(before.data);
  end();
}

TEST(CdStateRecord, version97_appends_event_activity) {
  begin();
  cdrom_write_register(0, 0);
  cdrom_write_register(1, 1);
  record_t previous = save_record(96);
  record_t current  = save_record(97);
  EXPECT_EQ(current.size, previous.size + 1);
  EXPECT_EQ(memcmp(current.data, previous.data, previous.size), 0);
  EXPECT_EQ(current.data[current.size - 1], 1);
  restore_record(&current);
  record_t replay = save_record(97);
  EXPECT_EQ(replay.size, current.size);
  EXPECT_EQ(memcmp(replay.data, current.data, current.size), 0);
  free(replay.data);
  free(current.data);
  free(previous.data);
  end();
}

TEST(CdStateRecord, reserved_event_activity_bits) {
  begin();
  record_t good = save_record(97);
  u8*      bad  = malloc(good.size);
  EXPECT_NOT_NULL(bad);
  for (u32 bits = 0x10; bits <= 0x80; bits <<= 1) {
    memcpy(bad, good.data, good.size);
    bad[good.size - 1] |= (u8)bits;
    reject_and_recover(&good, bad, good.size);
  }
  free(bad);
  free(good.data);
  end();
}
