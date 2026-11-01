/* cupid-ps1 memory-card wire protocol, continuation and persistence. */
#include "tests/test_harness.h"
#include "core/cpu/cpu_core.h"
#include "core/config/settings.h"
#include "core/spu/spu.h"
#include "core/system/system.h"
#include "core/input/memory_cards/memory_card.h"
#include "core/state/save_state_version.h"
#include "common/error.h"
#include "common/file_system.h"
#include "util/state/state_wrapper.h"
#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>
#endif

static memory_card_t* card_begin(void)
{
  timing_events_initialize();
  memory_card_t* card = memory_card_create(0);
  EXPECT_NOT_NULL(card);
  card->changed = false;
  return card;
}

static void card_end(memory_card_t* card)
{
  memory_card_destroy(card);
  timing_events_shutdown();
}

static void exchange(memory_card_t* card, u8 sent, u8 expected, bool ack)
{
  u8 reply = 0xFF;
  const bool actual_ack = memory_card_transfer(card, sent, &reply);
  EXPECT_EQ(reply, expected);
  EXPECT_EQ(actual_ack, ack);
}

static void command(memory_card_t* card, u8 opcode, u16 address)
{
  memory_card_reset_transfer_state(card);
  exchange(card, 0x81, 0xFF, true);
  exchange(card, opcode, card->flag, true);
  exchange(card, 0, 0x5A, true);
  exchange(card, 0, 0x5D, true);
  exchange(card, (u8)(address >> 8), 0, true);
  exchange(card, (u8)address, (u8)(address >> 8), true);
}

static void payload(u8 bytes[128])
{
  for (u32 i = 0; i < 128; i++) bytes[i] = (u8)(i * 29 + 7);
}

static u8 write_data(memory_card_t* card, u16 address, const u8 bytes[128], u32 start)
{
  u8 checksum = (u8)((address >> 8) ^ address);
  for (u32 i = 0; i < 128; i++) {
    checksum ^= bytes[i];
    if (i >= start) exchange(card, bytes[i], i ? bytes[i - 1] : (u8)address, true);
  }
  return checksum;
}

static void write_finish(memory_card_t* card, u8 checksum, u8 last, u8 result)
{
  exchange(card, checksum, last, true);
  exchange(card, 0, 0x5C, true);
  exchange(card, 0, 0x5D, true);
  exchange(card, 0, result, false);
}

static void write_sector(memory_card_t* card, u16 address, const u8 bytes[128])
{
  command(card, 0x57, address);
  const u8 checksum = write_data(card, address, bytes, 0);
  write_finish(card, checksum, bytes[127], 0x47);
}

TEST(MemoryCardProtocol, read_end_has_no_ack)
{
  memory_card_t* card = card_begin();
  command(card, 0x52, 0x123);
  exchange(card, 0, 0x5C, true); exchange(card, 0, 0x5D, true);
  exchange(card, 0, 1, true); exchange(card, 0, 0x23, true);
  u8 checksum = 0x22;
  for (u32 i = 0; i < 128; i++) {
    const u8 byte = card->data[0x123 * 128 + i];
    exchange(card, 0, byte, true); checksum ^= byte;
  }
  exchange(card, 0, checksum, true);
  exchange(card, 0, 0x47, false);
  card_end(card);
}

TEST(MemoryCardProtocol, id_end_has_no_ack_and_returns_idle)
{
  memory_card_t* card = card_begin();
  exchange(card, 0x81, 0xFF, true); exchange(card, 0x53, 8, true);
  const u8 replies[] = {0x5A, 0x5D, 0x5C, 0x5D, 4, 0, 0, 0x80};
  for (u32 i = 0; i < sizeof(replies); i++) exchange(card, 0, replies[i], i + 1 < sizeof(replies));
  EXPECT_EQ(card->state, MEMORY_CARD_STATE_IDLE);
  card_end(card);
}

TEST(MemoryCardProtocol, checksum_reply_echoes_last_data)
{
  memory_card_t* card = card_begin(); u8 bytes[128]; payload(bytes);
  command(card, 0x57, 0x123);
  write_finish(card, write_data(card, 0x123, bytes, 0), bytes[127], 0x47);
  EXPECT_EQ(memcmp(card->data + 0x123 * 128, bytes, 128), 0);
  card_end(card);
}

