/* cupid-ps1 ready-channel priority selection and device ordering. */
#include "tests/core/gpu/gpu_reset_display_fixture.h"
#include "core/dma/dma.h"
#include "core/spu/spu.h"

enum { ADDRESS = 0x60000, PIXEL = 0x23456789 };

static void prepare_gpu_read(u32 count) {
  gpu_write_register(4, 0x04000003);
  gpu_write_register(0, 0xA0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010000 | count * 2);
  for (u32 i = 0; i < count; i++)
    gpu_write_register(0, PIXEL + i);
  gpu_write_register(0, 0xC0000000);
  gpu_write_register(0, 0);
  gpu_write_register(0, 0x00010000 | count * 2);
}

static void queue_collision(bool gpu_read) {
  if (gpu_read)
    prepare_gpu_read(1);
  else
    gpu_write_register(4, 0x04000002);
  const u32 source = 0x1F000000;
  memcpy(bus_get_unprotected_ram_pointer() + ADDRESS, &source, 4);
  dma_write_register(0x20, ADDRESS);
  dma_write_register(0x24, 1);
  dma_write_register(0x28, gpu_read ? 0x11000000 : 0x11000001);
  dma_write_register(0x60, ADDRESS);
  dma_write_register(0x64, 1);
  dma_write_register(0x68, 0x11000000);
  dma_write_register(0x74, 0x00C40000);
}

static u32 priorities(u32 gpu, u32 otc) {
  return (gpu | 8) << 8 | (otc | 8) << 24;
}

TEST(DmaPriority, all_gpu_otc_priority_pairs_order_ram_writes) {
  display_fixture_t fixture;
  begin_display(&fixture);
  dma_initialize();
  u32 mismatches = 0;
  for (u32 gpu = 0; gpu < 8; gpu++) {
    for (u32 otc = 0; otc < 8; otc++) {
      dma_reset();
      gpu_reset(false);
      queue_collision(true);
      dma_write_register(0x70, priorities(gpu, otc));
      u32 actual;
      memcpy(&actual, bus_get_unprotected_ram_pointer() + ADDRESS, 4);
      mismatches += actual != (gpu < otc ? 0x00FFFFFFu : PIXEL);
      EXPECT_FALSE(dma_read_register(0x28) & (1u << 24));
      EXPECT_EQ(dma_read_register(0x68), 2);
      EXPECT_EQ(dma_read_register(0x74), 0xC4C40000);
    }
  }
  EXPECT_EQ(mismatches, 0);
  dma_shutdown();
  end_display(&fixture);
}

TEST(DmaPriority, all_gpu_otc_priority_pairs_order_device_commands) {
  display_fixture_t fixture;
  begin_display(&fixture);
  dma_initialize();
  u32 mismatches = 0;
  for (u32 gpu = 0; gpu < 8; gpu++) {
    for (u32 otc = 0; otc < 8; otc++) {
      dma_reset();
      gpu_reset(false);
      queue_collision(false);
      dma_write_register(0x70, priorities(gpu, otc));
      mismatches += !!(gpu_read_register(4) & (1u << 24)) != (gpu < otc);
      EXPECT_EQ(dma_read_register(0x74), 0xC4C40000);
    }
  }
  EXPECT_EQ(mismatches, 0);
  dma_shutdown();
  end_display(&fixture);
}

TEST(DmaPriority, disabled_higher_priority_channel_waits_for_enable) {
  display_fixture_t fixture;
  begin_display(&fixture);
  dma_initialize();
  queue_collision(false);
  dma_write_register(0x70, 7u << 8 | 8u << 24);
  EXPECT_TRUE(dma_read_register(0x28) & (1u << 24));
  EXPECT_EQ(dma_read_register(0x68), 2);
  EXPECT_EQ(dma_read_register(0x74), 0xC0C40000);
  dma_write_register(0x70, priorities(7, 0));
  EXPECT_FALSE(gpu_read_register(4) & (1u << 24));
  EXPECT_FALSE(dma_read_register(0x28) & (1u << 24));
  EXPECT_EQ(dma_read_register(0x74), 0xC4C40000);
  dma_shutdown();
  end_display(&fixture);
}

