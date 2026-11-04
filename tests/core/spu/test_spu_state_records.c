/* cupid-ps1 saved SPU records preserve live execution when rejected. */
#include "tests/core/spu/spu_state_fixture.h"

enum {
  CAPACITY      = SPU_RAM_SIZE + 65536,
  POSITION      = 174 + 2 * 128 * sizeof(s16) + 2 * 64 * sizeof(s16),
  VOICES        = POSITION + sizeof(s32),
  VOICE_BYTES   = 143,
  TRANSFER_BASE = 0x10000
};

static void begin_probe(void) {
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
  interrupt_controller_reset();
  dma_initialize();
  spu_initialize();
  u8* ram = spu_get_writable_ram();
  for (u32 i = 0; i < SPU_RAM_SIZE; i++)
    ram[i] = (u8)(i * 37u + 9u);
  /* Looping ADPCM, noise, sweeps and reverb have distinct live histories. */
  ram[0x1000] = 0x24;
  ram[0x1001] = 7;
  for (u32 voice = 1; voice <= 3; voice += 2) {
    const u32 offset = voice * 16;
    spu_write_register(offset, 0x3FFF);
    spu_write_register(offset + 2, 0x3FFF);
    spu_write_register(offset + 4, voice == 1 ? 0x900 : 0x1700);
    spu_write_register(offset + 6, 0x200);
    spu_write_register(offset + 8, 0xF);
  }
  spu_write_register(0x180, 0xC050);
  spu_write_register(0x182, 0x3FFF);
  spu_write_register(0x194, 8);
  spu_write_register(0x198, 0xA);
  spu_write_register(0x1A2, 0x8000 / 8);
  spu_write_register(0x184, 0x2000);
  spu_write_register(0x186, 0x2000);
  spu_write_register(0x1C4, 0x4000);
  spu_write_register(0x1D4, 0x100);
  spu_write_register(0x1D6, 0x200);
  spu_write_register(0x1FC, 0x4000);
  spu_write_register(0x1FE, 0x4000);
  spu_write_register(0x1AA, 0xFF80);
  spu_write_register(0x188, 0xA);
  advance_spu_clock(113 * 768 + 255);
  u32 capture_nonzero[2] = {0};
  for (u32 sample = 32; sample < 113; sample++) {
    for (u32 channel = 0; channel < 2; channel++) {
      s16 value;
      memcpy(&value, ram + 0x800 + channel * 0x400 + sample * sizeof(value), sizeof(value));
      capture_nonzero[channel] += value != 0;
    }
  }
  EXPECT_TRUE(capture_nonzero[0] > 32 && capture_nonzero[1] > 32);
  spu_write_register(0x1A6, TRANSFER_BASE / 8);
  spu_write_register(0x1AA, 0xFFA0);
  u32 transfer_words[16];
  for (u32 i = 0; i < countof(transfer_words); i++)
    transfer_words[i] = 0x12345678u ^ (i * 0x1020304u);
  spu_dma_write(transfer_words, countof(transfer_words));
  advance_spu_clock(7);
}

static void end_probe(void) {
  spu_shutdown();
  dma_shutdown();
  finish_cpu();
}

static size_t save_record(u8* data, u32 version) {
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, data, CAPACITY, version);
  EXPECT_TRUE(spu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw),
            VOICES + 24 * VOICE_BYTES + (version >= 92 ? 3 : 0) + (version >= 89 ? 4 : 0) + 76 + SPU_RAM_SIZE);
  return state_wrapper_get_position(&sw);
}

static void expect_rejected(u8* bytes, size_t length, size_t available, u32 version,
                            const spu_execution_snapshot_t* before) {
  /* Rejected records also carry different valid fields and complete RAM. */
  const u32 changed_level = 0xABCD1234;
  memcpy(bytes + 88, &changed_level, sizeof(changed_level));
  memset(bytes + length - SPU_RAM_SIZE, 0xA7, SPU_RAM_SIZE);
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bytes, available, version);
  EXPECT_FALSE(spu_do_state(&sw));
  EXPECT_TRUE(state_wrapper_has_error(&sw));
  compare_spu_execution(before);
}

