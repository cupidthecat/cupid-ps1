/* cupid-ps1 queued XA samples and pending-command continuation. */
#include "tests/core/cdrom/state/continuation_fixture.h"

static u32 xa_queue_size(void) {
  xa_saved_t*  saved  = xa_save();
  const size_t offset = xa_fixture.audio_offset;
  EXPECT_TRUE(offset + 8 <= saved->cd_size);
  const u32 count = xa_read_word(saved->cd + offset);
  EXPECT_TRUE(count <= 88200);
  EXPECT_EQ(offset + 4 + (size_t)count * 4 + 4 + (SAVE_STATE_VERSION >= 97 ? 1u : 0u), saved->cd_size);
  free(saved);
  return count;
}
static u32* xa_drain(u32* count) {
  *count = xa_queue_size();
  EXPECT_TRUE(*count > 0);
  u32* frames = malloc((size_t)*count * 4);
  EXPECT_NOT_NULL(frames);
  u32 nonzero = 0, different = 0;
  for (u32 i = 0; i < *count; i++) {
    s16 left, right;
    cdrom_get_audio_frame(&left, &right);
    frames[i] = (u16)left | ((u32)(u16)right << 16);
    nonzero += (left != 0 || right != 0);
    different += left != right;
    if (!xa_fixture.stereo) EXPECT_EQ(left, right);
  }
  EXPECT_TRUE(nonzero > 0);
  if (xa_fixture.stereo) EXPECT_TRUE(different > 0);
  EXPECT_EQ(xa_queue_size(), 0);
  return frames;
}
static void xa_case(bool eight, bool stereo, bool half) {
  xa_start(eight, stereo, half);
  xa_saved_t* empty       = xa_save();
  xa_fixture.audio_offset = empty->cd_size - 8 - (SAVE_STATE_VERSION >= 97 ? 1u : 0u);
  EXPECT_EQ(xa_read_word(empty->cd + xa_fixture.audio_offset), 0);
  /* Empty queues: predictor words precede 128 ring bytes, two phase bytes,
   * three queue counts, 16 PRNG bytes and 72 sector-metadata bytes. */
  const size_t predictor_offset = xa_fixture.audio_offset - (128 + 2 + 12 + 16 + 72 + 16);
  for (u32 i = 0; i < 4; i++)
    EXPECT_EQ(xa_read_word(empty->cd + predictor_offset + i * 4), 0);
  cdrom_write_register(0, 0);
  cdrom_write_register(1, 6);
  xa_saved_t* pending = xa_save();
  xa_ack();
  xa_until(1);
  u32  first_count;
  u32* first = xa_drain(&first_count);
  if (!half) EXPECT_EQ(first_count, (eight ? 2016u : 4032u) / (stereo ? 2u : 1u) * 7u / 6u);
  xa_saved_t* first_end = xa_save();
  xa_spu_equal(empty, first_end);
  xa_restore(pending);
  xa_ack();
  xa_until(1);
  u32  replay_count;
  u32* replay = xa_drain(&replay_count);
  EXPECT_EQ(first_count, replay_count);
  EXPECT_EQ(memcmp(first, replay, (size_t)first_count * 4), 0);
  xa_saved_t* replay_end = xa_save();
  xa_equal(first_end, replay_end);
  free(replay_end);
  free(replay);
  free(first);
  free(pending);
  /* Restore an active stream between sectors, then continue the complete next
   * packet. Interleaved sectors use a different channel and are skipped. */
  xa_until(xa_fixture.stride + 1);
  u32 remaining = xa_queue_size();
  EXPECT_TRUE(remaining > 37);
  u32 lead[37];
  for (u32 i = 0; i < 37; i++) {
    s16 left, right;
    cdrom_get_audio_frame(&left, &right);
    lead[i] = (u16)left | ((u32)(u16)right << 16);
  }
  xa_saved_t* midway = xa_save();
  u32         tail_count;
  u32*        tail  = xa_drain(&tail_count);
  xa_saved_t* final = xa_save();
  xa_spu_equal(empty, final);
  xa_restore(first_end);
  xa_until(xa_fixture.stride + 1);
  for (u32 i = 0; i < 37; i++) {
    s16 left, right;
    cdrom_get_audio_frame(&left, &right);
    EXPECT_EQ((u16)left | ((u32)(u16)right << 16), lead[i]);
  }
  replay_end = xa_save();
  xa_equal(midway, replay_end);
  free(replay_end);
  replay = xa_drain(&replay_count);
  EXPECT_EQ(tail_count, replay_count);
  EXPECT_EQ(memcmp(tail, replay, (size_t)tail_count * 4), 0);
  free(replay);
  replay_end = xa_save();
  xa_equal(final, replay_end);
  free(replay_end);
  xa_restore(midway);
  u8* invalid = malloc(midway->cd_size);
  EXPECT_NOT_NULL(invalid);
  const s32 invalid_samples[] = {-32769, 32768, INT32_MIN, INT32_MAX};
  for (u32 mutation = 0; mutation < 17; mutation++) {
    xa_restore(midway);
    memcpy(invalid, midway->cd, midway->cd_size);
    if (mutation == 0) {
      EXPECT_TRUE(xa_fixture.audio_offset >= 72);
      xa_word(invalid + xa_fixture.audio_offset - 72, 8);
    } else {
      const u32 slot = (mutation - 1) / 4, value = (mutation - 1) % 4;
      xa_word(invalid + predictor_offset + slot * 4, (u32)invalid_samples[value]);
    }
    state_wrapper_t rejected;
    state_wrapper_init_read(&rejected, invalid, midway->cd_size, SAVE_STATE_VERSION);
    EXPECT_FALSE(cdrom_do_state(&rejected));
    EXPECT_TRUE(rejected.error);
    replay_end = xa_save();
    xa_equal(midway, replay_end);
    free(replay_end);
    replay = xa_drain(&replay_count);
    EXPECT_EQ(tail_count, replay_count);
    EXPECT_EQ(memcmp(tail, replay, (size_t)tail_count * 4), 0);
    replay_end = xa_save();
    xa_equal(final, replay_end);
    free(replay_end);
    free(replay);
  }
  free(invalid);
  free(tail);
  free(final);
  free(midway);
  free(first_end);
  free(empty);
  printf("XA continuation eight=%u stereo=%u half=%u first=%u second=%u\n", eight, stereo, half, first_count,
         remaining);
  xa_end();
}
TEST(CdXaContinuation, four_bit_stereo) {
  xa_case(false, true, false);
}
TEST(CdXaContinuation, eight_bit_stereo) {
  xa_case(true, true, false);
}
TEST(CdXaContinuation, four_bit_mono_half_rate) {
  xa_case(false, false, true);
}
TEST(CdXaContinuation, eight_bit_stereo_half_rate) {
  xa_case(true, true, true);
}

