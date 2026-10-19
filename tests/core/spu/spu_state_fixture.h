/* cupid-ps1 transfer execution snapshots, including the shared clock. */
#ifndef CUPID_TESTS_SPU_STATE_FIXTURE_H
#define CUPID_TESTS_SPU_STATE_FIXTURE_H

#include "tests/core/spu/spu_clock_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"

typedef struct {
  bool (*do_state)(state_wrapper_t*);
  u8* bytes;
  size_t capacity, length;
} spu_component_record_t;

typedef struct {
  spu_component_record_t records[6];
  u32 version;
} spu_execution_snapshot_t;

static spu_execution_snapshot_t save_spu_execution(u32 version)
{
  /* The temporary stop can interrupt event processing after CPU overshoot.
   * Commit those elapsed ticks, as the system does before resuming execution. */
  timing_events_commit_leftover_ticks();
  spu_execution_snapshot_t result = {
      .records = {{cpu_do_state, NULL, 16384, 0}, {bus_do_state, NULL, BUS_RAM_8MB_SIZE + 4096, 0},
                  {dma_do_state, NULL, 4096, 0}, {interrupt_controller_do_state, NULL, 256, 0},
                  {spu_do_state, NULL, SPU_RAM_SIZE + 65536, 0}, {timing_events_do_state, NULL, 4096, 0}},
      .version = version};
  for (u32 i = 0; i < 6; i++) {
    spu_component_record_t* record = &result.records[i];
    record->bytes = calloc(1, record->capacity);
    EXPECT_NOT_NULL(record->bytes);
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, record->bytes, record->capacity, version);
    EXPECT_TRUE(record->do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    record->length = state_wrapper_get_position(&sw);
  }
  return result;
}

static void load_spu_execution(const spu_execution_snapshot_t* snapshot)
{
  cpu_code_cache_reset();
  spu_reset();
  dma_reset();
  interrupt_controller_reset();
  for (u32 i = 0; i < 6; i++) {
    const spu_component_record_t* record = &snapshot->records[i];
    state_wrapper_t sw;
    state_wrapper_init_read(&sw, record->bytes, record->length, snapshot->version);
    EXPECT_TRUE(record->do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), record->length);
  }
  spu_post_load_state(snapshot->version);
  cpu_code_cache_reset();
}

static void compare_spu_execution(const spu_execution_snapshot_t* snapshot)
{
  timing_events_commit_leftover_ticks();
  for (u32 i = 0; i < 6; i++) {
    const spu_component_record_t* record = &snapshot->records[i];
    u8* bytes = calloc(1, record->capacity);
    EXPECT_NOT_NULL(bytes);
    state_wrapper_t sw;
    state_wrapper_init_write(&sw, bytes, record->capacity, snapshot->version);
    EXPECT_TRUE(record->do_state(&sw));
    EXPECT_FALSE(state_wrapper_has_error(&sw));
    EXPECT_EQ(state_wrapper_get_position(&sw), record->length);
    const int difference = memcmp(bytes, record->bytes, record->length);
    printf("spu-state component=%u bytes=%zu equal=%u\n", i, record->length, difference == 0);
    if (difference) {
      u32 printed = 0;
      for (size_t byte = 0; byte < record->length && printed < 16; byte++) {
        if (bytes[byte] != record->bytes[byte]) {
          printf("spu-state-difference component=%u offset=%zu actual=%02X saved=%02X\n",
                 i, byte, bytes[byte], record->bytes[byte]);
          printed++;
        }
      }
    }
    fflush(stdout);
    EXPECT_EQ(difference, 0);
    free(bytes);
  }
}

static void free_spu_execution(spu_execution_snapshot_t* snapshot)
{
  for (u32 i = 0; i < 6; i++) free(snapshot->records[i].bytes);
}

static void remove_legacy_transfer_event(spu_execution_snapshot_t* snapshot)
{
  EXPECT_TRUE(snapshot->version < 90 && snapshot->version >= 71);
  spu_component_record_t* record = &snapshot->records[5];
  u32 count;
  memcpy(&count, record->bytes + 16, sizeof(count));
  size_t offset = 20;
  bool removed = false;
  for (u32 i = 0; i < count; i++) {
    u32 name_length;
    memcpy(&name_length, record->bytes + offset, sizeof(name_length));
    const size_t size = 4 + name_length + 24;
    EXPECT_TRUE(offset + size <= record->length);
    if (name_length == 12 && memcmp(record->bytes + offset + 4, "SPU Transfer", 12) == 0) {
      memmove(record->bytes + offset, record->bytes + offset + size, record->length - offset - size);
      record->length -= size;
      count--;
      memcpy(record->bytes + 16, &count, sizeof(count));
      removed = true;
      break;
    }
    offset += size;
  }
  EXPECT_TRUE(removed);
}

#endif
