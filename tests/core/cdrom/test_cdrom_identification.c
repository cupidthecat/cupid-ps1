/* cupid-ps1 generated-disc identification command regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/spu/spu.h"
#include "core/system/system.h"
#include "core/interrupts/interrupt_controller.h"

static bool id_read_sector(cd_image_t* self, void* buffer, const cd_image_index_t* index, cd_image_lba_t lba)
{
  (void)self; (void)index; (void)lba;
  memset(buffer, 0, CD_IMAGE_RAW_SECTOR_SIZE);
  return true;
}

static cd_image_t* id_disc(cd_image_track_mode_t mode)
{
  static const cd_image_vtable_t vtable = {.read_sector_from_index = id_read_sector};
  cd_image_t* image = calloc(1, sizeof(*image));
  EXPECT_NOT_NULL(image);
  cd_image_init(image, &vtable);
  image->lba_count = 32;
  cd_image_track_t* track = cd_image_push_track(image);
  *track = (cd_image_track_t){.track_number = 1, .length = 32, .mode = mode,
                            .control = {.bits = mode == CD_IMAGE_TRACK_MODE_AUDIO ? 1 : 0x41}};
  cd_image_index_t* index = cd_image_push_index(image);
  *index = (cd_image_index_t){.file_sector_size = CD_IMAGE_RAW_SECTOR_SIZE, .track_number = 1,
                            .index_number = 1, .length = 32, .mode = mode, .control = track->control};
  EXPECT_TRUE(cd_image_seek_lba(image, 0));
  return image;
}

static void check_id_type(cd_image_track_mode_t mode, u8 expected_type, bool restore)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  g_settings.cdrom_readahead_sectors = 0;
  spu_initialize();
  system_update_overclock();
  cdrom_initialize();
  Error error;
  Error_init(&error);
  cd_image_t* image = id_disc(mode);
  if (restore) {
    image->toc_disc_type = expected_type;
    image->has_toc_disc_type = true;
  }
  EXPECT_TRUE(cdrom_insert_media(image, DISC_REGION_NON_PS1, NULL, 0, NULL, 0, NULL, 0, &error));
  cpu_add_pending_ticks(33868800);
  timing_events_run_events();
  /* Acknowledge the insertion latch through Getstat before querying ID. */
  cdrom_write_register(0, 0);
  cdrom_write_register(1, 0x01);
  run_slice(30000);
  cdrom_write_register(0, 1);
  EXPECT_EQ(cdrom_read_register(3) & 7, 3);
  EXPECT_EQ(cdrom_read_register(1), 0x12);
  cdrom_write_register(3, 0x1F);
  cdrom_write_register(0, 0);
  cdrom_write_register(1, 0x1A);
  run_slice(30000);
  cdrom_write_register(0, 1);
  EXPECT_EQ(cdrom_read_register(3) & 7, 3);
  EXPECT_EQ(cdrom_read_register(1), 2);
  cdrom_write_register(3, 0x1F);
  u8* saved_cd = malloc(512 * 1024);
  EXPECT_NOT_NULL(saved_cd);
  u8 saved_cpu[16384], saved_timing[4096], saved_irq[128];
  size_t cd_size = 0, cpu_size = 0, timing_size = 0, irq_size = 0;
  state_wrapper_t state;
  if (restore) {
    state_wrapper_init_write(&state, saved_cd, 512 * 1024, SAVE_STATE_VERSION);
    EXPECT_TRUE(cdrom_do_state(&state));
    cd_size = state_wrapper_get_position(&state);
    state_wrapper_init_write(&state, saved_cpu, sizeof(saved_cpu), SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&state));
    cpu_size = state_wrapper_get_position(&state);
    state_wrapper_init_write(&state, saved_irq, sizeof(saved_irq), SAVE_STATE_VERSION);
    EXPECT_TRUE(interrupt_controller_do_state(&state));
    irq_size = state_wrapper_get_position(&state);
    state_wrapper_init_write(&state, saved_timing, sizeof(saved_timing), SAVE_STATE_VERSION);
    EXPECT_TRUE(timing_events_do_state(&state));
    timing_size = state_wrapper_get_position(&state);
  }
  run_slice(100000);
  EXPECT_EQ(cdrom_read_register(3) & 7, 5);
  u8 response[8];
  for (u32 i = 0; i < sizeof(response); i++) response[i] = cdrom_read_register(1);
  if (restore) {
    state_wrapper_init_read(&state, saved_cpu, cpu_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(cpu_do_state(&state));
    state_wrapper_init_read(&state, saved_irq, irq_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(interrupt_controller_do_state(&state));
    state_wrapper_init_read(&state, saved_cd, cd_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(cdrom_do_state(&state));
    state_wrapper_init_read(&state, saved_timing, timing_size, SAVE_STATE_VERSION);
    EXPECT_TRUE(timing_events_do_state(&state));
    run_slice(100000);
    EXPECT_EQ(cdrom_read_register(3) & 7, 5);
    for (u32 i = 0; i < sizeof(response); i++) EXPECT_EQ(cdrom_read_register(1), response[i]);
    cdrom_reset();
    EXPECT_EQ(cd_image_get_disc_type(cdrom_get_media()), expected_type);
  }
  free(saved_cd);
  printf("cd-id-type mode=%u type=%02X expected=%02X status=%02X flags=%02X\n", mode, response[2], expected_type, response[0], response[1]);
  fflush(stdout);
  cdrom_shutdown();
  spu_shutdown();
  finish_cpu();
  Error_destroy(&error);
  EXPECT_EQ(response[0], 0x0A);
  EXPECT_EQ(response[1], mode == CD_IMAGE_TRACK_MODE_AUDIO ? 0x90 : 0x80);
  EXPECT_EQ(response[2], expected_type);
  for (u32 i = 3; i < sizeof(response); i++) EXPECT_EQ(response[i], 0);
}

TEST(CdIdentification, audio) { check_id_type(CD_IMAGE_TRACK_MODE_AUDIO, 0, false); }
TEST(CdIdentification, mode1) { check_id_type(CD_IMAGE_TRACK_MODE_MODE1_RAW, 0, false); }
TEST(CdIdentification, mode2_control) { check_id_type(CD_IMAGE_TRACK_MODE_MODE2_RAW, 0x20, false); }
TEST(CdIdentification, toc_override_pending_response_restore)
{
  check_id_type(CD_IMAGE_TRACK_MODE_MODE2_RAW, 0x10, true);
}