TEST(MemoryCardProtocol, aborted_write_does_not_change_card)
{
  memory_card_t* card = card_begin(); u8 old[128];
  memcpy(old, card->data + 9 * 128, 128);
  command(card, 0x57, 9);
  for (u32 i = 0; i < 65; i++) {
    u8 ignored; EXPECT_TRUE(memory_card_transfer(card, (u8)i, &ignored));
  }
  memory_card_reset_transfer_state(card);
  EXPECT_EQ(memcmp(card->data + 9 * 128, old, 128), 0);
  EXPECT_FALSE(card->changed);
  EXPECT_EQ(card->flag, MEMORY_CARD_FLAG_NO_WRITE_YET);
  card_end(card);
}

TEST(MemoryCardProtocol, bad_checksum_does_not_commit)
{
  memory_card_t* card = card_begin(); u8 bytes[128], old[128]; payload(bytes);
  memcpy(old, card->data + 9 * 128, 128);
  command(card, 0x57, 9);
  const u8 checksum = write_data(card, 9, bytes, 0);
  write_finish(card, checksum ^ 1, bytes[127], 0x4E);
  EXPECT_EQ(memcmp(card->data + 9 * 128, old, 128), 0);
  EXPECT_FALSE(card->changed);
  EXPECT_EQ(card->flag, MEMORY_CARD_FLAG_NO_WRITE_YET);
  card_end(card);
}

TEST(MemoryCardProtocol, invalid_write_sector_does_not_wrap)
{
  memory_card_t* card = card_begin(); u8 bytes[128], old[128]; payload(bytes);
  memcpy(old, card->data, 128);
  command(card, 0x57, 0x400);
  write_finish(card, write_data(card, 0x400, bytes, 0), bytes[127], 0xFF);
  EXPECT_EQ(memcmp(card->data, old, 128), 0);
  EXPECT_FALSE(card->changed);
  card_end(card);
}

TEST(MemoryCardProtocol, invalid_read_sector_aborts_after_confirmed_address)
{
  memory_card_t* card = card_begin();
  command(card, 0x52, 0x400);
  exchange(card, 0, 0x5C, true); exchange(card, 0, 0x5D, true);
  exchange(card, 0, 0xFF, true); exchange(card, 0, 0xFF, false);
  EXPECT_EQ(card->state, MEMORY_CARD_STATE_IDLE);
  card_end(card);
}

TEST(MemoryCardProtocol, pending_sector_continues_after_state_restore)
{
  memory_card_t* card = card_begin(); u8 bytes[128]; payload(bytes);
  command(card, 0x57, 9);
  for (u32 i = 0; i < 65; i++) {
    u8 ignored; EXPECT_TRUE(memory_card_transfer(card, bytes[i], &ignored));
  }
  u8* saved = malloc(MEMORY_CARD_STATE_SIZE); EXPECT_NOT_NULL(saved);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, saved, MEMORY_CARD_STATE_SIZE, SAVE_STATE_VERSION);
  EXPECT_TRUE(memory_card_do_state(card, &sw));
  const size_t size = state_wrapper_get_position(&sw);
  write_finish(card, write_data(card, 9, bytes, 65), bytes[127], 0x47);
  memset(card->data + 9 * 128, 0xA5, 128);
  state_wrapper_init_read(&sw, saved, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(memory_card_do_state(card, &sw));
  EXPECT_EQ(card->sector_offset, 65);
  EXPECT_FALSE(card->changed);
  write_finish(card, write_data(card, 9, bytes, 65), bytes[127], 0x47);
  EXPECT_EQ(memcmp(card->data + 9 * 128, bytes, 128), 0);
  free(saved); card_end(card);
}

TEST(MemoryCardProtocol, invalid_state_enum_and_cursor_are_rejected)
{
  memory_card_t* card = card_begin();
  u8* saved = malloc(MEMORY_CARD_STATE_SIZE); EXPECT_NOT_NULL(saved);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, saved, MEMORY_CARD_STATE_SIZE, SAVE_STATE_VERSION);
  EXPECT_TRUE(memory_card_do_state(card, &sw));
  const size_t size = state_wrapper_get_position(&sw);
  saved[0] = 0xFF;
  state_wrapper_init_read(&sw, saved, size, SAVE_STATE_VERSION);
  EXPECT_FALSE(memory_card_do_state(card, &sw));
  saved[0] = MEMORY_CARD_STATE_READ_DATA; saved[4] = 128;
  state_wrapper_init_read(&sw, saved, size, SAVE_STATE_VERSION);
  EXPECT_FALSE(memory_card_do_state(card, &sw));
  free(saved); card_end(card);
}

