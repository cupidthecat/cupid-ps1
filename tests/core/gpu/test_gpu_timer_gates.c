/* cupid-ps1 counter gating through the GPU event queue. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/timers/timers.h"

static timing_event_t* gate_crtc_event(void)
{
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next) {
    if (event->name_len == 13 && memcmp(event->name_data, "GPU CRTC Tick", 13) == 0) return event;
  }
  EXPECT_TRUE(false);
  return NULL;
}

static void advance_beam(u32 tick, u32 line)
{
  const tick_count_t duration = gpu_get_system_ticks_until_ticks_and_line(tick, line);
  EXPECT_TRUE(duration > 0);
  cpu_add_pending_ticks(duration);
  timing_events_run_events();
}

static void restore_gated_clock(void)
{
  u8           cpu[16384], irq[16], counters[128], timing[4096];
  const size_t gpu_capacity = 2 * 1024 * 1024;
  u8*          gpu          = malloc(gpu_capacity);
  EXPECT_NOT_NULL(gpu);
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {interrupt_controller_do_state, irq, sizeof(irq), 0},
                    {gpu_do_state, gpu, gpu_capacity, 0},
                    {timers_do_state, counters, sizeof(counters), 0},
                    {timing_events_do_state, timing, sizeof(timing), 0}};
  state_wrapper_t state;
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }
  advance_beam(2000, 0);
  timers_write_register(0, 0x1234);
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  EXPECT_EQ(timers_read_register(0), 0);
  free(gpu);
}

static void check_hblank_pause(bool dotclock, bool restore)
{
  const u32 resolutions[] = {0, 1, 2, 3, 0x40};
  u32       failures = 0, observations = 0;
  for (u32 pal = 0; pal < 2; pal++) {
    for (u32 res = 0; res < 5; res++) {
      const u32 words[] = {0x1000FFFF, 0};
      start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
      interrupt_controller_reset();
      timers_initialize();
      gpu_backend_t* backend = gpu_sw_create();
      EXPECT_NOT_NULL(backend);
      gpu_initialize();
      gpu_reset(false);
      gpu_write_register(4, 0x08000000 | resolutions[res] | (pal ? 8 : 0));
      /* Keep the first two lines inside the vertical display window; this
       * fixture drives events without entering the system execution loop. */
      gpu_write_register(4, 0x07000000 | ((pal ? 288u : 240u) << 10));
      gpu_crtc_tick_event(0);
      timers_write_register(8, 0xFFFF);
      timers_write_register(4, (dotclock ? 0x100 : 0) | 1);
      const tick_count_t scheduled      = timing_event_get_ticks_until_next_execution(gate_crtc_event());
      const tick_count_t first_boundary = gpu_get_system_ticks_until_ticks_and_line(488, 0);
      advance_beam(100, 0);
      const u32 blank = timers_read_register(0);
      if (restore) restore_gated_clock();
      advance_beam(1000, 0);
      const u32 active_a = timers_read_register(0);
      advance_beam(2000, 0);
      const u32 active_b = timers_read_register(0);
      advance_beam((pal ? GPU_PAL_HORIZONTAL_ACTIVE_END : GPU_NTSC_HORIZONTAL_ACTIVE_END) + 8, 0);
      const u32 late = timers_read_register(0);
      cpu_add_pending_ticks(10);
      timing_events_run_events();
      const u32 after_blank = timers_read_register(0);
      advance_beam(1000, 1);
      const u32 next = timers_read_register(0);
      printf("gpu-timer-gate dotclock=%u restore=%u pal=%u resolution=%u scheduled=%d boundary=%d blank=%u active_a=%u "
             "active_b=%u late=%u after_blank=%u next=%u\n",
             dotclock, restore, pal, resolutions[res], scheduled, first_boundary, blank, active_a, active_b, late,
             after_blank, next);
      failures += scheduled <= 0 || scheduled > first_boundary || blank != 0 || active_a == 0 || active_b <= active_a ||
                  late <= active_b || after_blank != late || next <= late;
      observations++;
      gpu_shutdown();
      gpu_backend_destroy(backend);
      timers_shutdown();
      finish_cpu();
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 10);
  EXPECT_EQ(failures, 0);
}

TEST(GpuTimerGate, hblank_system_clock)
{
  check_hblank_pause(false, false);
}
TEST(GpuTimerGate, hblank_dot_clock)
{
  check_hblank_pause(true, false);
}
TEST(GpuTimerGate, restored_system_clock)
{
  check_hblank_pause(false, true);
}
TEST(GpuTimerGate, restored_dot_clock)
{
  check_hblank_pause(true, true);
}
