#ifndef CUPID_TEST_MDEC_INPUT_FIXTURE_H
#define CUPID_TEST_MDEC_INPUT_FIXTURE_H

/* cupid-ps1 MDEC DMA input keeps complete words at FIFO boundaries. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/mdec/mdec.h"

typedef struct {
  u32  status, head, tail, size;
  bool dma_in, dma_out;
  u16  halfwords[512];
} input_snapshot_t;

static input_snapshot_t read_input_raw(void)
{
  u8              bytes[8192];
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, sizeof(bytes), SAVE_STATE_VERSION);
  EXPECT_TRUE(mdec_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t     size = state_wrapper_get_position(&state);
  input_snapshot_t result;
  state_wrapper_init_read(&state, bytes, size, SAVE_STATE_VERSION);
  state_wrapper_do_u32(&state, &result.status);
  state_wrapper_do_bool(&state, &result.dma_in);
  state_wrapper_do_bool(&state, &result.dma_out);
  state_wrapper_do_u32(&state, &result.head);
  state_wrapper_do_u32(&state, &result.tail);
  state_wrapper_do_u32(&state, &result.size);
  state_wrapper_do_array(&state, result.halfwords, sizeof(u16), 512);
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_TRUE(result.head < 512 && result.tail < 512);
  return result;
}

static void restore_input(void)
{
  u8 cpu[16384], decoder[8192], dmac[4096], events[4096];
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity;
    size_t size;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {mdec_do_state, decoder, sizeof(decoder), 0},
                    {dma_do_state, dmac, sizeof(dmac), 0},
                    {timing_events_do_state, events, sizeof(events), 0}};
  state_wrapper_t state;
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].size = state_wrapper_get_position(&state);
  }
  mdec_write_register(4, 0x80000000);
  for (u32 i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].size, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].size);
  }
}

static input_snapshot_t read_input(void)
{
  input_snapshot_t result = read_input_raw();
  EXPECT_TRUE(result.size <= 512);
  return result;
}

static bool input_matches(const input_snapshot_t* before, const input_snapshot_t* after, const u32* words, u32 accepted)
{
  EXPECT_TRUE(before->size <= 512);
  bool equal = after->head == before->head && after->size == before->size + 2 * accepted &&
               after->tail == (before->tail + 2 * accepted) % 512;
  for (u32 i = 0; i < before->size; i++)
    equal &= after->halfwords[(before->head + i) % 512] == before->halfwords[(before->head + i) % 512];
  for (u32 i = 0; i < 2 * accepted; i++)
    equal &= after->halfwords[(before->tail + i) % 512] == (u16)(words[i / 2] >> (16 * (i & 1)));
  return equal;
}

static void begin_input(bool old, u32 room, cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, const u32* program,
                        size_t count)
{
  start_cpu_with_fastmem(mode, fastmem, 0xA0010000, program, count);
  g_settings.mdec_use_old_routines      = old;
  g_settings.mdec_disable_cdrom_speedup = false;
  dma_initialize();
  mdec_initialize();
  mdec_write_register(4, 0x40000000);
  mdec_write_register(0, 0x2000FFFF);
  if (room & 1) {
    mdec_write_register(0, 0x00000000);
    mdec_write_register(0, 0x55AAFE00);
  } else {
    mdec_write_register(0, 0xFE000000);
  }
  EXPECT_EQ(mdec_dbg_fifo_status() >> 24, 2);
  EXPECT_EQ((mdec_dbg_fifo_status() >> 11) & 0x7FF, room & 1);
  for (u32 i = room & 1; i < 512 - room; i += 2)
    mdec_write_register(0, 0x10001000u + i);
  EXPECT_EQ(read_input().size, 512 - room);
}

static void end_input(void)
{
  mdec_shutdown();
  dma_shutdown();
  finish_cpu();
}

#endif
