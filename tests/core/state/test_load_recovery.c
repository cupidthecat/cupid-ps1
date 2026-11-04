/* cupid-ps1 rejected state loads preserve the active machine and media. */
#include "recovery_fixture.h"

static void recovery_simple(bool media) {
  recovery_begin();
  if (media) recovery_insert("disc-a.cue");
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  recovery_ram(0x11223344);
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_ram(0x11223344);
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x11223344);
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_end();
}
TEST(StateLoadRecovery, late_error_without_media) {
  recovery_simple(false);
}
TEST(StateLoadRecovery, late_error_same_media) {
  recovery_simple(true);
}

TEST(StateLoadRecovery, late_error_disc_switch) {
  recovery_begin();
  recovery_insert("disc-b.cue");
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  system_remove_media();
  recovery_insert("disc-a.cue");
  recovery_ram(0x11223344);
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  EXPECT_TRUE(strstr(cdrom_get_media_path(), "disc-b.cue") != NULL);
  recovery_end();
}
TEST(StateLoadRecovery, late_error_disc_removal) {
  recovery_begin();
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  recovery_insert("disc-a.cue");
  recovery_ram(0x11223344);
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_FALSE(cdrom_has_media());
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_end();
}
TEST(StateLoadRecovery, late_error_disc_insertion) {
  recovery_begin();
  recovery_ram(0x11223344);
  recovery_save("no-disc.sav");
  recovery_insert("disc-b.cue");
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  EXPECT_EQ(recovery_load("no-disc.sav"), 1);
  EXPECT_FALSE(cdrom_has_media());
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_TRUE(cdrom_has_media());
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_end();
}
static void recovery_bad_media(bool subimage) {
  recovery_begin();
  recovery_insert("disc-b.cue");
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  size_t              size = 0;
  u8*                 data = recovery_read("valid.sav", &size);
  save_state_header_t header;
  EXPECT_TRUE(size >= sizeof(header));
  memcpy(&header, data, sizeof(header));
  if (subimage) {
    header.media_subimage_index = 1;
    memcpy(data, &header, sizeof(header));
  } else {
    EXPECT_TRUE(header.media_path_length > 0 && header.offset_to_media_path + header.media_path_length <= size);
    char* name = (char*)data + header.offset_to_media_path;
    char* tail = strstr(name, "disc-b.cue");
    EXPECT_NOT_NULL(tail);
    tail[5] = 'z';
  }
  recovery_write("bad.sav", data, size);
  free(data);
  system_remove_media();
  recovery_insert("disc-a.cue");
  recovery_ram(0x11223344);
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_end();
}
TEST(StateLoadRecovery, missing_referenced_media) {
  recovery_bad_media(false);
}
TEST(StateLoadRecovery, invalid_single_image_index) {
  recovery_bad_media(true);
}
TEST(StateLoadRecovery, rejected_media_sidecar_preserves_machine) {
  recovery_begin();
  recovery_insert("disc-b.cue");
  recovery_ram(0x55667788);
  recovery_save("foreign.sav");
  system_remove_media();
  recovery_insert("disc-a.cue");
  recovery_ram(0x11223344);
  /* Opening the image succeeds; insertion rejects its changed sidecar
   * after the previous image has been detached. */
  const u8 bad_sidecar[] = {'B', 'A', 'D', 0};
  recovery_write("disc-b.sbi", bad_sidecar, sizeof(bad_sidecar));
  recovery_reject_unchanged("foreign.sav");
  EXPECT_EQ(recovery_load("foreign.sav"), 0);
  EXPECT_TRUE(strstr(Error_get_description(&recovery_error), "Invalid header in 'disc-b.sbi'") != NULL);
  recovery_end();
}
TEST(StateLoadRecovery, playlist_index_and_rejected_switch) {
  recovery_begin();
  recovery_insert("discs.m3u");
  recovery_ram(0x11223344);
  recovery_save("first.sav");
  size_t              size = 0;
  u8*                 data = recovery_read("first.sav", &size);
  save_state_header_t header;
  EXPECT_TRUE(size >= sizeof(header));
  memcpy(&header, data, sizeof(header));
  header.media_subimage_index = 1;
  memcpy(data, &header, sizeof(header));
  recovery_write("second.sav", data, size);
  free(data);
  EXPECT_EQ(recovery_load("second.sav"), 1);
  EXPECT_EQ(cdrom_get_current_sub_image(), 1);
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  EXPECT_EQ(recovery_load("first.sav"), 1);
  EXPECT_EQ(cdrom_get_current_sub_image(), 0);
  recovery_reject_unchanged("bad.sav");
  EXPECT_EQ(recovery_load("valid.sav"), 1);
  EXPECT_EQ(cdrom_get_current_sub_image(), 1);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_end();
}