static bool xa_predictor_sector(cd_image_t* image, void* buffer, const cd_image_index_t* index, cd_image_lba_t lba) {
  if (!xa_sector(image, buffer, index, lba)) return false;
  /* Start the next packet with nonzero filter weights. The ordinary first
   * group's zero-weight unit would otherwise erase a corrupt predictor. */
  memset((u8*)buffer + 24, 0x12, 16);
  return true;
}

TEST(CdXaContinuation, rejected_predictors_preserve_next_decoded_packet) {
  xa_start(false, true, false);
  static const cd_image_vtable_t vt      = {.read_sector_from_index = xa_predictor_sector};
  ((cd_image_t*)cdrom_get_media())->vtbl = &vt;
  xa_saved_t* empty                      = xa_save();
  xa_fixture.audio_offset                = empty->cd_size - 9;
  const size_t predictors                = xa_fixture.audio_offset - 246;
  EXPECT_EQ(xa_read_word(empty->cd + xa_fixture.audio_offset), 0);
  for (u32 slot = 0; slot < 4; slot++)
    EXPECT_EQ(xa_read_word(empty->cd + predictors + slot * 4), 0);
  xa_command(6, NULL, 0);
  xa_until(4);
  u32  count;
  u32* first = xa_drain(&count);
  free(first);
  xa_saved_t* pending = xa_save();
  xa_until(5);
  u32*        expected = xa_drain(&count);
  xa_saved_t* final    = xa_save();
  xa_spu_equal(empty, final);
  const s32 invalid[] = {-32769, 32768, INT32_MIN, INT32_MAX};
  u8*       bad       = malloc(pending->cd_size);
  EXPECT_NOT_NULL(bad);
  for (u32 slot = 0; slot < 4; slot++)
    for (u32 value = 0; value < 4; value++) {
      xa_restore(pending);
      memcpy(bad, pending->cd, pending->cd_size);
      xa_word(bad + predictors + slot * 4, (u32)invalid[value]);
      state_wrapper_t sw;
      state_wrapper_init_read(&sw, bad, pending->cd_size, SAVE_STATE_VERSION);
      EXPECT_FALSE(cdrom_do_state(&sw));
      EXPECT_TRUE(sw.error);
      xa_saved_t* unchanged = xa_save();
      xa_equal(pending, unchanged);
      free(unchanged);
      xa_until(5);
      u32  replay_count;
      u32* replay = xa_drain(&replay_count);
      EXPECT_EQ(replay_count, count);
      EXPECT_EQ(memcmp(replay, expected, (size_t)count * 4), 0);
      free(replay);
      xa_saved_t* result = xa_save();
      xa_equal(final, result);
      free(result);
    }
  free(bad);
  free(expected);
  free(final);
  free(pending);
  free(empty);
  xa_end();
}