TEST(SpuStateRecords, malformed_reverb_preserves_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  const s32                invalid[] = {-1, 64, 128, INT32_MIN, INT32_MAX};
  spu_execution_snapshot_t before    = save_spu_execution(SAVE_STATE_VERSION);
  for (u32 version = SAVE_STATE_MINIMUM_VERSION; version <= SAVE_STATE_VERSION; version++) {
    for (u32 value = 0; value < countof(invalid); value++) {
      const size_t length = save_record(bytes, version);
      memcpy(bytes + POSITION, &invalid[value], sizeof(s32));
      expect_rejected(bytes, length, length, version, &before);
    }
  }
  free_spu_execution(&before);
  advance_spu_clock(768);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, truncated_ram_preserves_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before = save_spu_execution(SAVE_STATE_VERSION);
  for (u32 version = SAVE_STATE_MINIMUM_VERSION; version <= SAVE_STATE_VERSION; version++) {
    const size_t length = save_record(bytes, version);
    expect_rejected(bytes, length, length - 1, version, &before);
  }
  free_spu_execution(&before);
  advance_spu_clock(768);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, malformed_transfer_address_preserves_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before    = save_spu_execution(SAVE_STATE_VERSION);
  const u32                invalid[] = {1, SPU_RAM_SIZE - 1, SPU_RAM_SIZE, SPU_RAM_SIZE + 2, UINT32_MAX};
  for (u32 version = SAVE_STATE_MINIMUM_VERSION; version <= SAVE_STATE_VERSION; version++) {
    for (u32 value = 0; value < countof(invalid); value++) {
      const size_t length = save_record(bytes, version);
      memcpy(bytes + 10, &invalid[value], sizeof(u32));
      expect_rejected(bytes, length, length, version, &before);
    }
  }
  free_spu_execution(&before);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, malformed_voice_indices_and_phases_preserve_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before     = save_spu_execution(SAVE_STATE_VERSION);
  const u32                versions[] = {42, 88, 89, 90, 91, 92, SAVE_STATE_VERSION};
  for (u32 v = 0; v < countof(versions); v++) {
    for (u32 voice = 0; voice < 24; voice++) {
      if (versions[v] != SAVE_STATE_VERSION && voice != 0 && voice != 23) continue;
      for (u32 bad = 0; bad < 6; bad++) {
        const size_t length = save_record(bytes, versions[v]);
        if (bad < 4) {
          const u32 counter = (28 + bad) << 12;
          memcpy(bytes + VOICES + voice * VOICE_BYTES + 18, &counter, sizeof(counter));
        } else {
          bytes[VOICES + voice * VOICE_BYTES + 138] = bad == 4 ? 5 : 255;
        }
        expect_rejected(bytes, length, length, versions[v], &before);
      }
    }
  }
  free_spu_execution(&before);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, malformed_sample_and_capture_phases_preserve_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before            = save_spu_execution(SAVE_STATE_VERSION);
  const s32                invalid_ticks[]   = {-1, 768, INT32_MAX};
  const u16                invalid_capture[] = {1, 1023, 1024, UINT16_MAX};
  for (u32 version = SAVE_STATE_MINIMUM_VERSION; version <= SAVE_STATE_VERSION; version++) {
    for (u32 value = 0; value < countof(invalid_ticks) + countof(invalid_capture); value++) {
      const size_t length = save_record(bytes, version);
      if (value < countof(invalid_ticks))
        memcpy(bytes, &invalid_ticks[value], sizeof(s32));
      else
        memcpy(bytes + 18, &invalid_capture[value - countof(invalid_ticks)], sizeof(u16));
      expect_rejected(bytes, length, length, version, &before);
    }
  }
  free_spu_execution(&before);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, existing_rejections_preserve_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before     = save_spu_execution(SAVE_STATE_VERSION);
  const u32                versions[] = {42, 88, 89, 90, 91, 92, SAVE_STATE_VERSION};
  for (u32 v = 0; v < countof(versions); v++) {
    for (u32 bad = 0; bad < 15; bad++) {
      if (bad >= 11 && bad <= 12 && versions[v] < 92) continue;
      if (bad >= 13 && versions[v] < 89) continue;
      const size_t length = save_record(bytes, versions[v]);
      const size_t fifo   = length - SPU_RAM_SIZE - 12;
      if (bad < 4) {
        const size_t offsets[] = {33, 34, 35, 38};
        bytes[offsets[bad]]    = 2;
      } else if (bad == 4)
        bytes[32] = 128;
      else if (bad == 5) {
        const u16 increment = 0x8001;
        memcpy(bytes + 28, &increment, sizeof(increment));
      } else if (bad == 6) {
        const u32 counter = 0x8000;
        memcpy(bytes + 24, &counter, sizeof(counter));
      } else if (bad < 11) {
        const u32 invalid = bad == 9 ? 33 : bad == 10 ? 1 : 32;
        const u32 field   = bad == 10 ? 1 : bad - 7;
        memcpy(bytes + fifo + field * sizeof(u32), &invalid, sizeof(invalid));
      } else if (bad < 13) {
        const size_t phase                 = length - SPU_RAM_SIZE - 83;
        bytes[phase + (bad == 11 ? 1 : 2)] = bad == 11 ? 0x80 : 8;
      } else {
        const s32 carry = bad == 13 ? -1 : 16;
        memcpy(bytes + fifo - 64 - sizeof(carry), &carry, sizeof(carry));
      }
      expect_rejected(bytes, length, length, versions[v], &before);
    }
  }
  free_spu_execution(&before);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, truncated_fields_preserve_live_machine) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t before     = save_spu_execution(SAVE_STATE_VERSION);
  const u32                versions[] = {42, 88, 89, 90, 91, 92, SAVE_STATE_VERSION};
  for (u32 v = 0; v < countof(versions); v++) {
    const size_t length = save_record(bytes, versions[v]);
    const size_t ram    = length - SPU_RAM_SIZE;
    const size_t cuts[] = {0,
                           1,
                           10,
                           24,
                           39,
                           55,
                           174,
                           POSITION - 1,
                           POSITION,
                           VOICES - 1,
                           VOICES,
                           VOICES + 23,
                           VOICES + VOICE_BYTES - 1,
                           VOICES + 24 * VOICE_BYTES - 1,
                           ram - 77,
                           ram - 76,
                           ram - 13,
                           ram - 1,
                           ram,
                           ram + 1,
                           ram + SPU_RAM_SIZE / 2};
    for (u32 cut = 0; cut < countof(cuts); cut++) {
      save_record(bytes, versions[v]);
      expect_rejected(bytes, length, cuts[cut], versions[v], &before);
    }
  }
  free_spu_execution(&before);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, clock_divider_boundaries_and_legacy_writes) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  u8* after = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  EXPECT_NOT_NULL(after);
  for (u32 overclock = 0; overclock < 2; overclock++) {
    if (overclock) {
      g_settings.cpu_overclock_enable = true;
      settings_set_cpu_overclock_percent(&g_settings, 150);
      settings_update_overclock_active(&g_settings);
      spu_cpu_clock_changed();
    }
    const s32 divider = overclock ? 3 * 768 : 768;
    for (s32 carry = 0; carry < divider; carry += divider - 1) {
      const size_t length = save_record(bytes, SAVE_STATE_VERSION);
      memcpy(bytes, &carry, sizeof(carry));
      state_wrapper_t sw;
      state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
      EXPECT_TRUE(spu_do_state(&sw));
      EXPECT_EQ(save_record(after, SAVE_STATE_VERSION), length);
      EXPECT_EQ(memcmp(bytes, after, length), 0);
      spu_execution_snapshot_t before = save_spu_execution(SAVE_STATE_VERSION);
      for (u32 version = SAVE_STATE_MINIMUM_VERSION; version < SAVE_STATE_VERSION; version++) {
        save_record(bytes, version);
        compare_spu_execution(&before);
      }
      free_spu_execution(&before);
    }
    spu_execution_snapshot_t before = save_spu_execution(SAVE_STATE_VERSION);
    const size_t             length = save_record(bytes, SAVE_STATE_VERSION);
    memcpy(bytes, &divider, sizeof(divider));
    expect_rejected(bytes, length, length, SAVE_STATE_VERSION, &before);
    free_spu_execution(&before);
  }
  free(after);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, rejected_load_preserves_audio_and_transfer_continuation) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  spu_execution_snapshot_t initial = save_spu_execution(SAVE_STATE_VERSION);
  const tick_count_t       elapsed = 4096 + 37;
  advance_spu_clock(elapsed);
  spu_execution_snapshot_t expected = save_spu_execution(SAVE_STATE_VERSION);
  u16                      transferred;
  memcpy(&transferred, spu_get_ram() + TRANSFER_BASE, sizeof(transferred));
  EXPECT_EQ(transferred, 0x5678);
  for (u32 bad = 0; bad < 7; bad++) {
    load_spu_execution(&initial);
    compare_spu_execution(&initial);
    const size_t length    = save_record(bytes, SAVE_STATE_VERSION);
    const u32    values[]  = {UINT32_MAX, SPU_RAM_SIZE, 31u << 12, INT32_MAX, 32};
    const size_t offsets[] = {POSITION, 10, VOICES + VOICE_BYTES + 18, 0, length - SPU_RAM_SIZE - 12};
    if (bad < 5)
      memcpy(bytes + offsets[bad], &values[bad], sizeof(u32));
    else if (bad == 5)
      bytes[33] = 2;
    expect_rejected(bytes, length, length - (bad == 6), SAVE_STATE_VERSION, &initial);
    advance_spu_clock(elapsed);
    compare_spu_execution(&expected);
  }
  free_spu_execution(&expected);
  free_spu_execution(&initial);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, valid_voice_indices_phases_and_ram_boundary) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  u8* after = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  EXPECT_NOT_NULL(after);
  for (u32 voice = 0; voice < 24; voice++) {
    for (u32 edge = 0; edge < 2; edge++) {
      const size_t length  = save_record(bytes, SAVE_STATE_VERSION);
      const u32    counter = edge ? (27u << 12) | 0xFF0u : 0;
      const u32    address = edge ? SPU_RAM_SIZE - 2 : 0;
      const u16    capture = edge ? 1022 : 0;
      memcpy(bytes + VOICES + voice * VOICE_BYTES + 18, &counter, sizeof(counter));
      bytes[VOICES + voice * VOICE_BYTES + 138] = edge ? 4 : 0;
      memcpy(bytes + 10, &address, sizeof(address));
      memcpy(bytes + 18, &capture, sizeof(capture));
      state_wrapper_t sw;
      state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
      EXPECT_TRUE(spu_do_state(&sw));
      EXPECT_EQ(save_record(after, SAVE_STATE_VERSION), length);
      EXPECT_EQ(memcmp(bytes, after, length), 0);
    }
  }
  /* Voice 1 uses ADPCM interpolation, including the last three history taps. */
  spu_write_register(0x18, 0xF);
  spu_write_register(0x188, 2);
  advance_spu_clock(768);
  for (u32 index = 0; index < 28; index++) {
    const size_t length  = save_record(bytes, SAVE_STATE_VERSION);
    const u32    counter = (index << 12) | ((index & 1u) ? 0xFF0u : 0);
    const u16    level   = 0x4000;
    memcpy(bytes + VOICES + VOICE_BYTES + 18, &counter, sizeof(counter));
    memcpy(bytes + VOICES + VOICE_BYTES + 14, &level, sizeof(level));
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
    EXPECT_TRUE(spu_do_state(&sw));
    advance_spu_clock(768);
  }
  free(after);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, valid_legacy_records_keep_their_layout_and_conversion) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  u8* after = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  EXPECT_NOT_NULL(after);
  spu_execution_snapshot_t initial = save_spu_execution(SAVE_STATE_VERSION);
  for (u32 version = SAVE_STATE_MINIMUM_VERSION; version <= SAVE_STATE_VERSION; version++) {
    load_spu_execution(&initial);
    const size_t    length = save_record(bytes, version);
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, bytes, length, version);
    EXPECT_TRUE(spu_do_state(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), length);
    EXPECT_EQ(save_record(after, version), length);
    EXPECT_EQ(memcmp(bytes, after, length), 0);
    const size_t current = save_record(after, SAVE_STATE_VERSION);
    const size_t phase   = current - SPU_RAM_SIZE - 83;
    if (version < 92) {
      EXPECT_EQ(after[phase], 0);
      EXPECT_EQ(after[phase + 1], 0);
      EXPECT_EQ(after[phase + 2], 0);
    }
    if (version < 89) {
      s32 carry;
      memcpy(&carry, after + phase + 3, sizeof(carry));
      EXPECT_EQ(carry, 0);
    }
    spu_post_load_state(version);
    advance_spu_clock(2 * 768 + 17);
  }
  free_spu_execution(&initial);
  free(after);
  free(bytes);
  end_probe();
}

TEST(SpuStateRecords, every_valid_reverb_phase_survives) {
  begin_probe();
  u8* bytes = malloc(CAPACITY);
  u8* after = malloc(CAPACITY);
  EXPECT_NOT_NULL(bytes);
  EXPECT_NOT_NULL(after);
  for (s32 position = 0; position < 64; position++) {
    const size_t length = save_record(bytes, SAVE_STATE_VERSION);
    memcpy(bytes + POSITION, &position, sizeof(position));
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, bytes, length, SAVE_STATE_VERSION);
    EXPECT_TRUE(spu_do_state(&sw));
    EXPECT_EQ(save_record(after, SAVE_STATE_VERSION), length);
    EXPECT_EQ(memcmp(bytes, after, length), 0);
    advance_spu_clock(768);
  }
  free(after);
  free(bytes);
  end_probe();
}
