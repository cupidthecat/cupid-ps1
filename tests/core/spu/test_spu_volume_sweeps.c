/* cupid-ps1 volume controls, stereo current registers and sweep continuation. */
#include "tests/core/spu/spu_state_fixture.h"

static void begin_sweeps(void)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  interrupt_controller_reset(); dma_initialize(); spu_initialize();
}

static void end_sweeps(void)
{
  spu_shutdown(); dma_shutdown(); finish_cpu();
}

static s16 main_level(u32 side) { return (s16)spu_read_register(0x1B8 + side * 2); }
static s16 voice_level(u32 voice, u32 side) { return (s16)spu_read_register(0x200 + voice * 4 + side * 2); }
static u16 fixed_control(s16 level) { return (u16)(level / 2) & 0x7FFF; }
static void set_main(u32 side, s16 level)
{
  spu_write_register(0x180 + side * 2, fixed_control(level));
  advance_spu_clock(768); EXPECT_EQ(main_level(side), level);
}

TEST(SpuSweep, direct_volume_waits_for_next_sample)
{
  begin_sweeps();
  for (u32 side = 0; side < 2; side++) {
    spu_write_register(0x180 + side * 2, 0x1000);
    EXPECT_EQ(main_level(side), 0);
    advance_spu_clock(767); EXPECT_EQ(main_level(side), 0);
    advance_spu_clock(1); EXPECT_EQ(main_level(side), 0x2000);
  }
  end_sweeps();
}

TEST(SpuSweep, all_voice_current_registers_keep_channel_order)
{
  begin_sweeps();
  for (u32 voice = 0; voice < 24; voice++) {
    spu_write_register(voice * 16, fixed_control((s16)(0x1000 + voice * 16)));
    spu_write_register(voice * 16 + 2, fixed_control((s16)(-0x2000 - (s32)voice * 16)));
  }
  advance_spu_clock(768);
  u32 failures = 0;
  for (u32 voice = 0; voice < 24; voice++) {
    failures += voice_level(voice, 0) != (s16)(0x1000 + voice * 16);
    failures += voice_level(voice, 1) != (s16)(-0x2000 - (s32)voice * 16);
  }
  end_sweeps(); EXPECT_EQ(failures, 0);
}

TEST(SpuSweep, decreasing_linear_remains_active_until_zero)
{
  begin_sweeps();
  for (u32 side = 0; side < 2; side++) {
    for (u32 negative = 0; negative < 2; negative++) {
      const s16 initial = negative ? -0x4000 : 0x4000;
      const s32 step = negative ? 7 : -8;
      set_main(side, initial);
      spu_write_register(0x180 + side * 2, (u16)(0xA02C | (negative ? 0x1000 : 0)));
      for (u32 sample = 1; sample <= 4; sample++) {
        advance_spu_clock(768); EXPECT_EQ(main_level(side), initial + (s32)sample * step);
      }
    }
  }
  end_sweeps();
}

TEST(SpuSweep, exponential_decrease_ignores_phase_bit_and_continues)
{
  begin_sweeps();
  for (u32 side = 0; side < 2; side++) {
    for (u32 phase = 0; phase < 2; phase++) {
      set_main(side, 0x6000);
      spu_write_register(0x180 + side * 2, (u16)(0xE02C | phase << 12));
      for (u32 sample = 1; sample <= 4; sample++) {
        advance_spu_clock(768); EXPECT_EQ(main_level(side), 0x6000 - (s32)sample * 6);
      }
    }
  }
  end_sweeps();
}

TEST(SpuSweep, decreasing_endpoints_stay_zero)
{
  begin_sweeps();
  set_main(0, 16); set_main(1, -14);
  spu_write_register(0x180, 0xA02C); spu_write_register(0x182, 0xB02C);
  advance_spu_clock(768); EXPECT_EQ(main_level(0), 8); EXPECT_EQ(main_level(1), -7);
  advance_spu_clock(768); EXPECT_EQ(main_level(0), 0); EXPECT_EQ(main_level(1), 0);
  advance_spu_clock(4 * 768); EXPECT_EQ(main_level(0), 0); EXPECT_EQ(main_level(1), 0);
  end_sweeps();
}

TEST(SpuSweep, keyed_and_idle_voice_sweeps_keep_ticking)
{
  for (u32 keyed = 0; keyed < 2; keyed++) {
    begin_sweeps();
    spu_write_register(0, 0x2000); spu_write_register(2, 0x1000);
    advance_spu_clock(768);
    if (keyed) {
      spu_write_register(4, 0x1000); spu_write_register(6, 0x200);
      spu_write_register(8, 0xF); spu_write_register(10, 0);
      spu_write_register(0x1AA, 0xC000); spu_write_register(0x188, 1);
      advance_spu_clock(32 * 768);
    }
    spu_write_register(0, 0xA02C); spu_write_register(2, 0xA02C);
    advance_spu_clock(3 * 768);
    EXPECT_EQ(voice_level(0, 0), 0x4000 - 24);
    EXPECT_EQ(voice_level(0, 1), 0x2000 - 24);
    end_sweeps();
  }
}

