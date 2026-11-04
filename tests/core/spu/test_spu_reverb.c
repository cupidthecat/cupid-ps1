/* cupid-ps1 reverb input filtering and history continuation. */
#include "tests/core/spu/spu_state_fixture.h"

enum {
  SOUND_CAPACITY = SPU_RAM_SIZE + 65536,
  REVERB_BASE    = 0x8000,
  /* Version 90's SPU record: registers, stereo histories, then ring position. */
  DOWNSAMPLE_OFFSET = 174,
  UPSAMPLE_OFFSET   = DOWNSAMPLE_OFFSET + 2 * 128 * sizeof(s16),
  POSITION_OFFSET   = UPSAMPLE_OFFSET + 2 * 64 * sizeof(s16)
};

/* Full 39-tap input kernel, including zero taps and the centre coefficient. */
static const s16 input_kernel[39] = {-1,   0,    2,   0,     -10,  0,     35,    0,     -103, 0,     266, 0,    -616,
                                     0,    1332, 0,   -2960, 0,    10246, 16384, 10246, 0,    -2960, 0,   1332, 0,
                                     -616, 0,    266, 0,     -103, 0,     35,    0,     -10,  0,     2,   0,    -1};

static s32 floor_shift(s64 value, u32 shift) {
  const s64 unit = (s64)1 << shift;
  return (s32)(value >= 0 ? value / unit : -((-value + unit - 1) / unit));
}

static s16 saturate_sample(s32 value) {
  return (s16)(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
}

static void begin_reverb(void) {
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  interrupt_controller_reset();
  dma_initialize();
  spu_initialize();
  spu_write_register(0x1A2, REVERB_BASE / 8);
  spu_write_register(0x1C4, 0x4000); /* IIR alpha: half the filtered input. */
  spu_write_register(0x1FC, 0x4000); /* Left and right input gains: one half. */
  spu_write_register(0x1FE, 0x4000);
  spu_write_register(0x1D4, 0x100); /* Separate left/right reflection destinations. */
  spu_write_register(0x1D6, 0x200);
  spu_write_register(0x1AA, 0xC080);
}

static void end_reverb(void) {
  spu_shutdown();
  dma_shutdown();
  finish_cpu();
}

static size_t save_sound(u8* bytes) {
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, SOUND_CAPACITY, SAVE_STATE_VERSION);
  EXPECT_TRUE(spu_do_state(&sw));
  return state_wrapper_get_position(&sw);
}

static void load_sound(const u8* bytes, size_t length) {
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
  EXPECT_TRUE(spu_do_state(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), length);
  spu_post_load_state(SAVE_STATE_VERSION);
}

static void set_history(u8* bytes, const s16 history[2][64], u32 position) {
  for (u32 channel = 0; channel < 2; channel++) {
    for (u32 copy = 0; copy < 2; copy++)
      memcpy(bytes + DOWNSAMPLE_OFFSET + (channel * 128 + copy * 64) * sizeof(s16), history[channel], 64 * sizeof(s16));
  }
  memcpy(bytes + POSITION_OFFSET, &position, sizeof(position));
}

static void expect_reflections(const s16 history[2][64], u32 position, u32 reverb_step, s16* previous) {
  for (u32 channel = 0; channel < 2; channel++) {
    s64 sum = 0;
    for (u32 tap = 0; tap < 39; tap++)
      sum += (s64)input_kernel[tap] * history[channel][(position + 64 - 38 + tap) % 64];
    const s16 filtered = saturate_sample(floor_shift(sum, 15));
    const s16 expected = (s16)floor_shift(floor_shift(filtered, 1) + (previous ? previous[channel] : 0), 1);
    s16       actual;
    memcpy(&actual, spu_get_ram() + REVERB_BASE + (channel ? 0x200 : 0x100) * 8 + reverb_step * 2, sizeof(actual));
    EXPECT_EQ(actual, expected);
    if (previous) previous[channel] = expected;
  }
}

