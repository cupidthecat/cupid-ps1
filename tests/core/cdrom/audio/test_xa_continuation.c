/* cupid-ps1 queued XA samples and pending-command continuation. */
#include "xa_sector_fixture.h"
enum {
  XA_CD_CAPACITY     = 1024 * 1024,
  XA_CPU_CAPACITY    = 16384,
  XA_TIMING_CAPACITY = 8192,
  XA_SPU_CAPACITY    = SPU_RAM_SIZE + 65536
};
typedef struct {
  u8     cd[XA_CD_CAPACITY], cpu[XA_CPU_CAPACITY], irq[128], timing[XA_TIMING_CAPACITY], spu[XA_SPU_CAPACITY];
  size_t cd_size, cpu_size, irq_size, timing_size, spu_size;
} xa_saved_t;
#define XA_SAVE(member, fn)                                                                  \
  do {                                                                                       \
    state_wrapper_init_write(&sw, saved->member, sizeof(saved->member), SAVE_STATE_VERSION); \
    EXPECT_TRUE(fn(&sw));                                                                    \
    saved->member##_size = state_wrapper_get_position(&sw);                                  \
  } while (0)
#define XA_LOAD(member, fn)                                                                \
  do {                                                                                     \
    state_wrapper_init_read(&sw, saved->member, saved->member##_size, SAVE_STATE_VERSION); \
    EXPECT_TRUE(fn(&sw));                                                                  \
    EXPECT_EQ(state_wrapper_get_position(&sw), saved->member##_size);                      \
  } while (0)
static xa_saved_t* xa_save(void) {
  xa_saved_t* saved = malloc(sizeof(*saved));
  EXPECT_NOT_NULL(saved);
  state_wrapper_t sw;
  XA_SAVE(cpu, cpu_do_state);
  XA_SAVE(irq, interrupt_controller_do_state);
  XA_SAVE(cd, cdrom_do_state);
  XA_SAVE(spu, spu_do_state);
  XA_SAVE(timing, timing_events_do_state);
  return saved;
}
static void xa_restore(const xa_saved_t* saved) {
  state_wrapper_t sw;
  XA_LOAD(cpu, cpu_do_state);
  XA_LOAD(irq, interrupt_controller_do_state);
  XA_LOAD(cd, cdrom_do_state);
  XA_LOAD(timing, timing_events_do_state);
  /* The paused consumer has done no work. Restoring SPU would itself reactivate
   * it; retain the invariant consumer state and compare all its bytes instead. */
}
#define XA_COMPARE(member)                                        \
  do {                                                            \
    EXPECT_EQ(a->member##_size, b->member##_size);                \
    EXPECT_EQ(memcmp(a->member, b->member, a->member##_size), 0); \
  } while (0)
static void xa_equal(const xa_saved_t* a, const xa_saved_t* b) {
  XA_COMPARE(cpu);
  XA_COMPARE(irq);
  XA_COMPARE(cd);
  XA_COMPARE(spu);
  XA_COMPARE(timing);
}
static void xa_spu_equal(const xa_saved_t* a, const xa_saved_t* b) {
  XA_COMPARE(spu);
}
static u32 xa_queue_size(void) {
  xa_saved_t*  saved  = xa_save();
  const size_t offset = xa_fixture.audio_offset;
  EXPECT_TRUE(offset + 8 <= saved->cd_size);
  const u32 count = xa_read_word(saved->cd + offset);
  EXPECT_TRUE(count <= 88200);
  EXPECT_EQ(offset + 4 + (size_t)count * 4 + 4, saved->cd_size);
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
  xa_fixture.audio_offset = empty->cd_size - 8;
  EXPECT_EQ(xa_read_word(empty->cd + xa_fixture.audio_offset), 0);
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
  memcpy(invalid, midway->cd, midway->cd_size);
  EXPECT_TRUE(xa_fixture.audio_offset >= 72);
  xa_word(invalid + xa_fixture.audio_offset - 72, 8);
  state_wrapper_t rejected;
  state_wrapper_init_read(&rejected, invalid, midway->cd_size, SAVE_STATE_VERSION);
  EXPECT_FALSE(cdrom_do_state(&rejected));
  EXPECT_TRUE(rejected.error);
  free(invalid);
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
