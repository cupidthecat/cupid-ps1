/* cupid-ps1 GPU reset requests and interrupt-controller edge latches. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu_backend.h"
#include "core/gpu/software/gpu_sw.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/timers/timers.h"

static gpu_backend_t* begin_gpu_irq(void)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
  interrupt_controller_reset();
  timers_initialize();
  gpu_backend_t* backend = gpu_sw_create();
  EXPECT_NOT_NULL(backend);
  gpu_initialize();
  gpu_reset(false);
  return backend;
}

static void end_gpu_irq(gpu_backend_t* backend)
{
  gpu_shutdown();
  gpu_backend_destroy(backend);
  timers_shutdown();
  finish_cpu();
}

static bool gpu_irq_flag(void)
{
  return (gpu_read_register(4) & (1u << 24)) != 0;
}
static bool gpu_irq_latched(void)
{
  return (irq_dbg_status_register() & (1u << INTERRUPT_CONTROLLER_IRQ_GPU)) != 0;
}
static void acknowledge_cpu_gpu_irq(void)
{
  interrupt_controller_write_register(0, ~(1u << INTERRUPT_CONTROLLER_IRQ_GPU));
}

static void request_gpu_irq(bool partial_command)
{
  gpu_write_register(0, 0x1F000000);
  EXPECT_TRUE(gpu_irq_flag());
  EXPECT_TRUE(gpu_irq_latched());
  if (partial_command) gpu_write_register(0, 0x20000000);
}

static void restore_pending_gpu_irq(void)
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
  gpu_write_register(4, 0x02000000);
  gpu_write_register(4, 0x01000000);
  acknowledge_cpu_gpu_irq();
  EXPECT_FALSE(gpu_irq_flag());
  EXPECT_FALSE(gpu_irq_latched());
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  EXPECT_TRUE(gpu_irq_flag());
  EXPECT_TRUE(gpu_irq_latched());
  free(gpu);
}

static void check_gpu_reset_irq(bool hard)
{
  u32 failures = 0, observations = 0;
  for (u32 variant = 0; variant < (hard ? 2u : 4u); variant++) {
    for (u32 partial = 0; partial < 2; partial++) {
      for (u32 restore = 0; restore < 2; restore++) {
        gpu_backend_t* backend = begin_gpu_irq();
        request_gpu_irq(partial);
        if (restore) restore_pending_gpu_irq();
        if (hard)
          gpu_reset(variant != 0);
        else
          gpu_write_register(4, variant << 30);
        const bool request = gpu_irq_flag(), latched = gpu_irq_latched();
        acknowledge_cpu_gpu_irq();
        EXPECT_FALSE(gpu_irq_latched());
        gpu_write_register(0, 0x1F000000);
        const bool next_request = gpu_irq_flag(), next_latched = gpu_irq_latched();
        printf("gpu-reset-irq hard=%u variant=%u partial=%u restore=%u request=%u expected_request=0 latched=%u "
               "expected_latched=1 next_request=%u next_latched=%u expected_next=1\n",
               hard, variant, partial, restore, request, latched, next_request, next_latched);
        failures += request || !latched || !next_request || !next_latched;
        observations++;
        end_gpu_irq(backend);
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, hard ? 8 : 16);
  EXPECT_EQ(failures, 0);
}

TEST(GpuResetIrq, soft_reset)
{
  check_gpu_reset_irq(false);
}
TEST(GpuResetIrq, hard_reset)
{
  check_gpu_reset_irq(true);
}

TEST(GpuResetIrq, acknowledge_and_fifo_controls)
{
  u32 failures = 0, observations = 0;
  for (u32 fifo = 0; fifo < 2; fifo++) {
    for (u32 partial = 0; partial < 2; partial++) {
      for (u32 restore = 0; restore < 2; restore++) {
        gpu_backend_t* backend = begin_gpu_irq();
        request_gpu_irq(partial);
        if (restore) restore_pending_gpu_irq();
        gpu_write_register(4, fifo ? 0x01000000 : 0x02000000);
        const bool request = gpu_irq_flag(), latched = gpu_irq_latched();
        failures += request != (fifo != 0) || !latched;
        acknowledge_cpu_gpu_irq();
        if (fifo) {
          gpu_write_register(0, 0x1F000000);
          failures += gpu_irq_latched();
          gpu_write_register(4, 0x02000000);
        } else {
          gpu_write_register(4, 0x01000000);
        }
        gpu_write_register(0, 0x1F000000);
        const bool next = gpu_irq_latched();
        failures += !gpu_irq_flag() || !next;
        printf("gpu-reset-control fifo=%u partial=%u restore=%u request=%u expected_request=%u latched=%u "
               "next_latched=%u\n",
               fifo, partial, restore, request, fifo, latched, next);
        observations++;
        end_gpu_irq(backend);
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 8);
  EXPECT_EQ(failures, 0);
}
