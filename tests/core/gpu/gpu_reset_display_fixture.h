#ifndef CUPID_TEST_GPU_RESET_DISPLAY_FIXTURE_H
#define CUPID_TEST_GPU_RESET_DISPLAY_FIXTURE_H

/* cupid-ps1 GPU reset scanout address and command controls. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/timers/timers.h"

typedef struct {
  gpu_backend_t*              backend;
  const gpu_backend_vtable_t* original;
  gpu_backend_vtable_t        capture;
} display_fixture_t;

static gpu_backend_update_display_cmd_t captured_display;
static u32                              display_updates;

static void capture_display(gpu_backend_t* backend, const gpu_backend_update_display_cmd_t* command)
{
  (void)backend;
  captured_display = *command;
  display_updates++;
}

static gpu_backend_update_display_cmd_t read_display(void)
{
  const u32 before = display_updates;
  gpu_update_display(false);
  EXPECT_EQ(display_updates, before + 1);
  return captured_display;
}

static bool display_equal(const gpu_backend_update_display_cmd_t* a, const gpu_backend_update_display_cmd_t* b)
{
  return a->display_width == b->display_width && a->display_height == b->display_height &&
         a->display_origin_left == b->display_origin_left && a->display_origin_top == b->display_origin_top &&
         a->display_vram_left == b->display_vram_left && a->display_vram_top == b->display_vram_top &&
         a->display_vram_width == b->display_vram_width && a->display_vram_height == b->display_vram_height &&
         a->display_pixel_aspect_ratio == b->display_pixel_aspect_ratio && a->gpu_busy_pct == b->gpu_busy_pct &&
         a->interlaced_display_enabled == b->interlaced_display_enabled &&
         a->interlaced_display_field == b->interlaced_display_field &&
         a->interlaced_display_interleaved == b->interlaced_display_interleaved &&
         a->interleaved_480i_mode == b->interleaved_480i_mode && a->display_24bit == b->display_24bit &&
         a->display_disabled == b->display_disabled && a->submit_frame == b->submit_frame;
}

static void begin_display_cpu(display_fixture_t* fixture, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem,
                              const u32* words, size_t count)
{
  start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, count);
  interrupt_controller_reset();
  timers_initialize();
  fixture->backend = gpu_sw_create();
  EXPECT_NOT_NULL(fixture->backend);
  fixture->original               = fixture->backend->vtable;
  fixture->capture                = *fixture->original;
  fixture->capture.update_display = capture_display;
  fixture->backend->vtable        = &fixture->capture;
  gpu_initialize();
  gpu_reset(false);
}

static void begin_display(display_fixture_t* fixture)
{
  const u32 words[] = {0x1000FFFF, 0};
  begin_display_cpu(fixture, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, words, 2);
}

static void end_display(display_fixture_t* fixture)
{
  gpu_shutdown();
  fixture->backend->vtable = fixture->original;
  gpu_backend_destroy(fixture->backend);
  timers_shutdown();
  finish_cpu();
}

static void restore_display(void)
{
  u8           cpu[16384], irq[16], counters[128], timing[4096];
  const size_t capacity = 2 * 1024 * 1024;
  u8*          gpu      = malloc(capacity);
  EXPECT_NOT_NULL(gpu);
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {interrupt_controller_do_state, irq, sizeof(irq), 0},
                    {gpu_do_state, gpu, capacity, 0},
                    {timers_do_state, counters, sizeof(counters), 0},
                    {timing_events_do_state, timing, sizeof(timing), 0}};
  state_wrapper_t                        state;
  const gpu_backend_update_display_cmd_t before = read_display();
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }
  gpu_write_register(4, 0x05012340);
  gpu_write_register(4, 0x08000001);
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  const gpu_backend_update_display_cmd_t after = read_display();
  EXPECT_TRUE(display_equal(&before, &after));
  free(gpu);
}

#endif
