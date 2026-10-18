/* cupid-ps1 counter completion and GPU scheduling. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/timers/timers.h"

#include <limits.h>

static timing_event_t* find_crtc_event(void)
{
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next) {
    if (event->name_len == 13 && memcmp(event->name_data, "GPU CRTC Tick", 13) == 0) return event;
  }
  EXPECT_TRUE(false);
  return NULL;
}

static void check_completed_counter(bool toggle, bool restore)
{
  const u32 resolutions[]  = {0, 1, 2, 3, 0x40};
  const u32 dot_dividers[] = {10, 8, 5, 4, 7};
  u32       failures = 0, observations = 0;
  for (u32 timer = 0; timer < 2; timer++) {
    for (u32 res = 0; res < 5; res++) {
      const u32 words[] = {0x1000FFFF, 0};
      start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
      interrupt_controller_reset();
      timers_initialize();
      gpu_backend_t* backend = gpu_sw_create();
      EXPECT_NOT_NULL(backend);
      gpu_initialize();
      gpu_reset(false);
      gpu_write_register(4, 0x08000000 | resolutions[res]);
      gpu_write_register(4, 0x07000000 | (240u << 10));
      timers_write_register(timer * 16 + 8, 2);
      timers_write_register(timer * 16 + 4, 0x110 | (toggle ? 0x80 : 0));
      timers_add_ticks(timer, 2);
      EXPECT_TRUE(irq_dbg_status_register() & (1u << (INTERRUPT_CONTROLLER_IRQ_TMR0 + timer)));
      EXPECT_EQ(timers_get_ticks_until_irq(timer), INT32_MAX);
      if (restore) {
        u8              bytes[128];
        state_wrapper_t state;
        state_wrapper_init_write(&state, bytes, sizeof(bytes), SAVE_STATE_VERSION);
        EXPECT_TRUE(timers_do_state(&state));
        EXPECT_TRUE(interrupt_controller_do_state(&state));
        const size_t size = state_wrapper_get_position(&state);
        timers_write_register(timer * 16 + 4, 0x150);
        EXPECT_EQ(timers_get_ticks_until_irq(timer), 2);
        state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
        EXPECT_TRUE(timers_do_state(&state));
        EXPECT_TRUE(interrupt_controller_do_state(&state));
        EXPECT_EQ(state_wrapper_get_position(&state), size);
        EXPECT_EQ(timers_get_ticks_until_irq(timer), INT32_MAX);
      }
      gpu_crtc_tick_event(0);
      const tick_count_t deadline = timing_event_get_ticks_until_next_execution(find_crtc_event());
      timers_add_ticks(timer, 1);
      EXPECT_EQ(timers_dbg_counter(timer), 3);
      timers_write_register(timer * 16 + 4, 0x150);
      EXPECT_EQ(timers_get_ticks_until_irq(timer), 2);
      gpu_crtc_tick_event(0);
      const tick_count_t rearmed          = timing_event_get_ticks_until_next_execution(find_crtc_event());
      const tick_count_t expected_rearmed = gpu_get_system_ticks_until_ticks_and_line(
          timer ? GPU_NTSC_HORIZONTAL_ACTIVE_END : 2 * dot_dividers[res], timer ? 1 : 0);
      const u32 bit = 1u << (INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
      interrupt_controller_write_register(0, ~bit);
      EXPECT_TRUE(expected_rearmed > 1);
      cpu_add_pending_ticks(expected_rearmed - 1);
      timing_events_run_events();
      const bool early_irq = (irq_dbg_status_register() & bit) != 0;
      cpu_add_pending_ticks(1);
      timing_events_run_events();
      const bool due_irq = (irq_dbg_status_register() & bit) != 0;
      printf("gpu-timer-completed timer=%u resolution=%u toggle=%u restore=%u deadline=%d rearmed=%d "
             "expected_rearmed=%d early_irq=%u due_irq=%u\n",
             timer, resolutions[res], toggle, restore, deadline, rearmed, expected_rearmed, early_irq, due_irq);
      failures +=
          deadline <= 0 || rearmed <= 0 || rearmed >= deadline || rearmed != expected_rearmed || early_irq || !due_irq;
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

TEST(GpuTimerDeadline, completed_pulse)
{
  check_completed_counter(false, false);
}
TEST(GpuTimerDeadline, completed_toggle)
{
  check_completed_counter(true, false);
}
TEST(GpuTimerDeadline, restored_pulse)
{
  check_completed_counter(false, true);
}
TEST(GpuTimerDeadline, restored_toggle)
{
  check_completed_counter(true, true);
}
