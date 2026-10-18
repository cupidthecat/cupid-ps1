#ifndef CUPID_TEST_MDEC_DECODE_FIXTURE_H
#define CUPID_TEST_MDEC_DECODE_FIXTURE_H

/* cupid-ps1 decoder state and command-stream test helpers. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/mdec/mdec.h"

typedef struct {
  u32  status, remaining, block, coefficient;
  u16  qscale;
  u8   decoder;
  bool pending;
  s16  blocks[6][64];
  u32  rgb[256];
} coefficient_snapshot_t;

static coefficient_snapshot_t read_coefficients(void)
{
  u8              bytes[8192];
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, sizeof(bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t size = state_wrapper_get_position(&state);
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  coefficient_snapshot_t result;
  bool                   dma_in, dma_out;
  state_wrapper_do_u32(&state, &result.status);
  state_wrapper_do_bool(&state, &dma_in);
  state_wrapper_do_bool(&state, &dma_out);
  state_wrapper_skip_bytes(&state, 3 * sizeof(u32) + 512 * sizeof(u16));
  state_wrapper_skip_bytes(&state, 3 * sizeof(u32) + 192 * sizeof(u32));
  state_wrapper_do_u8(&state, &result.decoder);
  state_wrapper_do_u32(&state, &result.remaining);
  state_wrapper_skip_bytes(&state, 128 * sizeof(u8) + 64 * sizeof(s16));
  state_wrapper_do_array(&state, result.blocks, sizeof(s16), 6 * 64);
  state_wrapper_do_u32(&state, &result.block);
  state_wrapper_do_u32(&state, &result.coefficient);
  state_wrapper_do_u16(&state, &result.qscale);
  state_wrapper_do_array(&state, result.rgb, sizeof(u32), 256);
  state_wrapper_do_bool(&state, &result.pending);
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), size);
  return result;
}

static void restore_coefficients(void)
{
  u8 cpu[16384], decoder[8192], dmac[4096], events[4096], after[8192];
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {mdec_do_state, decoder, sizeof(decoder), 0},
                    {dma_do_state, dmac, sizeof(dmac), 0},
                    {timing_events_do_state, events, sizeof(events), 0}};
  state_wrapper_t      state;
  const global_ticks_t before_ticks = timing_events_get_global_tick_counter();
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }
  mdec_reset();
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
  EXPECT_EQ(timing_events_get_global_tick_counter(), before_ticks);
  state_wrapper_init_write(&state, after, sizeof(after), SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), components[1].size);
  EXPECT_EQ(memcmp(decoder, after, components[1].size), 0);
}

static timing_event_t* find_mdec_copy_event(void)
{
  timing_event_t* event = *timing_events_get_head_event_ptr();
  while (event && strcmp(event->name_data, "MDEC Block Copy Out") != 0)
    event = event->next;
  EXPECT_NOT_NULL(event);
  return event;
}

static void begin_decode(bool old)
{
  const u32 program[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, program, 2);
  g_settings.mdec_use_old_routines      = old;
  g_settings.mdec_disable_cdrom_speedup = false;
  dma_initialize();
  mdec_initialize();
}

static void end_decode(void)
{
  mdec_shutdown();
  dma_shutdown();
  finish_cpu();
}

#endif