TEST(MemoryCardProtocol, rejected_state_preserves_live_card)
{
  memory_card_t* card = card_begin();
  u8* saved = malloc(MEMORY_CARD_STATE_SIZE); EXPECT_NOT_NULL(saved);
  memory_card_t* before = malloc(sizeof(*before)); EXPECT_NOT_NULL(before);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, saved, MEMORY_CARD_STATE_SIZE, SAVE_STATE_VERSION);
  EXPECT_TRUE(memory_card_do_state(card, &sw));
  const size_t size = state_wrapper_get_position(&sw);
  memcpy(before, card, sizeof(*before));
  const size_t truncated[] = {0, 7, 1024, MEMORY_CARD_LEGACY_STATE_SIZE, MEMORY_CARD_STATE_SIZE - 1};
  for (u32 i = 0; i < countof(truncated); i++) {
    state_wrapper_init_read(&sw, saved, truncated[i], SAVE_STATE_VERSION);
    EXPECT_FALSE(memory_card_do_state(card, &sw));
    EXPECT_TRUE(state_wrapper_has_error(&sw));
    EXPECT_EQ(memcmp(before, card, sizeof(*before)), 0);
  }
  saved[0] = MEMORY_CARD_STATE_READ_DATA; saved[2] = 0; saved[3] = 4;
  state_wrapper_init_read(&sw, saved, size, SAVE_STATE_VERSION);
  EXPECT_FALSE(memory_card_do_state(card, &sw));
  EXPECT_EQ(memcmp(before, card, sizeof(*before)), 0);
  free(before); free(saved); card_end(card);
}

static u8* legacy_write_state(memory_card_t* card, const u8 data[128], memory_card_state_t phase, u8 offset)
{
  u8* saved = calloc(1, MEMORY_CARD_LEGACY_STATE_SIZE); EXPECT_NOT_NULL(saved);
  saved[0] = (u8)phase;
  saved[2] = 9;
  saved[4] = offset;
  memcpy(saved + 7, card->data, sizeof(card->data));
  const u32 committed = phase == MEMORY_CARD_STATE_WRITE_DATA ? offset : 128;
  memcpy(saved + 7 + 9 * 128, data, committed);
  saved[5] = 9;
  for (u32 i = 0; i < committed; i++) saved[5] ^= data[i];
  saved[6] = committed ? data[committed - 1] : 9;
  saved[MEMORY_CARD_LEGACY_STATE_SIZE - 1] = 1;
  return saved;
}

TEST(MemoryCardProtocol, legacy_partial_write_continues_from_committed_prefix)
{
  memory_card_t* card = card_begin(); u8 bytes[128]; payload(bytes);
  u8* saved = legacy_write_state(card, bytes, MEMORY_CARD_STATE_WRITE_DATA, 65);
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, saved, MEMORY_CARD_LEGACY_STATE_SIZE, 90);
  EXPECT_TRUE(memory_card_do_state(card, &sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), MEMORY_CARD_LEGACY_STATE_SIZE);
  EXPECT_EQ(memcmp(card->data + 9 * 128, bytes, 65), 0);
  write_finish(card, write_data(card, 9, bytes, 65), bytes[127], 0x47);
  EXPECT_EQ(memcmp(card->data + 9 * 128, bytes, 128), 0);
  free(saved); card_end(card);
}

TEST(MemoryCardProtocol, legacy_completed_write_phases_keep_committed_data)
{
  memory_card_t* card = card_begin(); u8 bytes[128]; payload(bytes);
  for (u32 phase = MEMORY_CARD_STATE_WRITE_CHECKSUM; phase <= MEMORY_CARD_STATE_WRITE_END; phase++) {
    u8* saved = legacy_write_state(card, bytes, (memory_card_state_t)phase, 0);
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, saved, MEMORY_CARD_LEGACY_STATE_SIZE, 90);
    EXPECT_TRUE(memory_card_do_state(card, &sw));
    u8 sum = 9; for (u32 i = 0; i < 128; i++) sum ^= bytes[i];
    if (phase == MEMORY_CARD_STATE_WRITE_CHECKSUM) exchange(card, sum, bytes[127], true);
    if (phase <= MEMORY_CARD_STATE_WRITE_ACK1) exchange(card, 0, 0x5C, true);
    if (phase <= MEMORY_CARD_STATE_WRITE_ACK2) exchange(card, 0, 0x5D, true);
    exchange(card, 0, 0x47, false);
    EXPECT_EQ(memcmp(card->data + 9 * 128, bytes, 128), 0);
    free(saved);
  }
  card_end(card);
}

