/* cupid-ps1 shared noise clock, voice inputs and state continuation. */
#include "tests/core/spu/spu_state_fixture.h"

enum { NOISE_STATE_CAPACITY = SPU_RAM_SIZE + 65536, NOISE_LEVEL_OFFSET = 88 };

static void begin_noise(void)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  interrupt_controller_reset(); dma_initialize(); spu_initialize();
}

static void end_noise(void)
{
  spu_shutdown(); dma_shutdown(); finish_cpu();
}

static size_t save_noise(u8* bytes, u32 version)
{
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, NOISE_STATE_CAPACITY, version);
  EXPECT_TRUE(spu_do_state(&sw));
  return state_wrapper_get_position(&sw);
}

static u32 noise_level(void)
{
  u8* bytes = malloc(NOISE_STATE_CAPACITY); EXPECT_NOT_NULL(bytes);
  save_noise(bytes, SAVE_STATE_VERSION);
  u32 level; memcpy(&level, bytes + NOISE_LEVEL_OFFSET, sizeof(level));
  free(bytes); return level;
}

TEST(SpuNoise, all_clock_rates)
{
  begin_noise();
  u32 failures = 0;
  /* LFSR values after 8,10,12,14 shifts from the retained seed of 1. */
  const u32 expected_levels[] = {0x1FF, 0x7FF, 0x1FFD, 0x7FF4};
  for (u32 clock = 0; clock < 64; clock++) {
    spu_reset();
    spu_write_register(0x1AA, (u16)(0xC000 | clock << 8));
    /* Sixteen divider carries produce exactly 8,10,12,14 shifts. At the
     * fastest shift setting, the generator advances once per sample. */
    const u32 frames = clock >= 60 ? 16 : (16384u >> (clock >> 2)) * 16u;
    const u32 expected = clock >= 60 ? 0x1FFD1 : expected_levels[clock & 3u];
    advance_spu_clock((tick_count_t)(frames * 768u));
    const u32 actual = noise_level();
    printf("noise-rate clock=%u samples=%u actual=%08X expected=%08X\n", clock, frames, actual, expected);
    failures += actual != expected;
  }
  fflush(stdout); end_noise(); EXPECT_EQ(failures, 0);
}

TEST(SpuNoise, control_write_synchronizes_and_preserves_phase)
{
  begin_noise(); spu_write_register(0x1AA, 0xC000);
  advance_spu_clock(16383 * 768);
  EXPECT_EQ(noise_level(), 1);
  /* The sample at the write boundary belongs to the old clock. It leaves
   * half of the step counter accumulated for the new clock's next carry. */
  cpu_add_pending_ticks(768); spu_write_register(0x1AA, 0xC100);
  EXPECT_EQ(noise_level(), 1);
  advance_spu_clock(16383 * 768);
  EXPECT_EQ(noise_level(), 1);
  advance_spu_clock(768);
  EXPECT_EQ(noise_level(), 3);
  end_noise();
}

TEST(SpuNoise, first_edge_deadlines)
{
  begin_noise();
  for (u32 clock = 0; clock < 64; clock++) {
    spu_reset(); spu_write_register(0x1AA, (u16)(0xC000 | clock << 8));
    const u32 first = clock >= 60 ? 1 : (16384u >> (clock >> 2)) * 2u;
    advance_spu_clock((tick_count_t)((first - 1) * 768u));
    EXPECT_EQ(noise_level(), 1);
    advance_spu_clock(768); EXPECT_EQ(noise_level(), 3);
  }
  end_noise();
}

TEST(SpuNoise, lfsr_sequence_survives_enable_and_voice_mask_writes)
{
  const u32 expected[] = {
    0x00000003u, 0x00000007u, 0x0000000Fu, 0x0000001Fu, 0x0000003Fu, 0x0000007Fu, 0x000000FFu, 0x000001FFu,
    0x000003FFu, 0x000007FFu, 0x00000FFEu, 0x00001FFDu, 0x00003FFAu, 0x00007FF4u, 0x0000FFE8u, 0x0001FFD1u,
    0x0003FFA3u, 0x0007FF47u, 0x000FFE8Fu, 0x001FFD1Fu, 0x003FFA3Fu, 0x007FF47Eu, 0x00FFE8FCu, 0x01FFD1F9u,
    0x03FFA3F3u, 0x07FF47E6u, 0x0FFE8FCCu, 0x1FFD1F98u, 0x3FFA3F30u, 0x7FF47E60u, 0xFFE8FCC0u, 0xFFD1F981u
  };
  begin_noise();
  for (u32 sample = 0; sample < countof(expected); sample++) {
    spu_write_register(0x1AA, (u16)(0x3F00 | (sample & 3u) << 14));
    spu_write_register(0x194, (sample & 1u) ? 0xFFFF : 0);
    advance_spu_clock(768); EXPECT_EQ(noise_level(), expected[sample]);
  }
  end_noise();
}

