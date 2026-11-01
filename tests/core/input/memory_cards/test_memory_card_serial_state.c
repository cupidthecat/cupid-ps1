/* Restore a card transaction after the live SIO0 transport has moved on. */
#include "tests/test_harness.h"
#include "core/cpu/cpu_core.h"
#include "core/input/pad.h"
#include "core/input/memory_cards/memory_card.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/state/save_state_version.h"
#include "util/state/state_wrapper.h"

typedef struct {
  u8* pad;
  size_t pad_size;
  u8 scheduler[4096];
  size_t scheduler_size;
} serial_snapshot_t;

static void advance(tick_count_t ticks)
{
  cpu_add_pending_ticks(ticks);
  timing_events_run_events();
}

static void begin(void)
{
  memset(&g_cpu_state, 0, sizeof(g_cpu_state));
  timing_events_initialize(); interrupt_controller_reset(); pad_initialize();
  pad_set_memory_card(0, memory_card_create(0));
  EXPECT_NOT_NULL(pad_get_memory_card(0));
  pad_write_register(0x0E, 1);
  pad_write_register(0x0A, 0x1003);
  pad_write_register(0, 0x81);
  advance(8);
  EXPECT_EQ(pad_read_register(0) & 255, 0xFF);
  advance(170);
  EXPECT_TRUE(pad_read_register(4) & 0x200);
  pad_write_register(0x0A, 0x1013);
}

static serial_snapshot_t save(void)
{
  serial_snapshot_t result;
  result.pad = malloc(256 * 1024); EXPECT_NOT_NULL(result.pad);
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, result.pad, 256 * 1024, SAVE_STATE_VERSION);
  EXPECT_TRUE(pad_do_state(&sw, false)); result.pad_size = state_wrapper_get_position(&sw);
  state_wrapper_init_write(&sw, result.scheduler, sizeof(result.scheduler), SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&sw)); result.scheduler_size = state_wrapper_get_position(&sw);
  return result;
}

static void load(const serial_snapshot_t* snapshot)
{
  g_cpu_state.pending_ticks = 0;
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, snapshot->pad, snapshot->pad_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(pad_do_state(&sw, false));
  state_wrapper_init_read(&sw, snapshot->scheduler, snapshot->scheduler_size, SAVE_STATE_VERSION);
  EXPECT_TRUE(timing_events_do_state(&sw));
}

static void receive_id1(void)
{
  advance(8);
  EXPECT_EQ(pad_read_register(0) & 255, 0x5A);
  advance(170);
  EXPECT_TRUE(pad_read_register(4) & 0x200);
}

static void finish(serial_snapshot_t* snapshot)
{
  free(snapshot->pad);
  pad_shutdown(); timing_events_shutdown();
}

TEST(MemoryCardSerialState, in_flight_byte_and_queued_byte_survive_rollback)
{
  begin();
  pad_write_register(0, 0x52); /* Already latched into the shift register. */
  pad_write_register(0, 0);    /* A distinct following byte is queued. */
  serial_snapshot_t snapshot = save();
  advance(8); EXPECT_EQ(pad_read_register(0) & 255, 8);
  advance(170); receive_id1();
  pad_write_register(0x0A, 0); /* Live device selection and shift value change. */
  load(&snapshot);
  advance(8); EXPECT_EQ(pad_read_register(0) & 255, 8);
  advance(170); receive_id1();
  finish(&snapshot);
}

TEST(MemoryCardSerialState, queued_byte_survives_restore_during_ack_delay)
{
  begin(); pad_write_register(0, 0x52);
  advance(8); EXPECT_EQ(pad_read_register(0) & 255, 8);
  pad_write_register(0, 0);
  serial_snapshot_t snapshot = save();
  advance(170); receive_id1();
  pad_write_register(0x0A, 0);
  load(&snapshot);
  advance(170); receive_id1();
  finish(&snapshot);
}