TEST(SpuReverb, input_impulses_all_history_positions) {
  begin_reverb();
  u8* initial = calloc(1, SOUND_CAPACITY);
  EXPECT_NOT_NULL(initial);
  const size_t length = save_sound(initial);
  /* Check the serialized layout before injecting histories. */
  u16 base;
  memcpy(&base, initial + DOWNSAMPLE_OFFSET - 66, sizeof(base));
  EXPECT_EQ(base, REVERB_BASE / 8);
  u8* injected = malloc(length);
  EXPECT_NOT_NULL(injected);
  for (u32 position = 1; position < 64; position += 2) {
    for (u32 tap = 0; tap < 38; tap++) {
      s16       history[2][64] = {{0}};
      const u32 index          = (position + 64 - 38 + tap) % 64;
      history[0][index]        = 30000;
      history[1][index]        = -24000;
      memcpy(injected, initial, length);
      set_history(injected, history, position);
      load_sound(injected, length);
      spu_execute(768);
      expect_reflections(history, position, 0, NULL);
    }
  }
  free(injected);
  free(initial);
  end_reverb();
}

TEST(SpuReverb, input_saturation_and_dc) {
  begin_reverb();
  u8* bytes = calloc(1, SOUND_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  const size_t length = save_sound(bytes);
  for (u32 pattern = 0; pattern < 3; pattern++) {
    s16 history[2][64] = {{0}};
    for (u32 tap = 0; tap < 38; tap++) {
      const u32 index   = (1 + 64 - 38 + tap) % 64;
      history[0][index] = pattern == 2 ? 12345 : (input_kernel[tap] < 0 ? -32768 : 32767);
      history[1][index] = pattern == 2 ? -12345 : (input_kernel[tap] < 0 ? 32767 : -32768);
      if (pattern == 1) {
        const s16 swap    = history[0][index];
        history[0][index] = history[1][index];
        history[1][index] = swap;
      }
    }
    set_history(bytes, history, 1);
    load_sound(bytes, length);
    spu_execute(768);
    expect_reflections(history, 1, 0, NULL);
  }
  free(bytes);
  end_reverb();
}

TEST(SpuReverb, streaming_history_wrap_and_restore) {
  begin_reverb();
  u8* bytes = calloc(1, SOUND_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  const size_t length = save_sound(bytes);
  s16          history[2][64];
  for (u32 i = 0; i < 64; i++) {
    history[0][i] = (s16)(i * 871 - 28000);
    history[1][i] = (s16)(27000 - i * 809);
  }
  const u32 start = 61;
  set_history(bytes, history, start);
  load_sound(bytes, length);
  spu_execution_snapshot_t midway      = {0};
  spu_execution_snapshot_t final       = {0};
  s16                      previous[2] = {0};
  for (u32 tick = 0; tick < 80; tick++) {
    const u32 position   = (start + tick) % 64;
    history[0][position] = history[1][position] = 0;
    spu_execute(768);
    if (position & 1) expect_reflections(history, position, tick / 2, previous);
    if (tick == 16) midway = save_spu_execution(SAVE_STATE_VERSION);
  }
  final = save_spu_execution(SAVE_STATE_VERSION);
  load_spu_execution(&midway);
  spu_execute(63 * 768);
  compare_spu_execution(&final);
  free_spu_execution(&final);
  free_spu_execution(&midway);
  free(bytes);
  end_reverb();
}

TEST(SpuReverb, decoded_voice_feeds_all_input_taps) {
  begin_reverb();
  for (u32 block = 0; block < 8; block++) {
    u8* data = spu_get_writable_ram() + 0x10000 + block * 16;
    data[0]  = 4; /* Unpredicted ADPCM with a varied signed nibble sequence. */
    data[1]  = 0;
    for (u32 byte = 2; byte < 16; byte++)
      data[byte] = (u8)(byte * 73 + block * 17);
  }
  spu_write_register(0x10, 0x2000); /* Voice 1: half gain left, quarter gain right. */
  spu_write_register(0x12, 0x1000);
  spu_write_register(0x14, 0x1000);
  spu_write_register(0x16, 0x10000 / 8);
  spu_write_register(0x18, 0x000F);
  spu_write_register(0x1A, 0x1F80);
  spu_write_register(0x198, 2);
  spu_write_register(0x188, 2);
  s16 history[2][64] = {{0}}, previous[2] = {0};
  u32 nonzero = 0;
  for (u32 tick = 0; tick < 160; tick++) {
    spu_execute(768);
    s16 captured;
    memcpy(&captured, spu_get_ram() + 0x800 + tick * 2, sizeof(captured));
    nonzero += captured != 0;
    const u32 position   = tick % 64;
    history[0][position] = (s16)floor_shift(captured, 1);
    history[1][position] = (s16)floor_shift(captured, 2);
    if (position & 1) expect_reflections(history, position, tick / 2, previous);
  }
  EXPECT_TRUE(nonzero > 100);
  end_reverb();
}
