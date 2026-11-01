/* cupid-ps1 executable payload and memory initialization regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/bios/bios.h"

static void check_fill(u32 address, u32 size, bool set_pc, const u32 expected[8])
{
  const u32 words[] = {0x1000FFFF, 0, 0x11223344, 0x55667788, 0x99AABBCC};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x80020000, words, 2);
  u32 saved_regs[CPU_REG_COUNT];
  memcpy(saved_regs, g_cpu_state.regs.r, sizeof(saved_regs));
  bios_psexe_header_t header = {0};
  memcpy(header.id, "PS-X EXE", sizeof(header.id));
  header.initial_pc = header.load_address = 0x80010000;
  header.file_size     = sizeof(words);
  header.memfill_start = address;
  header.memfill_size  = size;
  u8 image[sizeof(header) + sizeof(words)];
  memcpy(image, &header, sizeof(header));
  memcpy(image + sizeof(header), words, sizeof(words));
  u8* ram = bus_get_unprotected_ram_pointer();
  memset(ram + 0x10000, 0xCD, 32);
  memset(ram + 0x30000, 0xCD, 16);
  Error error;
  Error_init(&error);
  EXPECT_TRUE(bus_inject_executable(image, sizeof(image), set_pc, &error));
  u32 actual[8];
  memcpy(actual, ram + 0x10000, sizeof(actual));
  for (u32 i = 0; i < 8; i++) EXPECT_EQ(actual[i], expected[i]);
  for (u32 i = 0; i < 16; i++)
    EXPECT_EQ(ram[0x30000 + i], address == 0x80030000 && i < size ? 0 : 0xCD);
  EXPECT_EQ(g_cpu_state.pc, set_pc ? header.initial_pc : 0x80020000);
  if (!set_pc) EXPECT_EQ(memcmp(saved_regs, g_cpu_state.regs.r, sizeof(saved_regs)), 0);
  Error_destroy(&error);
  finish_cpu();
}

TEST(ExecutableMemfill, nonoverlapping_region_preserves_payload)
{
  const u32 expected[] = {0x1000FFFF, 0, 0x11223344, 0x55667788, 0x99AABBCC,
                          0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD};
  check_fill(0x80030000, 8, true, expected);
}

TEST(ExecutableMemfill, overlapping_region_clears_loaded_bytes)
{
  const u32 expected[] = {0x1000FFFF, 0, 0, 0, 0x99AABBCC, 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD};
  check_fill(0x80010008, 8, true, expected);
}

TEST(ExecutableMemfill, region_can_extend_beyond_payload)
{
  const u32 expected[] = {0x1000FFFF, 0, 0x11223344, 0x55667788, 0, 0, 0, 0xCDCDCDCD};
  check_fill(0x80010010, 12, true, expected);
}

TEST(ExecutableMemfill, zero_size_preserves_loaded_bytes)
{
  const u32 expected[] = {0x1000FFFF, 0, 0x11223344, 0x55667788, 0x99AABBCC,
                          0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD};
  check_fill(0x80010008, 0, true, expected);
}

TEST(ExecutableMemfill, load_only_initializes_memory_without_changing_registers)
{
  const u32 expected[] = {0x1000FFFF, 0, 0, 0, 0x99AABBCC, 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD};
  check_fill(0x80010008, 8, false, expected);
}