TEST(StateLoadRecovery, recovery_failure_stops_machine) {
  recovery_begin();
  recovery_insert("disc-b.cue");
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  system_remove_media();
  const u8 good_sidecar[] = {'S', 'B', 'I', 0};
  recovery_write("disc-a.sbi", good_sidecar, sizeof(good_sidecar));
  recovery_insert("disc-a.cue");
  /* The live disc was accepted with valid sidecar data. A later resource
   * failure makes reinsertion fail while recovering from the corrupt state. */
  const u8 bad_sidecar[] = {'B', 'A', 'D', 0};
  recovery_write("disc-a.sbi", bad_sidecar, sizeof(bad_sidecar));
  EXPECT_EQ(recovery_load("bad.sav"), 0);
  EXPECT_TRUE(strstr(Error_get_description(&recovery_error), "Recovery also failed:") != NULL);
  EXPECT_TRUE(system_is_shutdown());
  EXPECT_FALSE(cdrom_has_media());
  recovery_end();
}

TEST(StateLoadRecovery, ram_size_and_taint_recovery) {
  recovery_begin_with_ram(true);
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_8MB_SIZE);
  recovery_ram(0x55667788);
  recovery_save("large.sav");
  size_t large_size = 0;
  u8*    large      = recovery_read("large.sav", &large_size);
  recovery_end();
  recovery_begin();
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_2MB_SIZE);
  recovery_write("large.sav", large, large_size);
  free(large);
  recovery_corrupt_cd("large.sav", "bad-large.sav");
  recovery_ram(0x11223344);
  recovery_save("small.sav");
  recovery_corrupt_cd("small.sav", "bad-small.sav");
  recovery_reject_unchanged("bad-large.sav");
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_2MB_SIZE);
  EXPECT_EQ(recovery_load("large.sav"), 1);
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_8MB_SIZE);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
  recovery_reject_unchanged("bad-small.sav");
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_8MB_SIZE);
  EXPECT_EQ(recovery_load("small.sav"), 1);
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_2MB_SIZE);
  EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x11223344);
  recovery_end();
}

static void recovery_compressed(bool inner) {
  recovery_begin();
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  recovery_corrupt_cd("valid.sav", "bad.sav");
  const compress_type_t outer_codecs[] = {COMPRESS_TYPE_GZIP, COMPRESS_TYPE_ZSTANDARD, COMPRESS_TYPE_XZ};
  const compress_type_t inner_codecs[] = {COMPRESS_TYPE_DEFLATE, COMPRESS_TYPE_ZSTANDARD, COMPRESS_TYPE_XZ};
  const char*           extensions[]   = {"gz", "zst", "xz"};
  for (u32 codec = 0; codec < (inner ? 2 : 3); codec++) {
    char valid[64], bad[64];
    if (inner) {
      snprintf(valid, sizeof(valid), "inner-valid-%u.sav", codec);
      snprintf(bad, sizeof(bad), "inner-bad-%u.sav", codec);
    } else {
      snprintf(valid, sizeof(valid), "valid-%u.sav.%s", codec, extensions[codec]);
      snprintf(bad, sizeof(bad), "bad-%u.sav.%s", codec, extensions[codec]);
    }
    for (u32 corrupt = 0; corrupt < 2; corrupt++) {
      size_t              size = 0;
      u8*                 data = recovery_read(corrupt ? "bad.sav" : "valid.sav", &size);
      save_state_header_t header;
      EXPECT_TRUE(size >= sizeof(header));
      memcpy(&header, data, sizeof(header));
      const size_t offset = inner ? header.offset_to_data : 0;
      EXPECT_TRUE(offset < size);
      u8*    encoded      = NULL;
      size_t encoded_size = 0;
      EXPECT_TRUE(compress_helpers_compress_alloc(inner ? inner_codecs[codec] : outer_codecs[codec], data + offset,
                                                  size - offset, -1, &encoded, &encoded_size, &recovery_error));
      if (inner) {
        u8* file = malloc(offset + encoded_size);
        EXPECT_NOT_NULL(file);
        memcpy(file, data, offset);
        header.data_compression_type = (u32)inner_codecs[codec];
        header.data_compressed_size  = (u32)encoded_size;
        memcpy(file, &header, sizeof(header));
        memcpy(file + offset, encoded, encoded_size);
        recovery_write(corrupt ? bad : valid, file, offset + encoded_size);
        free(file);
      } else {
        recovery_write(corrupt ? bad : valid, encoded, encoded_size);
      }
      free(encoded);
      free(data);
    }
    EXPECT_EQ(recovery_load(valid), 1);
    EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x55667788);
    recovery_ram(0x11223344);
    recovery_reject_unchanged(bad);
    EXPECT_EQ(recovery_word(bus_get_ram_pointer() + 0x10000), 0x11223344);
    EXPECT_EQ(recovery_load(valid), 1);
  }
  recovery_end();
}
TEST(StateLoadRecovery, outer_compressed_inputs) {
  recovery_compressed(false);
}

