/* cupid-ps1 ordering-table control, transfer and restoration regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"

enum { OTC_BASE = 0x60, OTC_BLOCK = 0x64, OTC_CONTROL = 0x68, OTC_MASK = 0x51000000 };

static void begin_otc(void) {
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
  interrupt_controller_reset();
  dma_initialize();
}

static void end_otc(void) {
  dma_shutdown();
  finish_cpu();
}

static void prepare_otc(u32 address, u32 count) {
  u8* ram = bus_get_unprotected_ram_pointer();
  for (u32 i = 0; i < count; i++) {
    const u32 value = 0x44444444;
    memcpy(ram + ((address - i * 4) & bus_get_ram_mask()), &value, 4);
  }
  dma_write_register(OTC_BASE, address);
  dma_write_register(OTC_BLOCK, count & 0xFFFF);
}

static void check_otc(u32 address, u32 count) {
  u8* ram         = bus_get_unprotected_ram_pointer();
  u32 differences = 0;
  for (u32 i = 0; i < count; i++) {
    u32       actual;
    const u32 offset = (address - i * 4) & bus_get_ram_mask();
    memcpy(&actual, ram + offset, 4);
    const u32 expected = i + 1 == count ? 0xFFFFFF : (offset - 4) & bus_get_ram_mask();
    differences += actual != expected;
  }
  EXPECT_EQ(differences, 0);
  EXPECT_EQ(dma_read_register(OTC_CONTROL) & ~0x40000000u, 2);
  EXPECT_EQ(dma_read_register(OTC_BASE), address);
  EXPECT_EQ(dma_read_register(OTC_BLOCK), count & 0xFFFF);
  EXPECT_EQ(dma_dbg_request(DMA_CHANNEL_OTC), 0);
}

TEST(DmaOtc, reset_and_all_control_bits) {
  begin_otc();
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 2);
  for (u32 bit = 0; bit < 32; bit++) {
    dma_write_register(OTC_CONTROL, 1u << bit);
    EXPECT_EQ(dma_read_register(OTC_CONTROL), ((1u << bit) & OTC_MASK) | 2);
  }
  dma_write_register(OTC_CONTROL, 0xFFFFFFFF);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), OTC_MASK | 2);
  dma_write_register(OTC_CONTROL, 0);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 2);
  EXPECT_EQ(dma_dbg_request(DMA_CHANNEL_OTC), 0);
  dma_reset();
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 2);
  end_otc();
}

TEST(DmaOtc, direction_sync_and_chopping_bits_are_ignored) {
  begin_otc();
  dma_write_register(0x70, 8u << 24);
  for (u32 sync = 0; sync < 4; sync++) {
    for (u32 direction = 0; direction < 2; direction++) {
      prepare_otc(0x3100C, 4);
      dma_write_register(OTC_CONTROL, 0x11770100 | sync << 9 | direction);
      check_otc(0x3100C, 4);
    }
  }
  end_otc();
}

TEST(DmaOtc, trigger_and_master_enable_gate_transfer) {
  begin_otc();
  prepare_otc(0x3100C, 4);
  dma_write_register(OTC_CONTROL, 0x11000000);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x11000002);
  dma_write_register(0x70, 8u << 24);
  check_otc(0x3100C, 4);
  prepare_otc(0x3100C, 4);
  dma_write_register(OTC_CONTROL, 0x01000000);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x01000002);
  u32 value;
  memcpy(&value, bus_get_unprotected_ram_pointer() + 0x3100C, 4);
  EXPECT_EQ(value, 0x44444444);
  dma_write_register(OTC_CONTROL, 0x10000000);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x10000002);
  memcpy(&value, bus_get_unprotected_ram_pointer() + 0x3100C, 4);
  EXPECT_EQ(value, 0x44444444);
  dma_write_register(OTC_CONTROL, 0x51000000);
  check_otc(0x3100C, 4);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x40000002);
  end_otc();
}

TEST(DmaOtc, zero_count_and_mirrored_ram_boundary) {
  begin_otc();
  dma_write_register(0x70, 8u << 24);
  prepare_otc(0x7FFFC, 65536);
  dma_write_register(OTC_CONTROL, 0x11000000);
  check_otc(0x7FFFC, 65536);
  prepare_otc(0x200004, 4);
  dma_write_register(OTC_CONTROL, 0x11000000);
  check_otc(0x200004, 4);
  end_otc();
}

TEST(DmaOtc, completion_irq_and_acknowledgement) {
  begin_otc();
  dma_write_register(0x70, 8u << 24);
  dma_write_register(0x74, (1u << 23) | (1u << 22));
  prepare_otc(0x3100C, 4);
  dma_write_register(OTC_CONTROL, 0x11000000);
  check_otc(0x3100C, 4);
  EXPECT_EQ(dma_read_register(0x74), 0xC0C00000);
  EXPECT_TRUE(interrupt_controller_read_register(0) & (1u << INTERRUPT_CONTROLLER_IRQ_DMA));
  dma_write_register(0x74, 0x40C00000);
  EXPECT_EQ(dma_read_register(0x74), 0x00C00000);
  end_otc();
}

TEST(DmaOtc, old_pending_control_is_normalized_on_load) {
  begin_otc();
  prepare_otc(0x3100C, 4);
  dma_write_register(OTC_CONTROL, 0x11000000);
  u8              bytes[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, sizeof(bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  /* The existing record is halt ticks then seven {MADR,BCR,CHCR,request} tuples. */
  const u32 old_control = 0x31770701;
  memcpy(bytes + 4 + 6 * 13 + 8, &old_control, 4);
  bytes[4 + 6 * 13 + 12] = 0;
  dma_reset();
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x11000002);
  EXPECT_EQ(dma_dbg_request(DMA_CHANNEL_OTC), 1);
  dma_write_register(0x70, 8u << 24);
  check_otc(0x3100C, 4);
  end_otc();
}

