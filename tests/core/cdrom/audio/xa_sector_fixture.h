/* cupid-ps1 generated XA sectors through the CD register and reader APIs. */
#ifndef CUPID_TEST_XA_SECTOR_FIXTURE_H
#define CUPID_TEST_XA_SECTOR_FIXTURE_H
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/spu/spu.h"
#include "core/system/system.h"
#include "core/interrupts/interrupt_controller.h"

typedef struct {
  bool   eight, stereo, half;
  u32    stride;
  size_t audio_offset;
} xa_fixture_t;
static xa_fixture_t xa_fixture;
static u8           xa_bcd(u32 value) {
  return (u8)((value / 10) * 16 + value % 10);
}
static void xa_word(u8* bytes, u32 value) {
  for (u32 i = 0; i < 4; i++)
    bytes[i] = (u8)(value >> (8 * i));
}
static u32 xa_read_word(const u8* bytes) {
  return (u32)bytes[0] | (u32)bytes[1] << 8 | (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}
static bool xa_sector(cd_image_t* self, void* buffer, const cd_image_index_t* index, cd_image_lba_t lba) {
  (void)self;
  (void)index;
  u8* bytes = buffer;
  memset(bytes, 0, 2352);
  const u32 absolute = lba + 150;
  bytes[12]          = xa_bcd(absolute / 4500);
  bytes[13]          = xa_bcd((absolute / 75) % 60);
  bytes[14]          = xa_bcd(absolute % 75);
  bytes[15]          = 2;
  bytes[16]          = 1;
  bytes[17]          = (lba % xa_fixture.stride) == 0 ? 0 : 1;
  bytes[18]          = 0x64;
  bytes[19]          = (xa_fixture.eight ? 0x10 : 0) | (xa_fixture.stereo ? 1 : 0) | (xa_fixture.half ? 4 : 0);
  memcpy(bytes + 20, bytes + 16, 4);
  const u32 units = xa_fixture.eight ? 4 : 8, frames_per_group = 28 * units / (xa_fixture.stereo ? 2 : 1);
  const u32 packet = lba / xa_fixture.stride;
  for (u32 group = 0; group < 18; group++) {
    u8* chunk = bytes + 24 + 128 * group;
    for (u32 unit = 0; unit < units; unit++) {
      const u32 slot      = (unit & 3) | ((unit & 4) << 1);
      const u8  parameter = (u8)((((group + unit / (xa_fixture.stereo ? 2 : 1)) & 3) << 4) | 2);
      chunk[slot]         = parameter;
      chunk[slot | 4]     = parameter;
    }
    for (u32 sample = 0; sample < 28; sample++) {
      u32 word = 0;
      for (u32 unit = 0; unit < units; unit++) {
        const u32 channel = xa_fixture.stereo ? (unit & 1) : 0;
        const u32 frame   = packet * 18 * frames_per_group + group * frames_per_group +
                          (unit / (xa_fixture.stereo ? 2 : 1)) * 28 + sample;
        const u32 value = xa_fixture.eight ? (frame * 17 + channel * 61 + frame / 7) & 255u
                                           : (frame * 5 + channel * 7 + frame / 11) & 15u;
        word |= value << (unit * (xa_fixture.eight ? 8 : 4));
      }
      xa_word(chunk + 16 + sample * 4, word);
    }
  }
  return true;
}
static cd_image_t* xa_disc(void) {
  static const cd_image_vtable_t vt    = {.read_sector_from_index = xa_sector};
  cd_image_t*                    image = calloc(1, sizeof(*image));
  EXPECT_NOT_NULL(image);
  cd_image_init(image, &vt);
  image->lba_count        = 128;
  cd_image_track_t* track = cd_image_push_track(image);
  *track                  = (cd_image_track_t){
                       .track_number = 1, .length = 128, .mode = CD_IMAGE_TRACK_MODE_MODE2_RAW, .control = {.bits = 0x41}};
  cd_image_index_t* index = cd_image_push_index(image);
  *index                  = (cd_image_index_t){.file_sector_size = 2352,
                                               .track_number     = 1,
                                               .index_number     = 1,
                                               .length           = 128,
                                               .mode             = CD_IMAGE_TRACK_MODE_MODE2_RAW,
                                               .control          = track->control};
  EXPECT_TRUE(cd_image_seek_lba(image, 0));
  return image;
}
static void xa_step(void) {
  timing_event_t* event = *timing_events_get_head_event_ptr();
  EXPECT_NOT_NULL(event);
  const tick_count_t ticks = timing_event_get_ticks_until_next_execution(event);
  cpu_add_pending_ticks(ticks > 0 ? ticks : 1);
  timing_events_run_events();
}
static void xa_ack(void) {
  u32 flags = 0;
  for (u32 tries = 0; tries < 128; tries++) {
    cdrom_write_register(0, 1);
    flags = cdrom_read_register(3) & 7;
    if (flags) break;
    xa_step();
  }
  EXPECT_EQ(flags, 3);
  u32 response_size = 0;
  while (cdrom_read_register(0) & 0x20) {
    (void)cdrom_read_register(1);
    EXPECT_TRUE(++response_size <= 16);
  }
  EXPECT_TRUE(response_size > 0);
  cdrom_write_register(3, 0x1F);
  cdrom_write_register(0, 0);
}
static void xa_command(u8 command, const u8* params, u32 count) {
  cdrom_write_register(0, 0);
  for (u32 i = 0; i < count; i++)
    cdrom_write_register(2, params[i]);
  cdrom_write_register(1, command);
  xa_ack();
}
static void xa_until(u32 requested) {
  for (u32 tries = 0; tries < 128 && cdrom_dbg_requested_lba() < requested; tries++)
    xa_step();
  EXPECT_EQ(cdrom_dbg_requested_lba(), requested);
}
static void xa_start(bool eight, bool stereo, bool half) {
  xa_fixture       = (xa_fixture_t){.eight  = eight,
                                    .stereo = stereo,
                                    .half   = half,
                                    .stride = (eight ? 2u : 4u) * (stereo ? 1u : 2u) * (half ? 2u : 1u)};
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  g_settings.cdrom_readahead_sectors = 0;
  g_settings.cdrom_region_check      = false;
  spu_initialize();
  system_update_overclock();
  cdrom_initialize();
  /* The component test consumes CD frames explicitly. No SPU samples elapse,
   * and every serialized SPU byte is checked for invariance across the run. */
  timing_event_t* consumer = NULL;
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
    if (event->name_len == 10 && memcmp(event->name_data, "SPU Sample", 10) == 0) consumer = event;
  EXPECT_NOT_NULL(consumer);
  timing_event_deactivate(consumer);
  Error error = ERROR_INIT;
  EXPECT_TRUE(cdrom_insert_media(xa_disc(), DISC_REGION_NON_PS1, NULL, 0, NULL, 0, NULL, 0, &error));
  Error_destroy(&error);
  cpu_add_pending_ticks(33868800);
  timing_events_run_events();
  xa_command(1, NULL, 0);
  const u8 mode = 0x40;
  xa_command(0x0E, &mode, 1);
}
static void xa_end(void) {
  cdrom_shutdown();
  spu_shutdown();
  finish_cpu();
}
#endif
