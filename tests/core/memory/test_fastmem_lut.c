/* cupid-ps1 fastmem table mapping checks. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static uintptr_t decoded_lut_address(u32 address, bool isolated)
{
  u8* const* table = (u8* const*)bus_get_fastmem_base(isolated);
  EXPECT_TRUE(table != NULL);
  return (uintptr_t)table[address >> BUS_FASTMEM_LUT_PAGE_SHIFT] + (uintptr_t)address;
}

TEST(FastmemLut, ram_aliases_decode_to_their_shared_backing)
{
  const u32 words[2] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 2);
  const u32 addresses[] = {0x00010000, 0x00210000, 0x80010000, 0x80210000, 0xA0010000, 0xA0210000};
  for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
    const uintptr_t target = decoded_lut_address(addresses[i], false);
    EXPECT_EQ(target, (uintptr_t)(bus_get_ram_pointer() + 0x10000));
    u32 value;
    memcpy(&value, (const void*)target, sizeof(value));
    EXPECT_EQ(value, words[0]);
  }
  finish_cpu();
}

TEST(FastmemLut, holes_and_isolated_views_decode_to_null)
{
  const u32 words[2] = {0x1000FFFF, 0};
  start_cpu_with_fastmem(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_LUT, 0xA0010000, words, 2);
  const u32 holes[] = {0x00800000, 0x1F801000, 0xC0010000, 0xFFFFF000};
  for (size_t i = 0; i < sizeof(holes) / sizeof(holes[0]); i++)
    EXPECT_EQ(decoded_lut_address(holes[i], false), 0);
  EXPECT_EQ(decoded_lut_address(0, true), 0);
  EXPECT_EQ(decoded_lut_address(0x00010000, true), 0);
  EXPECT_EQ(decoded_lut_address(0x80010000, true), 0);
  EXPECT_EQ(decoded_lut_address(0xA0010000, true), 0);
  finish_cpu();
}