TEST(SpuNoise, shared_by_voices_with_different_pitches)
{
  begin_noise();
  for (u32 voice = 1; voice <= 3; voice += 2) {
    const u32 offset = voice * 16;
    spu_write_register(offset, 0x3FFF); spu_write_register(offset + 2, 0x3FFF);
    spu_write_register(offset + 4, voice == 1 ? 0x400 : 0x3000);
    spu_write_register(offset + 6, 0x200);
    spu_write_register(offset + 8, 0xF); spu_write_register(offset + 10, 0);
  }
  spu_write_register(0x194, 0xA); spu_write_register(0x1AA, 0xF900);
  spu_write_register(0x188, 0xA); advance_spu_clock(96 * 768);
  u32 nonzero = 0;
  for (u32 sample = 32; sample < 96; sample++) {
    s16 left, right;
    memcpy(&left, spu_get_ram() + 0x800 + sample * 2, sizeof(left));
    memcpy(&right, spu_get_ram() + 0xC00 + sample * 2, sizeof(right));
    EXPECT_EQ(left, right); nonzero += left != 0;
  }
  EXPECT_TRUE(nonzero > 32); end_noise();
}

TEST(SpuNoise, fractional_sample_and_clock_phase_restore)
{
  begin_noise(); spu_write_register(0x1AA, 0xD100);
  advance_spu_clock(733 * 768 + 255);
  spu_execution_snapshot_t midway = save_spu_execution(SAVE_STATE_VERSION);
  advance_spu_clock(4500 * 768 + 513);
  spu_write_register(0x1AA, 0xF900);
  advance_spu_clock(333 * 768 + 17);
  spu_execution_snapshot_t final = save_spu_execution(SAVE_STATE_VERSION);
  load_spu_execution(&midway); compare_spu_execution(&midway);
  advance_spu_clock(4500 * 768 + 513);
  spu_write_register(0x1AA, 0xF900);
  advance_spu_clock(333 * 768 + 17);
  compare_spu_execution(&final);
  free_spu_execution(&final); free_spu_execution(&midway); end_noise();
}

TEST(SpuNoise, legacy_record_retains_level_and_restarts_missing_phase)
{
  begin_noise(); spu_write_register(0x1AA, 0xD100);
  advance_spu_clock(1757 * 768 + 255);
  u8* bytes = malloc(NOISE_STATE_CAPACITY); EXPECT_NOT_NULL(bytes);
  const size_t current_size = save_noise(bytes, SAVE_STATE_VERSION);
  const size_t phase = current_size - SPU_RAM_SIZE - 80 - 3;
  u16 divider; memcpy(&divider, bytes + phase, sizeof(divider));
  EXPECT_EQ(divider, 733 * 32); EXPECT_EQ(bytes[phase + 2], 5);
  const size_t legacy_size = save_noise(bytes, 91);
  EXPECT_EQ(current_size, legacy_size + 3);
  const u32 old_count = 0x12340084, old_level = 0x13579BDF;
  memcpy(bytes + 84, &old_count, sizeof(old_count));
  memcpy(bytes + NOISE_LEVEL_OFFSET, &old_level, sizeof(old_level));
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bytes, legacy_size, 91);
  EXPECT_TRUE(spu_do_state(&sw)); EXPECT_EQ(state_wrapper_get_position(&sw), legacy_size);
  EXPECT_EQ(save_noise(bytes, SAVE_STATE_VERSION), current_size);
  memcpy(&divider, bytes + phase, sizeof(divider));
  EXPECT_EQ(divider, 0); EXPECT_EQ(bytes[phase + 2], 0);
  u32 retained; memcpy(&retained, bytes + 84, sizeof(retained)); EXPECT_EQ(retained, old_count);
  EXPECT_EQ(noise_level(), old_level);
  advance_spu_clock(2047 * 768); EXPECT_EQ(noise_level(), old_level);
  advance_spu_clock(768);
  EXPECT_EQ(noise_level(), (old_level << 1) | (((old_level >> 15) ^ (old_level >> 12) ^
            (old_level >> 11) ^ (old_level >> 10) ^ 1u) & 1u));
  free(bytes); end_noise();
}

TEST(SpuNoise, malformed_or_truncated_phase_keeps_events_inactive)
{
  u8* bytes = malloc(NOISE_STATE_CAPACITY); EXPECT_NOT_NULL(bytes);
  for (u32 invalid = 0; invalid < 3; invalid++) {
    begin_noise();
    const size_t size = save_noise(bytes, SAVE_STATE_VERSION);
    const size_t phase = size - SPU_RAM_SIZE - 80 - 3;
    if (invalid == 0) bytes[phase + 1] = 0x80;
    if (invalid == 1) bytes[phase + 2] = 8;
    timing_event_t* event = *timing_events_get_head_event_ptr(); EXPECT_NOT_NULL(event);
    timing_event_deactivate(event);
    EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, bytes, invalid == 2 ? phase + 1 : size, SAVE_STATE_VERSION);
    EXPECT_FALSE(spu_do_state(&sw)); EXPECT_TRUE(state_wrapper_has_error(&sw));
    EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
    end_noise();
  }
  free(bytes);
}
