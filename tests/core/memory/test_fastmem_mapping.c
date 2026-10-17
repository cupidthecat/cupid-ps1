/* cupid-ps1 RAM aliases and native code-page protection checks. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "common/memmap.h"

static u32 mapping_permissions(const void* address)
{
  FILE* maps = fopen("/proc/self/maps", "r");
  EXPECT_NOT_NULL(maps);
  const uintptr_t    target = (uintptr_t)address;
  char               line[512], permissions[5];
  unsigned long long begin, end;
  bool               found  = false;
  u32                result = 0;
  while (fgets(line, sizeof(line), maps)) {
    if (sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) == 3 && target >= begin && target < end) {
      result = (permissions[0] == 'r' ? 1u : 0u) | (permissions[1] == 'w' ? 2u : 0u);
      found  = true;
      break;
    }
  }
  fclose(maps);
  EXPECT_TRUE(found);
  return result;
}

TEST(FastmemMapping, code_pages_protect_every_mmap_alias)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_MMAP, 0xA0010000, words, 2);
  const u32 offset = 0x10000;
  const u32 page   = bus_get_ram_code_page_index(offset);
  EXPECT_FALSE(bus_can_use_fastmem_for_address(0x20010000));
  EXPECT_FALSE(bus_can_use_fastmem_for_address(0xC0010000));
  bus_set_ram_code_page(page);
  EXPECT_EQ(mapping_permissions(bus_get_ram_pointer() + offset), 1);
  for (u32 remap = 0; remap < 2; remap++) {
    u8* arena = bus_get_fastmem_base(false);
    EXPECT_NOT_NULL(arena);
    const u32 segments[] = {0, 0x80000000, 0xA0000000};
    for (u32 segment = 0; segment < 3; segment++) {
      for (u32 mirror = 0; mirror < 4; mirror++) {
        const u32 address = segments[segment] + mirror * BUS_RAM_2MB_SIZE + offset;
        EXPECT_EQ(mapping_permissions(arena + address), 1);
      }
    }
    if (remap == 0) bus_remap_fastmem_views();
  }
  const u32 window = 0x1F801060;
  cpu_get_memory_write_handler(window, MEMORY_ACCESS_SIZE_WORD)(window, 0x880);
  u8*       window_arena      = bus_get_fastmem_base(false);
  const u32 window_segments[] = {0, 0x80000000, 0xA0000000};
  for (u32 segment = 0; segment < 3; segment++) {
    EXPECT_EQ(mapping_permissions(window_arena + window_segments[segment] + offset), 1);
    for (u32 mirror = 1; mirror < 4; mirror++) {
      const u32 address = window_segments[segment] + mirror * BUS_RAM_2MB_SIZE + offset;
      EXPECT_EQ(mapping_permissions(window_arena + address), 0);
      EXPECT_FALSE(bus_can_use_fastmem_for_address(address));
    }
  }
  cpu_get_memory_write_handler(window, MEMORY_ACCESS_SIZE_WORD)(window, 0xB88);
  for (u32 segment = 0; segment < 3; segment++)
    for (u32 mirror = 0; mirror < 4; mirror++)
      EXPECT_EQ(mapping_permissions(window_arena + window_segments[segment] + mirror * BUS_RAM_2MB_SIZE + offset), 1);
  bus_clear_ram_code_page_flags();
  EXPECT_EQ(mapping_permissions(bus_get_ram_pointer() + offset), 3);
  u8*       arena      = bus_get_fastmem_base(false);
  const u32 segments[] = {0, 0x80000000, 0xA0000000};
  for (u32 segment = 0; segment < 3; segment++)
    for (u32 mirror = 0; mirror < 4; mirror++)
      EXPECT_EQ(mapping_permissions(arena + segments[segment] + mirror * BUS_RAM_2MB_SIZE + offset), 3);
  bus_set_ram_code_page(page);
  bus_reset();
  EXPECT_FALSE(bus_is_ram_code_page(page));
  EXPECT_EQ(mapping_permissions(bus_get_ram_pointer() + offset), 3);
  for (u32 segment = 0; segment < 3; segment++)
    for (u32 mirror = 0; mirror < 4; mirror++)
      EXPECT_EQ(mapping_permissions(arena + segments[segment] + mirror * BUS_RAM_2MB_SIZE + offset), 3);
  u32 cleared;
  memcpy(&cleared, bus_get_unprotected_ram_pointer() + offset, sizeof(cleared));
  EXPECT_EQ(cleared, 0);
  finish_cpu();
}

static void check_eight_mb_mapping(cpu_fastmem_mode_t mode)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_INTERPRETER, mode, 0xA0010000, words, 2);
  g_settings.cpu_enable_8mb_ram = true;
  bus_initialize();
  EXPECT_EQ(bus_get_ram_size(), BUS_RAM_8MB_SIZE);
  const u32 segments[] = {0, 0x80000000, 0xA0000000};
  for (u32 chunk = 0; chunk < 4; chunk++) {
    const u32 offset = chunk * BUS_RAM_2MB_SIZE + 0x10000;
    const u32 value  = 0x12340000 | chunk;
    memcpy(bus_get_unprotected_ram_pointer() + offset, &value, sizeof(value));
    for (u32 segment = 0; segment < 3; segment++) {
      const u32 address = segments[segment] + offset;
      EXPECT_TRUE(bus_can_use_fastmem_for_address(address));
      const void* pointer;
      if (mode == CPU_FASTMEM_MODE_LUT) {
        u8* const*      table   = bus_get_fastmem_base(false);
        const uintptr_t decoded = (uintptr_t)table[address >> BUS_FASTMEM_LUT_PAGE_SHIFT] + address;
        EXPECT_EQ(decoded, (uintptr_t)(bus_get_ram_pointer() + offset));
        pointer = (const void*)decoded;
      } else {
        pointer = (u8*)bus_get_fastmem_base(false) + address;
      }
      u32 actual;
      memcpy(&actual, pointer, sizeof(actual));
      EXPECT_EQ(actual, value);
      EXPECT_EQ(cpu_get_memory_read_handler(address, MEMORY_ACCESS_SIZE_WORD)(address), value);
    }
  }
  for (u32 segment = 0; segment < 3; segment++) {
    for (u32 chunk = 4; chunk < 8; chunk++) {
      const u32 address = segments[segment] + chunk * BUS_RAM_2MB_SIZE + 0x10000;
      EXPECT_FALSE(bus_can_use_fastmem_for_address(address));
      if (mode == CPU_FASTMEM_MODE_LUT) {
        u8* const* table = bus_get_fastmem_base(false);
        EXPECT_EQ((uintptr_t)table[address >> BUS_FASTMEM_LUT_PAGE_SHIFT] + address, 0);
      } else {
        EXPECT_EQ(mapping_permissions((u8*)bus_get_fastmem_base(false) + address), 0);
      }
    }
  }
  if (mode == CPU_FASTMEM_MODE_MMAP) {
    const u32 offset = 3 * BUS_RAM_2MB_SIZE + 0x10000;
    const u32 page   = bus_get_ram_code_page_index(offset);
    bus_set_ram_code_page(page);
    for (u32 segment = 0; segment < 3; segment++) {
      u8* arena = bus_get_fastmem_base(false);
      EXPECT_EQ(mapping_permissions(arena + segments[segment] + offset), 1);
      EXPECT_EQ(mapping_permissions(arena + segments[segment] + 0x10000), 3);
    }
    bus_clear_ram_code_page(page);
    for (u32 segment = 0; segment < 3; segment++)
      EXPECT_EQ(mapping_permissions((u8*)bus_get_fastmem_base(false) + segments[segment] + offset), 3);
  }
  finish_cpu();
}

TEST(FastmemMapping, eight_mb_lut_chunks_and_holes)
{
  check_eight_mb_mapping(CPU_FASTMEM_MODE_LUT);
}
TEST(FastmemMapping, eight_mb_mmap_chunks_and_holes)
{
  check_eight_mb_mapping(CPU_FASTMEM_MODE_MMAP);
}
