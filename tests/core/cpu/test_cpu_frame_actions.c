/* cupid-ps1 frame actions wait for instructions and callbacks to complete. */
#include "tests/core/memory/partial_store_fixture.h"
#include "core/mdec/mdec.h"
#include "core/system/system.h"

static bool frame_body_completed;
static u32  frame_store_value;

static void frame_action_write(u32 address, u32 value) {
  (void)address;
  frame_store_value = value;
  system_frame_done();
  EXPECT_TRUE(system_has_pending_frame_actions());
  frame_body_completed = true;
}

static void frame_action_event(void* user, tick_count_t ticks, tick_count_t late) {
  (void)ticks;
  (void)late;
  timing_event_deactivate((timing_event_t*)user);
  system_frame_done();
  EXPECT_TRUE(system_has_pending_frame_actions());
  frame_body_completed = true;
}

static void check_frame_actions(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking) {
  for (u32 callback = 0; callback < 2; callback++) {
    const u32 pc      = 0xA0010000;
    const u32 words[] = {0xAD4B0400, 0x8D490000, callback ? 0u : 0xAD090000, 0x26100001, 0x1000FFFF, 0};
    start_cpu_with_fastmem(mode, fastmem, pc, words, countof(words));
    EXPECT_TRUE(system_is_shutdown());
    g_settings.cpu_recompiler_block_linking = linking;
    g_cpu_state.regs.r[8]                   = 0x1F801100;
    g_cpu_state.regs.r[9]                   = 0xAABB0C01;
    g_cpu_state.regs.r[10]                  = 0xA0030000;
    g_cpu_state.regs.r[11]                  = 0x1357;
    const u32 next                          = 0xFEDC2202;
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &next, sizeof(next));
    spu_initialize();
    mdec_initialize();
    bus_memory_write_handler_t* handlers =
        bus_offset_write_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
    const u32                        page     = 0x1F801100 >> BUS_MEMORY_LUT_PAGE_SHIFT;
    const bus_memory_write_handler_t original = handlers[page];
    handlers[page]                            = frame_action_write;
    frame_body_completed                      = false;
    frame_store_value                         = 0;
    timing_event_t event;
    if (callback) {
      timing_event_init(&event, "frame-action-test", 17, 30, 30, frame_action_event, &event);
      timing_event_activate(&event);
    }
    run_slice(1000);
    if (callback) {
      timing_event_deactivate(&event);
      timing_event_destroy(&event);
    }
    handlers[page] = original;
    EXPECT_TRUE(frame_body_completed);
    EXPECT_FALSE(system_has_pending_frame_actions());
    EXPECT_FALSE(timing_events_is_running_events());
    EXPECT_TRUE(timing_events_get_global_tick_counter() < 100);
    EXPECT_EQ(g_cpu_state.regs.r[9], next);
    if (!callback) EXPECT_EQ(frame_store_value, 0xAABB0C01);
    expect_device_bus_path(mode, fastmem, cpu_execution_stats_get());
    mdec_shutdown();
    spu_shutdown();
    finish_cpu();
  }
}
PARTIAL_DEVICE_MODES(CpuFrameActions, check_frame_actions)