TEST(DmaPriority, shared_halt_restore_uses_saved_priorities) {
  for (u32 gpu = 0; gpu < 8; gpu++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    dma_initialize();
    queue_collision(true);
    u8              saved[256];
    state_wrapper_t state;
    state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
    EXPECT_TRUE(dma_do_state(&state));
    const size_t size = state_wrapper_get_position(&state);
    const s32    halt = 100;
    const u32    dpcr = priorities(gpu, 3);
    memcpy(saved, &halt, 4);
    memcpy(saved + 95, &dpcr, 4);
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    EXPECT_TRUE(dma_do_state(&state));
    cpu_add_pending_ticks(99);
    timing_events_run_events();
    EXPECT_TRUE(dma_read_register(0x28) & (1u << 24));
    cpu_add_pending_ticks(1);
    timing_events_run_events();
    u32 actual;
    memcpy(&actual, bus_get_unprotected_ram_pointer() + ADDRESS, 4);
    EXPECT_EQ(actual, gpu < 3 ? 0x00FFFFFFu : PIXEL);
    EXPECT_EQ(dma_read_register(0x74), 0xC4C40000);
    dma_shutdown();
    end_display(&fixture);
  }
}

TEST(DmaPriority, coincident_chop_deadlines_use_priority_before_callback_order) {
  for (u32 gpu = 0; gpu < 8; gpu++) {
    display_fixture_t fixture;
    begin_display(&fixture);
    dma_initialize();
    spu_initialize();
    prepare_gpu_read(2);
    const u32 old_word = 0x44445555;
    memcpy(bus_get_unprotected_ram_pointer() + ADDRESS + 4, &old_word, 4);
    spu_write_register(0x1A6, 0x10000 / 8);
    spu_write_register(0x1AA, 0x20);
    dma_write_register(0x20, ADDRESS);
    dma_write_register(0x24, 2);
    dma_write_register(0x28, 0x11700100);
    dma_write_register(0x40, ADDRESS);
    dma_write_register(0x44, 2);
    dma_write_register(0x48, 0x11700101);
    /* Start GPU first, then change priority while both channels are paused. */
    dma_write_register(0x70, 8u << 8 | 15u << 16);
    EXPECT_EQ(dma_read_register(0x24), 1);
    EXPECT_EQ(dma_read_register(0x44), 1);
    dma_write_register(0x70, (gpu | 8) << 8 | 11u << 16);
    u8              saved[256];
    state_wrapper_t state;
    state_wrapper_init_write(&state, saved, sizeof(saved), SAVE_STATE_VERSION);
    EXPECT_TRUE(dma_do_state(&state));
    const size_t size  = state_wrapper_get_position(&state);
    const s32    pause = 128;
    memcpy(saved + 111, &pause, 4);
    memcpy(saved + 119, &pause, 4);
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    EXPECT_TRUE(dma_do_state(&state));
    timing_event_t* gpu_event = NULL;
    timing_event_t* spu_event = NULL;
    for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next) {
      if (event->name_len == 12 && !memcmp(event->name_data, "DMA Chop GPU", 12)) gpu_event = event;
      if (event->name_len == 12 && !memcmp(event->name_data, "DMA Chop SPU", 12)) {
        EXPECT_NOT_NULL(gpu_event);
        spu_event = event;
      }
    }
    EXPECT_NOT_NULL(gpu_event);
    EXPECT_NOT_NULL(spu_event);
    EXPECT_EQ(gpu_event->next_run_time, spu_event->next_run_time);
    cpu_add_pending_ticks(128);
    timing_events_run_events();
    cpu_add_pending_ticks(32);
    timing_events_run_events();
    u32 actual;
    memcpy(&actual, spu_get_ram() + 0x10000, 4);
    EXPECT_EQ(actual, PIXEL);
    memcpy(&actual, spu_get_ram() + 0x10004, 4);
    EXPECT_EQ(actual, gpu < 3 ? PIXEL + 1 : old_word);
    memcpy(&actual, bus_get_unprotected_ram_pointer() + ADDRESS + 4, 4);
    EXPECT_EQ(actual, PIXEL + 1);
    EXPECT_FALSE(dma_read_register(0x28) & (1u << 24));
    EXPECT_FALSE(dma_read_register(0x48) & (1u << 24));
    spu_shutdown();
    dma_shutdown();
    end_display(&fixture);
  }
}