TEST(SpuSweep, all_ones_rate_does_not_step)
{
  begin_sweeps();
  for (u32 mode = 0; mode < 8; mode++) {
    const s16 initial = (mode & 1u) ? -0x4000 : 0x4000;
    set_main(0, initial); spu_write_register(0x180, (u16)(0x807F | mode << 12));
    advance_spu_clock(4096 * 768); EXPECT_EQ(main_level(0), initial);
  }
  end_sweeps();
}

TEST(SpuSweep, pending_direct_and_active_sweep_restore)
{
  begin_sweeps(); set_main(0, 0x4000);
  spu_write_register(0x180, 0x1000); EXPECT_EQ(main_level(0), 0x4000);
  spu_execution_snapshot_t pending = save_spu_execution(SAVE_STATE_VERSION);
  advance_spu_clock(768); EXPECT_EQ(main_level(0), 0x2000);
  load_spu_execution(&pending); EXPECT_EQ(main_level(0), 0x4000);
  advance_spu_clock(768); EXPECT_EQ(main_level(0), 0x2000);
  free_spu_execution(&pending);
  spu_write_register(0x180, 0xA03C); advance_spu_clock(5 * 768 + 200);
  pending = save_spu_execution(SAVE_STATE_VERSION);
  advance_spu_clock(32 * 768 + 568); EXPECT_EQ(main_level(0), 0x2000 - 16);
  spu_execution_snapshot_t final = save_spu_execution(SAVE_STATE_VERSION);
  load_spu_execution(&pending); compare_spu_execution(&pending);
  advance_spu_clock(32 * 768 + 568); compare_spu_execution(&final);
  free_spu_execution(&final); free_spu_execution(&pending); end_sweeps();
}

TEST(SpuSweep, legacy_stopped_decrease_resumes_from_saved_level)
{
  begin_sweeps(); set_main(0, 0x4000); spu_write_register(0x180, 0xA02C); advance_spu_clock(768);
  u8* bytes = malloc(SPU_RAM_SIZE + 65536); EXPECT_NOT_NULL(bytes);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, SPU_RAM_SIZE + 65536, SAVE_STATE_VERSION);
  EXPECT_TRUE(spu_do_state(&sw)); const size_t size = state_wrapper_get_position(&sw);
  /* The older halted-sweep bug saved a false active flag at a nonzero level. */
  bytes[38] = 0;
  state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION); EXPECT_TRUE(spu_do_state(&sw));
  advance_spu_clock(768); EXPECT_EQ(main_level(0), 0x4000 - 16);
  free(bytes); end_sweeps();
}

TEST(SpuSweep, current_main_write_restarts_finished_sweep)
{
  begin_sweeps(); set_main(0, 16); spu_write_register(0x180, 0xA02C);
  advance_spu_clock(2 * 768); EXPECT_EQ(main_level(0), 0);
  spu_write_register(0x1B8, 32); advance_spu_clock(768); EXPECT_EQ(main_level(0), 24);
  end_sweeps();
}

TEST(SpuSweep, slow_exponential_increase_keeps_minimum_counter_step)
{
  begin_sweeps();
  const u16 rates[] = {100, 103, 104, 108, 124, 126};
  for (u32 index = 0; index < countof(rates); index++) {
    set_main(0, 0x6000); spu_write_register(0x180, (u16)(0xC000 | rates[index]));
    const s32 step = 7 - (rates[index] & 3u);
    advance_spu_clock(32768 * 768); EXPECT_EQ(main_level(0), 0x6000 + step);
    advance_spu_clock(32768 * 768); EXPECT_EQ(main_level(0), 0x6000 + 2 * step);
  }
  end_sweeps();
}

TEST(SpuSweep, malformed_envelopes_do_not_resume_events)
{
  u8* bytes = malloc(SPU_RAM_SIZE + 65536); EXPECT_NOT_NULL(bytes);
  const size_t offsets[] = {33, 34, 35, 38, 32, 28, 24};
  for (u32 field = 0; field < countof(offsets); field++) {
    begin_sweeps(); state_wrapper_t sw;
    state_wrapper_init_write(&sw, bytes, SPU_RAM_SIZE + 65536, SAVE_STATE_VERSION);
    EXPECT_TRUE(spu_do_state(&sw)); const size_t size = state_wrapper_get_position(&sw);
    if (field < 4) bytes[offsets[field]] = 2;
    else if (field == 4) bytes[offsets[field]] = 128;
    else if (field == 5) { const u16 invalid = 0x8001; memcpy(bytes + offsets[field], &invalid, sizeof(invalid)); }
    else { const u32 invalid = 0x8000; memcpy(bytes + offsets[field], &invalid, sizeof(invalid)); }
    timing_event_t* event = *timing_events_get_head_event_ptr(); EXPECT_NOT_NULL(event);
    timing_event_deactivate(event);
    state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION);
    EXPECT_FALSE(spu_do_state(&sw)); EXPECT_TRUE(state_wrapper_has_error(&sw));
    EXPECT_TRUE(*timing_events_get_head_event_ptr() == NULL);
    end_sweeps();
  }
  free(bytes);
}
