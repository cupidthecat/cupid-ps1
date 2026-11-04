/* cupid-ps1 tiled-output DMA progress and legacy queue restoration. */
#include "tests/core/mdec/mdec_spatial_fixture.h"

static void check_split_dma(u32 depth, bool wrapped, bool no_function) {
  decode_spatial_quad(false, depth, false, false);
  const u32 address = wrapped ? bus_get_ram_mask() - 15 : 0x50000;
  const u32 count   = spatial_word_count(depth);
  spatial_dma(address, 4);
  coefficient_snapshot_t before = read_coefficients();
  EXPECT_EQ(before.output_row_words, depth == 3 ? 4 : 6);
  EXPECT_EQ(before.output_word, 4);
  if (no_function) mdec_write_register(0, depth == 3 ? 0x10000000 : 0x18000000);
  restore_coefficients();
  const coefficient_snapshot_t after = read_coefficients();
  EXPECT_EQ(after.output_row_words, before.output_row_words);
  EXPECT_EQ(after.output_word, 4);
  spatial_dma((address + 16) & bus_get_ram_mask(), count - 4);
  u8 expected[768];
  spatial_expected(expected, depth, false, false, false);
  for (u32 i = 0; i < count * 4; i++)
    EXPECT_EQ(bus_get_unprotected_ram_pointer()[(address + i) & bus_get_ram_mask()], expected[i]);
  EXPECT_EQ(read_coefficients().output_word, 0);
  end_decode();
}

TEST(MdecSpatialState, split_dma_restores_both_row_widths) {
  check_split_dma(3, false, false);
  check_split_dma(2, false, false);
}

TEST(MdecSpatialState, wrapped_dma_restores_both_row_widths) {
  check_split_dma(3, true, false);
  check_split_dma(2, true, false);
}

TEST(MdecSpatialState, no_function_depth_does_not_change_queued_dma_layout) {
  check_split_dma(3, false, true);
  check_split_dma(2, false, true);
}

TEST(MdecSpatialState, cpu_block_status_tracks_queued_tiles) {
  for (u32 depth = 2; depth <= 3; depth++) {
    decode_spatial_quad(false, depth, false, false);
    const u32 count = spatial_word_count(depth);
    for (u32 word = 0; word < count; word++) {
      EXPECT_EQ((mdec_read_register(4) >> 16) & 7, word / (count / 4));
      mdec_read_register(0);
    }
    end_decode();
  }
}

TEST(MdecSpatialState, new_decode_discards_old_queue_and_resets_width) {
  decode_spatial_quad(false, 3, false, false);
  spatial_dma(0x50000, 4);
  mdec_write_register(0, 0x30000006);
  EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, 0);
  EXPECT_EQ(read_coefficients().output_row_words, 6);
  EXPECT_EQ(read_coefficients().output_word, 0);
  const s16 dc[] = {0, 0, -128, -64, 64, 128};
  for (u32 i = 0; i < 6; i++)
    mdec_write_register(0, 0xFE000000 | ((u32)dc[i] & 0x3FF));
  restore_coefficients();
  run_slice(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()) + 1);
  spatial_dma(0x60000, 192);
  u8 expected[768];
  spatial_expected(expected, 2, false, false, false);
  EXPECT_EQ(memcmp(bus_get_unprotected_ram_pointer() + 0x60000, expected, 768), 0);
  end_decode();
}

TEST(MdecSpatialState, translated_dma_writes_invalidate_compiled_pages) {
  decode_spatial_quad(false, 3, false, false);
  const u32 target = 0x50000;
  const u32 loop[] = {0x1000FFFF, 0};
  memcpy(bus_get_unprotected_ram_pointer() + target, loop, sizeof(loop));
  cpu_code_cache_compile_or_revalidate_block(0x80000000 | target);
  EXPECT_TRUE(bus_is_ram_code_page(bus_get_ram_code_page_index(target)));
  spatial_dma(target, 128);
  EXPECT_FALSE(bus_is_ram_code_page(bus_get_ram_code_page_index(target)));
  u8 expected[768];
  spatial_expected(expected, 3, false, false, false);
  EXPECT_EQ(memcmp(bus_get_unprotected_ram_pointer() + target, expected, 512), 0);
  end_decode();
}

TEST(MdecSpatialState, rejects_invalid_saved_dma_metadata) {
  const u8 cases[][2] = {{1, 0}, {5, 0}, {4, 128}, {0, 1}, {6, 192}};
  for (u32 i = 0; i < countof(cases); i++) {
    begin_decode(false);
    mdec_write_register(0, 0x28000001);
    mdec_write_register(0, 0xFE000000);
    timing_event_t* event = find_mdec_copy_event();
    u8              bytes[8192];
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, bytes, sizeof(bytes), SAVE_STATE_VERSION);
    EXPECT_TRUE(mdec_do_state(&sw));
    const size_t size = state_wrapper_get_position(&sw);
    memcpy(bytes + size - 2, cases[i], 2);
    state_wrapper_init_read(&sw, bytes, size, SAVE_STATE_VERSION);
    EXPECT_FALSE(mdec_do_state(&sw));
    EXPECT_TRUE(state_wrapper_has_error(&sw));
    EXPECT_FALSE(timing_event_is_active(event));
    mdec_reset();
    end_decode();
  }
}

