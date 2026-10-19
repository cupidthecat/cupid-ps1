/* cupid-ps1 audio/transfer ordering and scheduler restoration. */
#include "tests/core/spu/spu_clock_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"
#include "tests/core/spu/spu_state_fixture.h"

enum { SOUND_CAPACITY = SPU_RAM_SIZE + 65536 };

static void begin_timeline(void)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  interrupt_controller_reset();
  dma_initialize();
  spu_initialize();
  spu_write_register(0x1A2, 0xF000);
}

static void end_timeline(void)
{
  spu_shutdown();
  dma_shutdown();
  finish_cpu();
}

TEST(SpuTransferTimeline, capture_before_write_at_sample_boundary)
{
  begin_timeline();
  advance_spu_clock(752);
  spu_write_register(0x1A6, 0);
  spu_write_register(0x1AA, 0x20);
  const u32 word = 0x1234ABCD;
  spu_dma_write(&word, 1);
  advance_spu_clock(16);
  u16 value;
  memcpy(&value, spu_get_ram(), sizeof(value));
  EXPECT_EQ(value, 0xABCD);
  advance_spu_clock(16);
  memcpy(&value, spu_get_ram() + 2, sizeof(value));
  EXPECT_EQ(value, 0x1234);
  advance_spu_clock(752);
  memcpy(&value, spu_get_ram() + 2, sizeof(value));
  EXPECT_EQ(value, 0);
  memcpy(&value, spu_get_ram(), sizeof(value));
  EXPECT_EQ(value, 0xABCD);
  end_timeline();
}

TEST(SpuTransferTimeline, read_keeps_data_before_later_capture)
{
  begin_timeline();
  memset(spu_get_writable_ram(), 0xFF, 64);
  advance_spu_clock(752);
  spu_write_register(0x1A6, 0);
  spu_write_register(0x1AA, 0x30);
  advance_spu_clock(784);
  u32 word;
  spu_dma_read(&word, 1);
  EXPECT_EQ(word, 0xFFFF0000);
  end_timeline();
}

TEST(SpuTransferTimeline, address_change_preserves_elapsed_work)
{
  begin_timeline();
  spu_write_register(0x1A6, 0x10000 / 8);
  spu_write_register(0x1AA, 0x20);
  const u32 words[] = {0x1234ABCD, 0x5678CAFE};
  spu_dma_write(words, 2);
  cpu_add_pending_ticks(20);
  /* No status read precedes the address write. Only one halfword is due. */
  spu_write_register(0x1A6, 0x20000 / 8);
  u32 old, changed;
  memcpy(&old, spu_get_ram() + 0x10000, sizeof(old));
  EXPECT_EQ(old, 0x0000ABCD);
  advance_spu_clock(11);
  memcpy(&changed, spu_get_ram() + 0x20000, sizeof(changed));
  EXPECT_EQ(changed, 0);
  advance_spu_clock(1);
  memcpy(&changed, spu_get_ram() + 0x20000, sizeof(changed));
  EXPECT_EQ(changed, 0x1234);
  end_timeline();
}

TEST(SpuTransferTimeline, reset_cancels_pending_transfer)
{
  begin_timeline();
  spu_write_register(0x1A6, 0x10000 / 8);
  spu_write_register(0x1AA, 0x20);
  const u32 word = 0x1234ABCD;
  spu_dma_write(&word, 1);
  advance_spu_clock(15);
  spu_reset();
  advance_spu_clock(1000);
  u32 actual;
  memcpy(&actual, spu_get_ram() + 0x10000, sizeof(actual));
  EXPECT_EQ(actual, 0);
  end_timeline();
}

TEST(SpuTransferTimeline, legacy_deadline_uses_restored_clock)
{
  for (u32 version = 88; version <= 89; version++) {
    begin_timeline();
    advance_spu_clock(1000);
    spu_write_register(0x1A6, 0x10000 / 8);
    spu_write_register(0x1AA, 0x20);
    const u32 word = 0x1234ABCD;
    spu_dma_write(&word, 1);
    advance_spu_clock(7);
    spu_execution_snapshot_t legacy = save_spu_execution(version);
    /* Older streams contain the audio event but no transfer-event record. */
    remove_legacy_transfer_event(&legacy);
    advance_spu_clock(10000);
    load_spu_execution(&legacy);
    const tick_count_t remaining = version == 88 ? 16 : 9;
    advance_spu_clock(remaining - 1);
    u32 actual;
    memcpy(&actual, spu_get_ram() + 0x10000, sizeof(actual));
    EXPECT_EQ(actual, 0);
    advance_spu_clock(1);
    memcpy(&actual, spu_get_ram() + 0x10000, sizeof(actual));
    EXPECT_EQ(actual, 0xABCD);
    free_spu_execution(&legacy);
    end_timeline();
  }
}

TEST(SpuTransferTimeline, irq_fires_without_status_polling)
{
  for (u32 read = 0; read < 2; read++) {
    begin_timeline();
    spu_write_register(0x1A6, 0x10000 / 8);
    spu_write_register(0x1A4, 0x10008 / 8);
    spu_write_register(0x1AA, read ? 0x70 : 0x60);
    if (!read) {
      const u32 words[] = {0x1234ABCD, 0x5678CAFE};
      spu_dma_write(words, 2);
    }
    cpu_add_pending_ticks(63);
    timing_events_run_events();
    EXPECT_EQ(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_SPU), 0);
    cpu_add_pending_ticks(1);
    timing_events_run_events();
    EXPECT_NE(irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_SPU), 0);
    end_timeline();
  }
}
