/* cupid-ps1 saved command-event activity during CD sector interrupts. */
#include "continuation_fixture.h"

static timing_event_t* command_find(const char* name) {
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
    if (event->name_len == strlen(name) && memcmp(event->name_data, name, event->name_len) == 0) return event;
  return NULL;
}
static void command_clear_sector(void) {
  cdrom_write_register(0, 1);
  EXPECT_EQ(cdrom_read_register(3) & 7, 1);
  u32 responses = 0;
  while (cdrom_read_register(0) & 0x20) {
    (void)cdrom_read_register(1);
    EXPECT_TRUE(++responses <= 16);
  }
  EXPECT_TRUE(responses > 0);
  cdrom_write_register(3, 0x1F);
  cdrom_write_register(0, 0);
}
static void command_case(bool double_speed, bool active) {
  xa_start(false, true, false);
  const u8 mode = double_speed ? 0x80 : 0;
  xa_command(0x0E, &mode, 1);
  xa_command(6, NULL, 0);
  xa_until(1);
  timing_event_t* command;
  if (active) {
    command_clear_sector();
    timing_event_t* drive = command_find("CDROM Drive Event");
    EXPECT_NOT_NULL(drive);
    const tick_count_t until = timing_event_get_ticks_until_next_execution(drive);
    EXPECT_TRUE(until > 1);
    cpu_add_pending_ticks(until - 1);
    timing_events_run_events();
    cdrom_write_register(1, 1);
    command = command_find("CDROM Command Event");
    EXPECT_NOT_NULL(command);
    xa_step();
    EXPECT_EQ(cdrom_dbg_requested_lba(), 2);
    EXPECT_TRUE(timing_event_is_active(command));
  } else {
    cdrom_write_register(0, 0);
    cdrom_write_register(1, 1);
    EXPECT_NULL(command_find("CDROM Command Event"));
    command = NULL;
  }
  xa_saved_t* overlap = xa_save();
  u8*         invalid = malloc(overlap->cd_size);
  EXPECT_NOT_NULL(invalid);
  memcpy(invalid, overlap->cd, overlap->cd_size);
  invalid[overlap->cd_size - 1] |= 0x80u;
  state_wrapper_t rejected;
  state_wrapper_init_read(&rejected, invalid, overlap->cd_size, SAVE_STATE_VERSION);
  EXPECT_FALSE(cdrom_do_state(&rejected));
  EXPECT_TRUE(rejected.error);
  free(invalid);
  xa_saved_t* unchanged = xa_save();
  xa_equal(overlap, unchanged);
  free(unchanged);
  if (!active) command_clear_sector();
  xa_ack();
  xa_saved_t* result = xa_save();
  xa_spu_equal(overlap, result);
  xa_restore(overlap);
  EXPECT_EQ(command_find("CDROM Command Event") != NULL, active);
  if (command) EXPECT_TRUE(timing_event_is_active(command));
  xa_saved_t* restored = xa_save();
  xa_equal(overlap, restored);
  free(restored);
  if (!active) command_clear_sector();
  xa_ack();
  xa_saved_t* replay = xa_save();
  xa_equal(result, replay);
  free(replay);
  if (active) {
    /* The acknowledgement queued INT1. Restore that gap after its callback
     * has already fired, so loading must reactivate the saved async event. */
    EXPECT_NOT_NULL(command_find("CDROM Async Interrupt Event"));
    xa_step();
    cdrom_write_register(0, 1);
    EXPECT_EQ(cdrom_read_register(3) & 7, 1);
    cdrom_write_register(0, 0);
    xa_saved_t* delivered = xa_save();
    xa_spu_equal(overlap, delivered);
    xa_restore(result);
    replay = xa_save();
    xa_equal(result, replay);
    free(replay);
    EXPECT_NOT_NULL(command_find("CDROM Async Interrupt Event"));
    xa_step();
    cdrom_write_register(0, 1);
    EXPECT_EQ(cdrom_read_register(3) & 7, 1);
    cdrom_write_register(0, 0);
    replay = xa_save();
    xa_equal(delivered, replay);
    free(replay);
    free(delivered);
  }
  free(result);
  free(overlap);
  xa_end();
}
TEST(CdCommandContinuation, active_sector_overlap_single_speed) {
  command_case(false, true);
}
TEST(CdCommandContinuation, active_sector_overlap_double_speed) {
  command_case(true, true);
}
TEST(CdCommandContinuation, inactive_interrupt_single_speed) {
  command_case(false, false);
}
TEST(CdCommandContinuation, inactive_interrupt_double_speed) {
  command_case(true, false);
}