TEST(DmaOtc, completed_state_round_trip_is_exact) {
  begin_otc();
  prepare_otc(0x3100C, 4);
  dma_write_register(0x70, 8u << 24);
  dma_write_register(OTC_CONTROL, 0x51000000);
  check_otc(0x3100C, 4);
  u8              before[256], after[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, before, sizeof(before), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size = state_wrapper_get_position(&state);
  dma_reset();
  state_wrapper_init_read(&state, before, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  state_wrapper_init_write(&state, after, sizeof(after), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  EXPECT_EQ(memcmp(before, after, size), 0);
  end_otc();
}

TEST(DmaOtc, restored_stale_request_cannot_start_without_trigger) {
  begin_otc();
  prepare_otc(0x3100C, 4);
  dma_write_register(OTC_CONTROL, 0x01000000);
  u8              bytes[256];
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, sizeof(bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  const size_t size      = state_wrapper_get_position(&state);
  bytes[4 + 6 * 13 + 12] = 1;
  dma_reset();
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(dma_do_state(&state));
  EXPECT_EQ(dma_dbg_request(DMA_CHANNEL_OTC), 0);
  dma_write_register(0x70, 8u << 24);
  u32 value;
  memcpy(&value, bus_get_unprotected_ram_pointer() + 0x3100C, 4);
  EXPECT_EQ(value, 0x44444444);
  EXPECT_EQ(dma_read_register(OTC_CONTROL), 0x01000002);
  end_otc();
}

TEST(DmaOtc, other_channel_control_masks_remain_writable) {
  begin_otc();
  for (u32 channel = 0; channel < DMA_CHANNEL_OTC; channel++) {
    dma_write_register(channel * 16 + 8, 0xFFFFFFFF);
    EXPECT_EQ(dma_read_register(channel * 16 + 8), 0x71770703);
  }
  end_otc();
}