static void recovery_reject_unsupported_inner_xz(void) {
  recovery_begin();
  recovery_ram(0x55667788);
  recovery_save("valid.sav");
  size_t              size = 0, encoded_size = 0;
  u8*                 data = recovery_read("valid.sav", &size);
  save_state_header_t header;
  memcpy(&header, data, sizeof(header));
  const size_t offset  = header.offset_to_data;
  u8*          encoded = NULL;
  EXPECT_TRUE(compress_helpers_compress_alloc(COMPRESS_TYPE_XZ, data + offset, size - offset, -1, &encoded,
                                              &encoded_size, &recovery_error));
  u8* file = malloc(offset + encoded_size);
  EXPECT_NOT_NULL(file);
  memcpy(file, data, offset);
  header.data_compression_type = SAVE_STATE_COMPRESSION_TYPE_XZ;
  header.data_compressed_size  = (u32)encoded_size;
  memcpy(file, &header, sizeof(header));
  memcpy(file + offset, encoded, encoded_size);
  recovery_write("unsupported.sav", file, offset + encoded_size);
  free(file);
  free(encoded);
  free(data);
  recovery_ram(0x11223344);
  recovery_reject_unchanged("unsupported.sav");
  recovery_end();
}
TEST(StateLoadRecovery, inner_compressed_inputs) {
  recovery_compressed(true);
  recovery_reject_unsupported_inner_xz();
}

typedef struct {
  cd_image_t*              image;
  const cd_image_vtable_t* original;
  cd_image_vtable_t        vtable;
  u32                      destroyed;
} recovery_owned_image_t;
static recovery_owned_image_t recovery_images[2];
static void                   recovery_image_destroy(cd_image_t* image) {
  recovery_owned_image_t* tracked = NULL;
  for (u32 i = 0; i < 2; i++)
    if (recovery_images[i].image == image) tracked = &recovery_images[i];
  EXPECT_NOT_NULL(tracked);
  EXPECT_EQ(tracked->destroyed, 0);
  tracked->destroyed++;
  image->vtbl = tracked->original;
  if (tracked->original->destroy) tracked->original->destroy(image);
}
TEST(StateLoadRecovery, replacing_media_closes_previous_image) {
  recovery_begin();
  for (u32 i = 0; i < 2; i++) {
    char path[2048];
    recovery_path(path, sizeof(path), i ? "disc-b.cue" : "disc-a.cue");
    recovery_owned_image_t* tracked = &recovery_images[i];
    tracked->image                  = cd_image_open(path, &recovery_error);
    EXPECT_NOT_NULL(tracked->image);
    tracked->original = tracked->image->vtbl;
    EXPECT_NOT_NULL(tracked->original);
    tracked->vtable         = *tracked->original;
    tracked->vtable.destroy = recovery_image_destroy;
    tracked->image->vtbl    = &tracked->vtable;
    EXPECT_TRUE(cdrom_insert_media(tracked->image, DISC_REGION_NON_PS1, "", 0, "", 0, "", 0, &recovery_error));
  }
  EXPECT_EQ(recovery_images[0].destroyed, 1);
  EXPECT_EQ(recovery_images[1].destroyed, 0);
  recovery_end();
  EXPECT_EQ(recovery_images[0].destroyed, 1);
  EXPECT_EQ(recovery_images[1].destroyed, 1);
}

#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/timers/timers.h"
#include "core/spu/spu.h"
#include "core/gpu/gpu.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"

