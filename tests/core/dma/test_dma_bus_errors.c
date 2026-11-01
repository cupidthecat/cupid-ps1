/* cupid-ps1 DMA physical-window and device-side-effect regressions. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/dma/dma.h"

enum { LAST_WORD = 0x7FFFFC, GPU_BASE = 0x20, OTC_BASE = 0x60 };

static void begin_dma(display_fixture_t* fixture, bool read) {
  begin_display(fixture);
  dma_initialize();
  gpu_write_register(4, read ? 0x04000003 : 0x04000002);
}

static void end_dma(display_fixture_t* fixture) {
  dma_shutdown();
  end_display(fixture);
}

static void put_ram(u32 address, u32 value) {
  memcpy(bus_get_unprotected_ram_pointer() + (address & bus_get_ram_mask()), &value, 4);
}

static u32 get_ram(u32 address) {
  u32 value;
  memcpy(&value, bus_get_unprotected_ram_pointer() + (address & bus_get_ram_mask()), 4);
  return value;
}

static void start_dma(u32 channel, u32 address, u32 block, u32 control) {
  dma_write_register(channel * 16, address);
  dma_write_register(channel * 16 + 4, block);
  dma_write_register(0x70, 8u << (channel * 4));
  dma_write_register(channel * 16 + 8, control);
}

static void check_error(u32 channel, u32 dicr) {
  EXPECT_EQ(dma_read_register(0x74), dicr | 0x80008000u);
  EXPECT_FALSE(dma_read_register(channel * 16 + 8) & (1u << 24));
  EXPECT_TRUE(interrupt_controller_read_register(0) & (1u << INTERRUPT_CONTROLLER_IRQ_DMA));
}

TEST(DmaBusError, otc_retains_valid_prefix_without_early_terminator) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  put_ram(0, 0x44444444);
  put_ram(4, 0x44444444);
  put_ram(LAST_WORD, 0x44444444);
  start_dma(6, 4, 3, 0x11000000);
  EXPECT_EQ(get_ram(4), 0);
  EXPECT_EQ(get_ram(0), 0x1FFFFC);
  EXPECT_EQ(get_ram(LAST_WORD), 0x44444444);
  check_error(6, 0);
  EXPECT_EQ(dma_read_register(OTC_BASE), 4);
  EXPECT_EQ(dma_read_register(OTC_BASE + 4), 3);
  end_dma(&fixture);
}

TEST(DmaBusError, invalid_start_has_no_ram_or_completion_side_effects) {
  const u32 enables[] = {0, 1u << 23, (1u << 23) | (1u << 22)};
  for (u32 i = 0; i < 3; i++) {
    display_fixture_t fixture;
    begin_dma(&fixture, false);
    put_ram(0, 0x44444444);
    dma_write_register(0x74, enables[i]);
    start_dma(6, 0x800000, 1, 0x11000000);
    EXPECT_EQ(get_ram(0), 0x44444444);
    check_error(6, enables[i]);
    EXPECT_EQ(dma_dbg_transfer_counts()[6], 0);
    end_dma(&fixture);
  }
}

TEST(DmaBusError, gpu_manual_write_preserves_first_device_word) {
  for (u32 reverse = 0; reverse < 2; reverse++) {
    display_fixture_t fixture;
    begin_dma(&fixture, false);
    put_ram(reverse ? 0 : LAST_WORD, 0x1F000000);
    start_dma(2, reverse ? 0 : LAST_WORD, 2, 0x11000001 | reverse << 1);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    check_error(2, 0);
    end_dma(&fixture);
  }
}

static void prepare_readback(void) {
  gpu_write_register(0, 0xA0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010004);
  gpu_write_register(0, 0x12345678);
  gpu_write_register(0, 0x23456789);
  gpu_write_register(0, 0xC0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010004);
}

TEST(DmaBusError, gpu_manual_read_stops_consuming_at_fault) {
  display_fixture_t fixture;
  begin_dma(&fixture, true);
  prepare_readback();
  put_ram(LAST_WORD, 0x44444444);
  start_dma(2, LAST_WORD, 2, 0x11000000);
  EXPECT_EQ(get_ram(LAST_WORD), 0x12345678);
  EXPECT_EQ(gpu_read_register(0), 0x23456789);
  check_error(2, 0);
  end_dma(&fixture);
}

TEST(DmaBusError, request_read_and_write_preserve_partial_block) {
  for (u32 read = 0; read < 2; read++) {
    display_fixture_t fixture;
    begin_dma(&fixture, read);
    if (read) prepare_readback();
    put_ram(LAST_WORD, read ? 0x44444444 : 0x1F000000);
    start_dma(2, LAST_WORD, 0x00020002, 0x01000200 | (read ? 0 : 1));
    if (read) {
      EXPECT_EQ(get_ram(LAST_WORD), 0x12345678);
      EXPECT_EQ(gpu_read_register(0), 0x23456789);
    } else
      EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    EXPECT_EQ(dma_read_register(GPU_BASE), LAST_WORD);
    EXPECT_EQ(dma_read_register(GPU_BASE + 4), 0x00010002);
    check_error(2, 0);
    end_dma(&fixture);
  }
}

TEST(DmaBusError, last_valid_linked_header_is_readable) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  put_ram(LAST_WORD, 0x00FFFFFF);
  start_dma(2, LAST_WORD, 0, 0x01000401);
  EXPECT_EQ(dma_read_register(GPU_BASE), 0xFFFFFF);
  EXPECT_EQ(dma_read_register(0x74), 0);
  EXPECT_FALSE(dma_read_register(GPU_BASE + 8) & (1u << 24));
  end_dma(&fixture);
}

TEST(DmaBusError, linked_payload_uses_its_own_address_and_preserves_prefix) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  put_ram(LAST_WORD - 4, 0x02FFFFFF);
  put_ram(LAST_WORD, 0x1F000000);
  start_dma(2, LAST_WORD - 4, 0, 0x01000401);
  EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
  EXPECT_EQ(dma_read_register(GPU_BASE), 0xFFFFFF);
  check_error(2, 0);
  end_dma(&fixture);
}

TEST(DmaBusError, fault_after_last_header_does_not_emit_payload) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  put_ram(LAST_WORD, 0x01FFFFFF);
  start_dma(2, LAST_WORD, 0, 0x01000401);
  EXPECT_FALSE(gpu_read_register(4) & (1u << 24));
  EXPECT_EQ(dma_read_register(GPU_BASE), 0xFFFFFF);
  check_error(2, 0);
  end_dma(&fixture);
}

TEST(DmaBusError, preserves_prior_flags_and_retriggers_after_acknowledgement) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  dma_write_register(0x74, 0x00C40000);
  start_dma(6, 0x30000, 1, 0x11000000);
  EXPECT_EQ(dma_read_register(0x74), 0xC0C40000);
  start_dma(2, 0x800000, 1, 0x11000001);
  EXPECT_EQ(dma_read_register(0x74), 0xC0C48000);
  EXPECT_EQ(dma_dbg_transfer_counts()[2], 0);
  dma_write_register(0x74, 0x40C40000);
  EXPECT_EQ(dma_read_register(0x74), 0x00C40000);
  interrupt_controller_write_register(0, ~(1u << INTERRUPT_CONTROLLER_IRQ_DMA));
  EXPECT_FALSE(interrupt_controller_read_register(0) & (1u << INTERRUPT_CONTROLLER_IRQ_DMA));
  start_dma(2, 0x800000, 1, 0x11000001);
  check_error(2, 0x00C40000);
  end_dma(&fixture);
}

TEST(DmaBusError, restored_fault_keeps_gpu_fifo_and_dma_record) {
  display_fixture_t fixture;
  begin_dma(&fixture, true);
  prepare_readback();
  start_dma(2, LAST_WORD, 2, 0x11000000);
  EXPECT_EQ(get_ram(LAST_WORD), 0x12345678);
  u8           dma[256], check[256];
  const size_t capacity = 2 * 1024 * 1024;
  u8*          gpu      = malloc(capacity);
  EXPECT_NOT_NULL(gpu);
  state_wrapper_t state;
  state_wrapper_init_write(&state, dma, sizeof(dma), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t dma_size = state_wrapper_get_position(&state);
  state_wrapper_init_write(&state, gpu, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  const size_t gpu_size = state_wrapper_get_position(&state);
  EXPECT_EQ(gpu_read_register(0), 0x23456789);
  gpu_reset(false);
  dma_reset();
  state_wrapper_init_read(&state, gpu, gpu_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(gpu_do_state(&state));
  state_wrapper_init_read(&state, dma, dma_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  state_wrapper_init_write(&state, check, sizeof(check), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), dma_size);
  EXPECT_EQ(memcmp(dma, check, dma_size), 0);
  EXPECT_EQ(gpu_read_register(0), 0x23456789);
  check_error(2, 0);
  free(gpu);
  end_dma(&fixture);
}

TEST(DmaBusError, completing_at_last_valid_word_does_not_fault) {
  for (u32 request = 0; request < 2; request++) {
    display_fixture_t fixture;
    begin_dma(&fixture, false);
    put_ram(LAST_WORD, 0x1F000000);
    dma_write_register(0x74, 0x00840000);
    const u32 completions = dma_dbg_transfer_counts()[2];
    start_dma(2, LAST_WORD, request ? 0x00010001 : 1, request ? 0x01000201 : 0x11000001);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    EXPECT_EQ(dma_read_register(0x74), 0x84840000);
    EXPECT_EQ(dma_dbg_transfer_counts()[2], completions + 1);
    EXPECT_EQ(dma_read_register(GPU_BASE), request ? 0x800000 : LAST_WORD);
    EXPECT_EQ(dma_read_register(GPU_BASE + 4), 1);
    end_dma(&fixture);
  }
}

TEST(DmaBusError, request_count_and_size_zero_fault_in_first_block) {
  const u32 blocks[] = {0x00000002, 0x00020000};
  const u32 after[]  = {0xFFFF0002, 0x00010000};
  for (u32 i = 0; i < 2; i++) {
    display_fixture_t fixture;
    begin_dma(&fixture, false);
    put_ram(LAST_WORD, 0x1F000000);
    start_dma(2, LAST_WORD, blocks[i], 0x01000201);
    EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
    EXPECT_EQ(dma_read_register(GPU_BASE), LAST_WORD);
    EXPECT_EQ(dma_read_register(GPU_BASE + 4), after[i]);
    check_error(2, 0);
    end_dma(&fixture);
  }
}

TEST(DmaBusError, configured_physical_window_stops_before_backing_ram_wrap) {
  display_fixture_t fixture;
  begin_dma(&fixture, false);
  bus_memory_write_handler_t* writes =
      bus_offset_write_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
  writes[0x1F801060 >> BUS_MEMORY_LUT_PAGE_SHIFT](0x1F801060, 0x888);
  EXPECT_EQ(bus_get_ram_mapped_size(), 0x200000);
  put_ram(0x1FFFFC, 0x1F000000);
  put_ram(0, 0x44444444);
  start_dma(2, 0x1FFFFC, 2, 0x11000001);
  EXPECT_TRUE(gpu_read_register(4) & (1u << 24));
  EXPECT_EQ(get_ram(0), 0x44444444);
  check_error(2, 0);
  end_dma(&fixture);
}