TEST(MemoryCardPersistence, atomic_replacement_retains_open_reader)
{
  char* name = NULL;
  FILE* old = fs_open_temporary_file("cupid-card-save", 15, &name, NULL);
  EXPECT_NOT_NULL(old);
  EXPECT_EQ(fwrite("original", 1, 8, old), 8); EXPECT_TRUE(fs_flush_file(old, NULL));
  u8* image = malloc(MEMORY_CARD_IMAGE_DATA_SIZE); EXPECT_NOT_NULL(image);
  memory_card_image_format(image);
  EXPECT_TRUE(memory_card_image_save_to_file(image, name, NULL));
  EXPECT_EQ(fs_fseek64(old, 0, SEEK_SET), 0);
  char previous[9] = {0}; EXPECT_EQ(fread(previous, 1, 8, old), 8);
  EXPECT_STREQ(previous, "original"); fclose(old);
  u8* actual = malloc(MEMORY_CARD_IMAGE_DATA_SIZE); EXPECT_NOT_NULL(actual);
  EXPECT_TRUE(memory_card_image_load_from_file(actual, name, NULL));
  EXPECT_EQ(memcmp(image, actual, MEMORY_CARD_IMAGE_DATA_SIZE), 0);
  EXPECT_TRUE(fs_delete_file(name, NULL)); free(name); free(image); free(actual);
}

#if !defined(_WIN32)
TEST(MemoryCardPersistence, short_write_does_not_truncate_previous_image)
{
  char* name = NULL;
  FILE* original = fs_open_temporary_file("cupid-card-short", 16, &name, NULL);
  EXPECT_NOT_NULL(original);
  EXPECT_EQ(fwrite("preserved-card", 1, 14, original), 14);
  EXPECT_EQ(fclose(original), 0);
  u8* image = malloc(MEMORY_CARD_IMAGE_DATA_SIZE); EXPECT_NOT_NULL(image);
  memory_card_image_format(image);
  signal(SIGXFSZ, SIG_IGN);
  const struct rlimit limit = {64, 64};
  EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &limit), 0);
  Error error = ERROR_INIT;
  EXPECT_FALSE(memory_card_image_save_to_file(image, name, &error));
  EXPECT_TRUE(Error_is_valid(&error));
  original = fs_open_file(name, "rb", NULL); EXPECT_NOT_NULL(original);
  char bytes[15] = {0}; EXPECT_EQ(fread(bytes, 1, 14, original), 14); fclose(original);
  EXPECT_STREQ(bytes, "preserved-card");
  EXPECT_TRUE(fs_delete_file(name, NULL)); Error_destroy(&error); free(name); free(image);
}
#endif

TEST(MemoryCardPersistence, failed_save_remains_dirty_and_can_retry)
{
  /* Initialize the real system clock, then isolate the card's event queue. */
  settings_init(&g_settings);
  timing_events_initialize(); spu_initialize(); system_update_overclock();
  spu_shutdown(); timing_events_shutdown();
  EXPECT_EQ(system_get_ticks_per_second(), 33868800);
  memory_card_t* card = card_begin(); char* name = NULL;
  FILE* unique = fs_open_temporary_file("cupid-card-retry", 16, &name, NULL);
  EXPECT_NOT_NULL(unique); fclose(unique); EXPECT_TRUE(fs_delete_file(name, NULL));
  const size_t capacity = strlen(name) + 16;
  card->path = malloc(capacity); EXPECT_NOT_NULL(card->path);
  snprintf(card->path, capacity, "%s/card.mcd", name);
  u8 bytes[128]; payload(bytes); write_sector(card, 9, bytes);
  memory_card_reset(card);
  EXPECT_TRUE(card->changed);
  EXPECT_TRUE(timing_event_is_active(&card->save_event));
  /* Let the scheduled callback fail too, then recover before its next retry. */
  cpu_add_pending_ticks(timing_event_get_ticks_until_next_execution(&card->save_event));
  timing_events_run_events();
  EXPECT_TRUE(card->changed);
  EXPECT_TRUE(timing_event_is_active(&card->save_event));
  EXPECT_TRUE(fs_create_directory(name, false, NULL));
  cpu_add_pending_ticks(timing_event_get_ticks_until_next_execution(&card->save_event));
  timing_events_run_events();
  EXPECT_FALSE(card->changed);
  EXPECT_FALSE(timing_event_is_active(&card->save_event));
  u8* restored = malloc(MEMORY_CARD_IMAGE_DATA_SIZE); EXPECT_NOT_NULL(restored);
  EXPECT_TRUE(memory_card_image_load_from_file(restored, card->path, NULL));
  EXPECT_EQ(memcmp(restored, card->data, MEMORY_CARD_IMAGE_DATA_SIZE), 0);
  free(restored);
  EXPECT_TRUE(fs_delete_file(card->path, NULL));
  EXPECT_TRUE(fs_delete_directory(name, NULL)); free(name);
  card_end(card); settings_destroy(&g_settings);
}