static void active_compare(const char* expected, const char* actual) {
  size_t a_size, b_size;
  u8*    a = recovery_read(expected, &a_size);
  u8*    b = recovery_read(actual, &b_size);
  EXPECT_EQ(a_size, b_size);
  if (memcmp(a, b, a_size)) {
    for (size_t i = 0, count = 0; i < a_size && count < 12; i++) {
      if (a[i] == b[i]) continue;
      printf("active-state difference offset=%zu expected=%02X actual=%02X\n", i, a[i], b[i]);
      count++;
    }
  }
  EXPECT_EQ(memcmp(a, b, a_size), 0);
  free(a);
  free(b);
}

static void active_corrupt_tail(void) {
  size_t   size;
  u8*      data     = recovery_read("foreign.sav", &size);
  const u8 marker[] = {7, 0, 0, 0, 'C', 'h', 'e', 'e', 'v', 'o', 's'};
  u32      found    = 0;
  for (size_t i = sizeof(save_state_header_t); i + sizeof(marker) <= size; i++) {
    if (memcmp(data + i, marker, sizeof(marker))) continue;
    data[i + 4] = 'X';
    found++;
  }
  EXPECT_EQ(found, 1);
  recovery_write("bad-tail.sav", data, size);
  free(data);
}

static timing_event_t* active_event(const char* name) {
  for (timing_event_t* event = *timing_events_get_head_event_ptr(); event; event = event->next)
    if (event->name_len == strlen(name) && !memcmp(event->name_data, name, event->name_len)) return event;
  return NULL;
}

static void active_audio(void) {
  u8* ram     = spu_get_writable_ram();
  ram[0x1000] = 0x24;
  ram[0x1001] = 7;
  for (u32 i = 2; i < 16; i++)
    ram[0x1000 + i] = (u8)(i * 17);
  for (u32 voice = 1; voice <= 3; voice += 2) {
    spu_write_register(voice * 16, 0x3FFF);
    spu_write_register(voice * 16 + 2, 0x3FFF);
    spu_write_register(voice * 16 + 4, voice == 1 ? 0x900 : 0x1700);
    spu_write_register(voice * 16 + 6, 0x200);
    spu_write_register(voice * 16 + 8, 15);
  }
  const u16 registers[][2] = {{0x180, 0xC050}, {0x182, 0x3FFF}, {0x194, 8},      {0x198, 10},    {0x1A2, 0x8000 / 8},
                              {0x184, 0x2000}, {0x186, 0x2000}, {0x1C4, 0x4000}, {0x1D4, 0x100}, {0x1D6, 0x200},
                              {0x1FC, 0x4000}, {0x1FE, 0x4000}, {0x1AA, 0xFF80}, {0x188, 10}};
  for (u32 i = 0; i < countof(registers); i++)
    spu_write_register(registers[i][0], registers[i][1]);
  cpu_add_pending_ticks(113 * 768 + 255);
  timing_events_run_events();
  spu_generate_pending_samples();
}

static void active_run(void) {
  run_slice(50000);
  timing_events_commit_leftover_ticks();
  spu_generate_pending_samples();
  EXPECT_TRUE(g_cpu_state.regs.r[23] > 4);
  EXPECT_FALSE(dma_read_register(0x28) & (1u << 24));
  EXPECT_TRUE(cdrom_read_register(0) & 0x20);
  EXPECT_TRUE(spu_read_register(0x1AE) & 0x40);
  u16 transferred;
  memcpy(&transferred, spu_get_ram() + 0x10000, sizeof(transferred));
  EXPECT_EQ(transferred, 0x5678);
}

