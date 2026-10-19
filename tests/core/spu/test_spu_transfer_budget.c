/* cupid-ps1 transfer-budget conservation and state continuation. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/spu/spu.h"

enum { TRANSFER_BASE = 0x10000, STATE_CAPACITY = SPU_RAM_SIZE + 65536 };

static size_t save_sound(u8* bytes, u32 version)
{
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, bytes, STATE_CAPACITY, version);
  EXPECT_TRUE(spu_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  return state_wrapper_get_position(&sw);
}

static bool load_sound(u8* bytes, size_t length, u32 version)
{
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, bytes, length, version);
  return spu_do_state(&sw) && !state_wrapper_has_error(&sw) && state_wrapper_get_position(&sw) == length;
}

static void begin_budget(bool read)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  interrupt_controller_reset();
  dma_initialize();
  spu_initialize();
  spu_write_register(0x1A6, TRANSFER_BASE / 8);
  if (read) {
    for (u32 i = 0; i < 128; i++) {
      const u16 value = (u16)(0x1357 ^ (i * 0x139));
      memcpy(spu_get_writable_ram() + TRANSFER_BASE + i * 2, &value, sizeof(value));
    }
  }
  spu_write_register(0x1AA, read ? 0x30 : 0x20);
  if (!read) {
    u32 words[16];
    for (u32 i = 0; i < 16; i++) words[i] = 0x1234ABCD ^ (i * 0x00310073);
    spu_dma_write(words, 16);
  }
}

static void end_budget(void)
{
  spu_shutdown();
  dma_shutdown();
  finish_cpu();
}

static void expect_progress(u32 halfwords, bool read)
{
  u8* bytes = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  const size_t length = save_sound(bytes, SAVE_STATE_VERSION);
  u32 address, size;
  memcpy(&address, bytes + 10, sizeof(address));
  memcpy(&size, bytes + length - SPU_RAM_SIZE - sizeof(size), sizeof(size));
  printf("spu-budget read=%u halfwords=%u address=%08X size=%u\n", read, halfwords, address, size);
  fflush(stdout);
  EXPECT_EQ(address, TRANSFER_BASE + 2 * halfwords);
  EXPECT_EQ(size, read ? halfwords : 32 - halfwords);
  if (read) {
    u32 head;
    memcpy(&head, bytes + length - SPU_RAM_SIZE - 12, sizeof(head));
    for (u32 i = 0; i < halfwords; i++) {
      u16 value;
      memcpy(&value, bytes + length - SPU_RAM_SIZE - 76 + ((head + i) % 32) * 2, sizeof(value));
      EXPECT_EQ(value, (u16)(0x1357 ^ (i * 0x139)));
    }
  } else {
    for (u32 i = 0; i < 32; i++) {
      u16 value;
      memcpy(&value, spu_get_ram() + TRANSFER_BASE + i * 2, sizeof(value));
      const u32 word = 0x1234ABCD ^ ((i / 2) * 0x00310073);
      EXPECT_EQ(value, i < halfwords ? (u16)(word >> (16 * (i & 1))) : 0);
    }
  }
  free(bytes);
}

static void check_budget(bool read)
{
  begin_budget(read);
  spu_execute(16);
  expect_progress(1, read);
  spu_execute(0);
  expect_progress(1, read);
  spu_execute(1);
  expect_progress(1, read);
  spu_execute(14);
  expect_progress(1, read);
  spu_execute(1);
  expect_progress(2, read);
  spu_execute(17);
  expect_progress(3, read);
  spu_execute(15);
  expect_progress(4, read);
  end_budget();
}

TEST(SpuTransferBudget, read_consumes_ticks_once) { check_budget(true); }
TEST(SpuTransferBudget, write_consumes_ticks_once) { check_budget(false); }

TEST(SpuTransferBudget, batching_preserves_complete_state)
{
  u8* batched = calloc(1, STATE_CAPACITY);
  u8* split = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(batched);
  EXPECT_NOT_NULL(split);
  for (u32 read = 0; read < 2; read++) {
    for (u32 ticks = 17; ticks <= 511; ticks += 19) {
      begin_budget(read != 0);
      spu_execute((tick_count_t)ticks);
      const size_t length = save_sound(batched, SAVE_STATE_VERSION);
      end_budget();
      begin_budget(read != 0);
      for (u32 i = 0; i < ticks; i++) spu_execute(1);
      EXPECT_EQ(save_sound(split, SAVE_STATE_VERSION), length);
      EXPECT_EQ(memcmp(batched, split, length), 0);
      end_budget();
    }
  }
  free(split);
  free(batched);
}

TEST(SpuTransferBudget, restore_continues_fractional_ticks)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  u8* expected = calloc(1, STATE_CAPACITY);
  u8* actual = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  EXPECT_NOT_NULL(expected);
  EXPECT_NOT_NULL(actual);
  for (u32 read = 0; read < 2; read++) {
    for (u32 phase = 1; phase < 16; phase++) {
      begin_budget(read != 0);
      spu_execute((tick_count_t)(16 + phase));
      const size_t length = save_sound(saved, SAVE_STATE_VERSION);
      spu_execute((tick_count_t)(16 - phase));
      EXPECT_EQ(save_sound(expected, SAVE_STATE_VERSION), length);
      spu_reset();
      EXPECT_TRUE(load_sound(saved, length, SAVE_STATE_VERSION));
      spu_execute((tick_count_t)(16 - phase));
      EXPECT_EQ(save_sound(actual, SAVE_STATE_VERSION), length);
      EXPECT_EQ(memcmp(actual, expected, length), 0);
      expect_progress(2, read != 0);
      end_budget();
    }
  }
  free(actual);
  free(expected);
  free(saved);
}

TEST(SpuTransferBudget, completed_fifo_has_no_idle_credit)
{
  for (u32 read = 0; read < 2; read++) {
    begin_budget(read != 0);
    spu_execute(32 * 16 + 15);
    expect_progress(32, read != 0);
    spu_execute(100);
    if (read) {
      u32 words[16];
      spu_dma_read(words, 16);
    } else {
      const u32 word = 0x87654321;
      spu_dma_write(&word, 1);
    }
    spu_execute(0);
    spu_execute(1);
    u16 value = 0;
    if (read) {
      u32 word;
      spu_dma_read(&word, 1);
      EXPECT_EQ(word, 0);
      spu_execute(15);
      spu_dma_read(&word, 1);
      const u16 expected = (u16)(0x1357 ^ (32 * 0x139));
      EXPECT_EQ(word, (u32)expected * 0x10001u);
    } else {
      memcpy(&value, spu_get_ram() + TRANSFER_BASE + 64, sizeof(value));
      EXPECT_EQ(value, 0);
      spu_execute(15);
      memcpy(&value, spu_get_ram() + TRANSFER_BASE + 64, sizeof(value));
      EXPECT_EQ(value, 0x4321);
    }
    end_budget();
  }
}

TEST(SpuTransferBudget, legacy_state_drops_unrecorded_credit)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  begin_budget(false);
  spu_execute(7);
  const size_t length = save_sound(saved, 88);
  spu_execute(8);
  EXPECT_TRUE(load_sound(saved, length, 88));
  spu_execute(1);
  expect_progress(0, false);
  spu_execute(15);
  expect_progress(1, false);
  end_budget();
  free(saved);
}

TEST(SpuTransferBudget, invalid_fifo_state_is_rejected)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  for (u32 field = 0; field < 4; field++) {
    begin_budget(true);
    const size_t length = save_sound(saved, SAVE_STATE_VERSION);
    const size_t fifo_indices = length - SPU_RAM_SIZE - 3 * sizeof(u32);
    const u32 bad = field < 2 ? 32 : field == 2 ? 33 : 1;
    memcpy(saved + fifo_indices + (field < 3 ? field : 1) * sizeof(u32), &bad, sizeof(bad));
    EXPECT_FALSE(load_sound(saved, length, SAVE_STATE_VERSION));
    end_budget();
  }
  free(saved);
}

TEST(SpuTransferBudget, invalid_fractional_state_is_rejected)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  const s32 invalid[] = {-1, 16, INT32_MAX};
  for (u32 i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    begin_budget(true);
    const size_t length = save_sound(saved, SAVE_STATE_VERSION);
    EXPECT_TRUE(SAVE_STATE_VERSION >= 89);
    memcpy(saved + length - SPU_RAM_SIZE - 76 - sizeof(s32), &invalid[i], sizeof(invalid[i]));
    EXPECT_FALSE(load_sound(saved, length, SAVE_STATE_VERSION));
    end_budget();
  }
  free(saved);
}

TEST(SpuTransferBudget, truncated_state_is_rejected)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  begin_budget(true);
  const size_t length = save_sound(saved, SAVE_STATE_VERSION);
  EXPECT_FALSE(load_sound(saved, length - 1, SAVE_STATE_VERSION));
  end_budget();
  free(saved);
}

TEST(SpuTransferBudget, transfer_irq_waits_for_its_address)
{
  for (u32 read = 0; read < 2; read++) {
    begin_budget(read != 0);
    spu_write_register(0x1A4, (TRANSFER_BASE + 8) / 8);
    spu_write_register(0x1AA, read ? 0x70 : 0x60);
    spu_execute(3 * 16 + 15);
    expect_progress(3, read != 0);
    EXPECT_EQ(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_SPU), 0);
    spu_execute(0);
    EXPECT_EQ(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_SPU), 0);
    spu_execute(1);
    expect_progress(4, read != 0);
    EXPECT_NE(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_SPU), 0);
    end_budget();
  }
}

TEST(SpuTransferBudget, nonpositive_slices_preserve_state)
{
  u8* saved = calloc(1, STATE_CAPACITY);
  u8* actual = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(saved);
  EXPECT_NOT_NULL(actual);
  begin_budget(false);
  spu_execute(7);
  const size_t length = save_sound(saved, SAVE_STATE_VERSION);
  spu_execute(0);
  spu_execute(-1);
  EXPECT_EQ(save_sound(actual, SAVE_STATE_VERSION), length);
  EXPECT_EQ(memcmp(saved, actual, length), 0);
  end_budget();
  free(actual);
  free(saved);
}

TEST(SpuTransferBudget, dma_refill_shares_the_elapsed_budget)
{
  begin_budget(false);
  spu_reset();
  spu_write_register(0x1A6, TRANSFER_BASE / 8);
  spu_write_register(0x1AA, 0x20);
  u32 words[64];
  for (u32 i = 0; i < 64; i++) words[i] = 0x1248ABCD ^ (i * 0x17731439u);
  memcpy(bus_get_unprotected_ram_pointer() + 0x40000, words, sizeof(words));
  dma_write_register(0x70, 0x80000);
  dma_write_register(0x40, 0x40000);
  dma_write_register(0x44, 0x40010);
  dma_write_register(0x48, 0x01000201);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 3);
  spu_execute(31 * 16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 3);
  EXPECT_EQ(memcmp(spu_get_ram() + TRANSFER_BASE, words, 31 * 2), 0);
  spu_execute(16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 2);
  EXPECT_EQ(memcmp(spu_get_ram() + TRANSFER_BASE, words, 32 * 2), 0);
  spu_execute(3 * 32 * 16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 0);
  EXPECT_EQ(dma_dbg_channel_control(4) & (1u << 24), 0);
  EXPECT_EQ(memcmp(spu_get_ram() + TRANSFER_BASE, words, sizeof(words)), 0);
  end_budget();
}

TEST(SpuTransferBudget, dma_read_blocks_share_the_elapsed_budget)
{
  begin_budget(true);
  memset(bus_get_unprotected_ram_pointer() + 0x40000, 0xA5, 64 * sizeof(u32));
  dma_write_register(0x70, 0x80000);
  dma_write_register(0x40, 0x40000);
  dma_write_register(0x44, 0x40010);
  dma_write_register(0x48, 0x01000200);
  spu_execute(31 * 16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 4);
  spu_execute(16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 3);
  spu_execute(3 * 32 * 16);
  EXPECT_EQ(dma_dbg_block_control(4) >> 16, 0);
  EXPECT_EQ(dma_dbg_channel_control(4) & (1u << 24), 0);
  EXPECT_EQ(memcmp(bus_get_unprotected_ram_pointer() + 0x40000, spu_get_ram() + TRANSFER_BASE, 64 * sizeof(u32)), 0);
  end_budget();
}
