/* cupid-ps1 generated inputs and complete saved-record comparisons. */
#ifndef CUPID_TEST_STATE_RECOVERY_FIXTURE_H
#define CUPID_TEST_STATE_RECOVERY_FIXTURE_H
#include "tests/test_harness.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/small_string.h"
#include "core/config/settings.h"
#include "core/cdrom/cdrom.h"
#include "core/cpu/cpu_core.h"
#include "core/memory/bus.h"
#include "core/state/save_state_version.h"
#include "core/system/system.h"
#include "util/state/state_wrapper.h"
#include "util/state/compress_helpers.h"
#include <unistd.h>

static char                 recovery_root[1024];
static Error                recovery_error;
static cpu_execution_mode_t recovery_mode    = CPU_EXECUTION_MODE_INTERPRETER;
static cpu_fastmem_mode_t   recovery_memory  = CPU_FASTMEM_MODE_DISABLED;
static bool                 recovery_linking = true;
const char*                 emu_folders_get_bios(void);
void core_get_string_setting_value(const char* section, const char* key, const char* def, small_string_t* out);
__attribute__((weak)) const char* emu_folders_get_bios(void) {
  return recovery_root;
}
__attribute__((weak)) void core_get_string_setting_value(const char* section, const char* key, const char* def,
                                                         small_string_t* out) {
  (void)section;
  (void)key;
  (void)def;
  small_string_assign_cstr(out, "generated.bin");
}
static void recovery_path(char* out, size_t capacity, const char* name) {
  const int n = snprintf(out, capacity, "%s/%s", recovery_root, name);
  EXPECT_TRUE(n > 0 && (size_t)n < capacity);
}
static void recovery_write(const char* name, const void* data, size_t length) {
  char path[2048];
  recovery_path(path, sizeof(path), name);
  EXPECT_TRUE(fs_write_binary_file(path, data, length, &recovery_error));
}
static u8* recovery_read(const char* name, size_t* length) {
  char path[2048];
  recovery_path(path, sizeof(path), name);
  u8* data = NULL;
  EXPECT_TRUE(fs_read_binary_file_path(path, &data, length, &recovery_error));
  return data;
}
static void recovery_store(u8* p, u32 value) {
  for (u32 i = 0; i < 4; i++)
    p[i] = (u8)(value >> (8 * i));
}
static u32 recovery_word(const u8* p) {
  return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void recovery_ram(u32 value) {
  recovery_store(bus_get_ram_pointer() + 0x10000, value);
}
static void recovery_save(const char* name) {
  char path[2048];
  recovery_path(path, sizeof(path), name);
  EXPECT_TRUE(system_save_state(path, &recovery_error, false, true));
}
static int recovery_load(const char* name) {
  char path[2048];
  recovery_path(path, sizeof(path), name);
  Error_clear(&recovery_error);
  return system_load_state(path, &recovery_error, true, false);
}
static void recovery_insert(const char* name) {
  char path[2048];
  recovery_path(path, sizeof(path), name);
  EXPECT_TRUE(system_insert_media(path));
}
static void recovery_generate_disc(const char* stem, u8 seed) {
  u8* data = calloc(64, 2352);
  EXPECT_NOT_NULL(data);
  for (u32 sector = 0; sector < 64; sector++) {
    u8* bytes = data + sector * 2352;
    memset(bytes + 1, 0xFF, 10);
    bytes[13] = 2;
    bytes[14] = (u8)((sector / 10) * 16 + sector % 10);
    bytes[15] = 2;
    bytes[24] = seed;
  }
  char name[64];
  snprintf(name, sizeof(name), "%s.bin", stem);
  recovery_write(name, data, 64 * 2352);
  free(data);
  char      cue[256];
  const int n = snprintf(cue, sizeof(cue), "FILE \"%s.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n", stem);
  EXPECT_TRUE(n > 0 && (size_t)n < sizeof(cue));
  snprintf(name, sizeof(name), "%s.cue", stem);
  recovery_write(name, cue, (size_t)n);
}
static void recovery_begin_with_ram(bool large_ram) {
  strcpy(recovery_root, "/tmp/cupid-state-recovery-XXXXXX");
  EXPECT_NOT_NULL(mkdtemp(recovery_root));
  Error_init(&recovery_error);
  u8* bios = calloc(1, 512 * 1024);
  EXPECT_NOT_NULL(bios);
  recovery_store(bios, 0x1000FFFF);
  recovery_write("generated.bin", bios, 512 * 1024);
  free(bios);
  recovery_generate_disc("disc-a", 1);
  recovery_generate_disc("disc-b", 2);
  const char playlist[] = "disc-a.cue\ndisc-b.cue\n";
  recovery_write("discs.m3u", playlist, sizeof(playlist) - 1);
  settings_init(&g_settings);
  g_settings.cpu_execution_mode           = recovery_mode;
  g_settings.cpu_fastmem_mode             = recovery_memory;
  g_settings.cpu_recompiler_block_linking = recovery_linking;
  g_settings.cpu_enable_8mb_ram           = large_ram;
  g_settings.gpu_renderer                 = GPU_RENDERER_SOFTWARE;
  g_settings.gpu_use_thread               = false;
  g_settings.gpu_pgxp_enable              = false;
  g_settings.cdrom_readahead_sectors      = 0;
  g_settings.apply_compatibility_settings = false;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    g_settings.memory_card_types[i] = MEMORY_CARD_TYPE_NONE;
  EXPECT_TRUE(system_process_startup(&recovery_error));
  EXPECT_TRUE(system_core_thread_initialize(&recovery_error));
  system_boot_parameters_t boot;
  system_boot_parameters_init(&boot);
  boot.force_software_renderer = true;
  EXPECT_TRUE(system_boot_system(&boot, &recovery_error));
  system_boot_parameters_destroy(&boot);
}
static void recovery_begin(void) {
  recovery_begin_with_ram(false);
}
static void recovery_end(void) {
  system_shutdown_system(false);
  system_core_thread_shutdown();
  system_process_shutdown();
  settings_destroy(&g_settings);
  Error_destroy(&recovery_error);
}
static void recovery_corrupt_cd(const char* input, const char* output) {
  size_t size = 0;
  u8*    data = recovery_read(input, &size);
  EXPECT_TRUE(size >= sizeof(save_state_header_t));
  save_state_header_t header;
  memcpy(&header, data, sizeof(header));
  EXPECT_EQ(header.version, SAVE_STATE_VERSION);
  EXPECT_EQ(header.data_compression_type, 0);
  const u8 marker[] = {5, 0, 0, 0, 'C', 'D', 'R', 'O', 'M'};
  size_t   cd       = 0;
  for (size_t i = header.offset_to_data; i + sizeof(marker) <= size; i++)
    if (memcmp(data + i, marker, sizeof(marker)) == 0) {
      EXPECT_EQ(cd, 0);
      cd = i + sizeof(marker);
    }
  EXPECT_TRUE(cd > 0);
  /* Empty queues and sector buffers have a fixed suffix, with 80 bytes
   * before the activity mask. Verify that shape before corrupting it. */
  size_t   end    = cd;
  const u8 next[] = {3, 0, 0, 0, 'P', 'a', 'd'};
  while (end + sizeof(next) <= size && memcmp(data + end, next, sizeof(next)) != 0)
    end++;
  EXPECT_TRUE(end + sizeof(next) <= size && end >= cd + 81);
  for (size_t i = end - 81; i < end - 1; i += 4)
    EXPECT_EQ(recovery_word(data + i), 0);
  recovery_store(data + end - 81, 8);
  recovery_write(output, data, size);
  free(data);
}
static void recovery_reject_unchanged(const char* input) {
  recovery_save("before.sav");
  const cd_image_t* image  = cdrom_get_media();
  const u32         index  = cdrom_has_media() ? cdrom_get_current_sub_image() : 0;
  char*             path   = strdup(system_get_game_path());
  char*             title  = strdup(system_get_game_title());
  char*             serial = strdup(system_get_game_serial());
  EXPECT_NOT_NULL(path);
  EXPECT_NOT_NULL(title);
  EXPECT_NOT_NULL(serial);
  const game_hash_t hash = system_get_game_hash();
  EXPECT_EQ(recovery_load(input), 0);
  EXPECT_TRUE(system_is_valid());
  EXPECT_EQ(cdrom_get_media(), image);
  EXPECT_EQ(cdrom_has_media() ? cdrom_get_current_sub_image() : 0, index);
  EXPECT_EQ(strcmp(path, system_get_game_path()), 0);
  EXPECT_EQ(strcmp(title, system_get_game_title()), 0);
  EXPECT_EQ(strcmp(serial, system_get_game_serial()), 0);
  EXPECT_EQ(system_get_game_hash(), hash);
  free(path);
  free(title);
  free(serial);
  Error_clear(&recovery_error);
  recovery_save("after.sav");
  size_t a_size = 0, b_size = 0;
  u8*    a = recovery_read("before.sav", &a_size);
  u8*    b = recovery_read("after.sav", &b_size);
  EXPECT_EQ(a_size, b_size);
  EXPECT_EQ(memcmp(a, b, a_size), 0);
  free(a);
  free(b);
}
#endif