static void active_case(cpu_execution_mode_t mode, cpu_fastmem_mode_t memory, bool linking) {
  recovery_mode    = mode;
  recovery_memory  = memory;
  recovery_linking = linking;
  recovery_begin();
  recovery_insert("disc-b.cue");
  recovery_ram(0x55667788);
  recovery_save("foreign.sav");
  recovery_corrupt_cd("foreign.sav", "bad-cd.sav");
  active_corrupt_tail();
  system_remove_media();
  recovery_insert("disc-a.cue");
  cdrom_reset();
  active_audio();
  const u32 words[]   = {0x3C08A003, 0x8D100020, 0,          0x8E090000, 0x01205021, 0x25290001, 0xAE090000, 0x256B0001,
                         0x156C0004, 0,          0x340D0401, 0x408D6000, 0,          0x1000FFF5, 0};
  const u32 handler[] = {0x401A7000, 0, 0x3C1B1F80, 0xAF601070, 0x26F70001, 0x03400008, 0x42000010};
  memcpy(bus_get_unprotected_ram_pointer() + 0x10000, words, sizeof(words));
  memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
  const u32 address = 0xA0030000, value = 0x13579BDF;
  memcpy(bus_get_unprotected_ram_pointer() + 0x30020, &address, sizeof(address));
  memcpy(bus_get_unprotected_ram_pointer() + 0x30000, &value, sizeof(value));
  g_cpu_state.cache_control.bits = 0x1E988;
  g_cpu_state.cop0_regs.sr.bits  = 0;
  g_cpu_state.regs.r[12]         = UINT32_MAX;
  cpu_set_pc(0xA0010000);
  cpu_execution_stats_initialize(true);
  run_slice(5000);
  g_cpu_state.regs.r[12] = g_cpu_state.regs.r[11] + 128;
  run_to_pc(0xA0010010);
  timing_events_commit_leftover_ticks();
  EXPECT_EQ(g_cpu_state.load_delay_reg, 9);
  EXPECT_TRUE(g_cpu_state.regs.r[11] > 1);

  spu_write_register(0x1A6, 0x10000 / 8);
  spu_write_register(0x1A4, 0x10008 / 8);
  spu_write_register(0x1AA, 0xFFE0);
  u32 data[16];
  for (u32 i = 0; i < countof(data); i++)
    data[i] = 0x12345678 ^ (i * 0x1020304);
  spu_dma_write(data, countof(data));
  gpu_write_register(4, 0x04000002);
  for (u32 i = 0; i < 12; i++)
    memcpy(bus_get_unprotected_ram_pointer() + 0x50000 + i * 4, &(u32){0x1F000000}, 4);
  dma_write_register(0x20, 0x50000);
  dma_write_register(0x24, 12);
  dma_write_register(0x70, 8 << 8);
  dma_write_register(0x28, 0x11700101);
  timers_write_register(0x28, 2047);
  timers_write_register(0x24, 0x58);
  interrupt_controller_write_register(4, (1u << 2) | (1u << 6) | (1u << 9));
  cdrom_write_register(0, 1);
  cdrom_write_register(2, 0x1F);
  cdrom_write_register(0, 0);
  cdrom_write_register(1, 1);
  EXPECT_NOT_NULL(active_event("SPU Transfer"));
  EXPECT_NOT_NULL(active_event("DMA Chop GPU"));
  EXPECT_NOT_NULL(active_event("CDROM Command Event"));
  EXPECT_EQ(g_cpu_state.load_delay_reg, 9);
  recovery_save("resume.sav");
  active_run();
  recovery_save("expected.sav");
  EXPECT_EQ(recovery_load("resume.sav"), 1);
  recovery_save("valid-loaded.sav");
  active_compare("resume.sav", "valid-loaded.sav");
  active_run();
  recovery_save("valid-result.sav");
  active_compare("expected.sav", "valid-result.sav");
  for (u32 bad = 0; bad < 2; bad++) {
    EXPECT_EQ(recovery_load("resume.sav"), 1);
    recovery_reject_unchanged(bad ? "bad-tail.sav" : "bad-cd.sav");
    active_run();
    recovery_save("actual.sav");
    active_compare("expected.sav", "actual.sav");
  }
  const cpu_execution_stats_t stats = *cpu_execution_stats_get();
  printf("active recovery mode=%u memory=%u linking=%u interpreted=%llu native=%llu loads=%llu stores=%llu\n", mode,
         memory, linking, (unsigned long long)stats.interpreted_instructions,
         (unsigned long long)stats.native_blocks_entered, (unsigned long long)stats.native_lut_loads,
         (unsigned long long)stats.native_lut_stores);
  if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
    EXPECT_TRUE(stats.native_blocks_entered > 100);
    EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
    if (memory == CPU_FASTMEM_MODE_LUT) {
      EXPECT_TRUE(stats.native_lut_loads > 0);
      EXPECT_TRUE(stats.native_lut_stores > 0);
    }
  } else
    EXPECT_TRUE(stats.interpreted_instructions > 100);
  recovery_end();
}

#define ACTIVE_RECOVERY(name, mode, memory, linking) \
  TEST(StateRecoveryActive, name) {                  \
    active_case(mode, memory, linking);              \
  }
ACTIVE_RECOVERY(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
ACTIVE_RECOVERY(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
ACTIVE_RECOVERY(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
ACTIVE_RECOVERY(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
ACTIVE_RECOVERY(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
ACTIVE_RECOVERY(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
ACTIVE_RECOVERY(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
ACTIVE_RECOVERY(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