TEST(MdecSpatialState, rejects_old_writer_without_mutating_live_tiled_output) {
  decode_spatial_quad(false, 3, false, false);
  u8              bytes[8192];
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, sizeof(bytes), 92);
  EXPECT_FALSE(mdec_do_state(&sw));
  EXPECT_TRUE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), 0);
  EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, 128);
  EXPECT_EQ(mdec_read_register(0), 0x39CE39CE);
  end_decode();
}

TEST(MdecSpatialState, imports_version_92_queued_records) {
  for (u32 depth = 2; depth <= 3; depth++) {
    for (u32 partial = 0; partial < 2; partial++) {
      for (u32 dma = 0; dma < 2; dma++) {
        begin_decode(false);
        char        path[4096];
        const char* root = getenv("CUPID_TEST_SOURCE_DIR");
        EXPECT_NOT_NULL(root);
        snprintf(path, sizeof(path), "%s/tests/core/mdec/fixtures/color%u-%s-v92.hex", root, depth == 3 ? 15 : 24,
                 partial ? "partial" : "full");
        FILE* file = fopen(path, "r");
        EXPECT_NOT_NULL(file);
        u8           bytes[8192];
        size_t       size = 0;
        unsigned int value;
        while (fscanf(file, "%2x", &value) == 1) {
          EXPECT_TRUE(size < sizeof(bytes));
          bytes[size++] = (u8)value;
        }
        fclose(file);
        EXPECT_EQ(size, 3886);
        state_wrapper_t sw;
        state_wrapper_init_read(&sw, bytes, size, 92);
        EXPECT_TRUE(mdec_do_state(&sw));
        EXPECT_EQ(state_wrapper_get_position(&sw), size);
        EXPECT_EQ(read_coefficients().output_row_words, 0);
        u8 expected[768];
        spatial_expected(expected, depth, false, false, false);
        const u32 consumed = partial ? 5 : 0;
        const u32 count    = spatial_word_count(depth) - consumed;
        u32       actual[192];
        if (dma)
          mdec_dma_read(actual, count);
        else
          for (u32 i = 0; i < count; i++)
            actual[i] = mdec_read_register(0);
        EXPECT_EQ(memcmp(actual, expected + consumed * 4, count * 4), 0);
        EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, 0);
        mdec_reset();
        queue_spatial_quad(depth, false, false);
        spatial_expected(expected, depth, false, false, true);
        for (u32 i = 0; i < spatial_word_count(depth); i++)
          actual[i] = mdec_read_register(0);
        EXPECT_EQ(memcmp(actual, expected, spatial_word_count(depth) * 4), 0);
        end_decode();
      }
    }
  }
}

TEST(MdecSpatialState, dma_cursor_survives_next_macroblock_in_same_command) {
  for (u32 depth = 2; depth <= 3; depth++) {
    decode_spatial_quad(false, depth, false, false);
    mdec_reset();
    mdec_write_register(0, 0x2000000C | (depth << 27));
    const s16 dc[] = {0, 0, -128, -64, 64, 128};
    for (u32 block = 0; block < 2; block++)
      for (u32 i = 0; i < 6; i++)
        mdec_write_register(0, 0xFE000000 | ((u32)dc[i] & 0x3FF));
    run_slice(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()) + 1);
    const u32 count = spatial_word_count(depth), width = depth == 3 ? 4 : 6;
    for (u32 i = 0; i < 8; i++)
      mdec_read_register(0);
    u32 word;
    s32 offset;
    for (u32 i = 8; i < count; i++)
      EXPECT_TRUE(mdec_dma_read_word(&word, &offset));
    EXPECT_EQ(read_coefficients().output_word, count - 8);
    EXPECT_TRUE(read_coefficients().pending);
    restore_coefficients();
    run_slice(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()) + 1);
    EXPECT_EQ(read_coefficients().output_word, count - 8);
    for (u32 i = 0; i < count; i++) {
      const u32 row      = ((count - 8 + i) % count) / width;
      const s32 expected = (s32)((row & 7) * width) - ((row & 8) ? (s32)(7 * width) : 0);
      EXPECT_TRUE(mdec_dma_read_word(&word, &offset));
      EXPECT_EQ(offset, expected);
    }
    EXPECT_EQ(read_coefficients().output_word, count - 8);
    end_decode();
  }
}

TEST(MdecSpatialState, legacy_pending_copy_starts_new_tiled_output) {
  for (u32 depth = 2; depth <= 3; depth++) {
    decode_spatial_quad(false, depth, false, false);
    mdec_reset();
    mdec_write_register(0, 0x20000006 | (depth << 27));
    const s16 dc[] = {0, 0, -128, -64, 64, 128};
    for (u32 i = 0; i < 6; i++)
      mdec_write_register(0, 0xFE000000 | ((u32)dc[i] & 0x3FF));
    EXPECT_TRUE(read_coefficients().pending);
    u8              bytes[8192];
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, bytes, sizeof(bytes), 92);
    EXPECT_TRUE(mdec_do_state(&sw));
    const size_t size = state_wrapper_get_position(&sw);
    state_wrapper_init_read(&sw, bytes, size, 92);
    EXPECT_TRUE(mdec_do_state(&sw));
    EXPECT_EQ(read_coefficients().output_row_words, 0);
    run_slice(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()) + 1);
    EXPECT_EQ(read_coefficients().output_row_words, depth == 3 ? 4 : 6);
    u8 expected[768];
    spatial_expected(expected, depth, false, false, true);
    u32 actual[192];
    mdec_dma_read(actual, spatial_word_count(depth));
    EXPECT_EQ(memcmp(actual, expected, spatial_word_count(depth) * 4), 0);
    end_decode();
  }
}
