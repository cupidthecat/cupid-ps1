/* cupid-ps1 peripheral bus width and address-stepping regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/cdrom/cdrom.h"
#include "core/spu/spu.h"

static void start_peripheral_bus(bool cd)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  spu_initialize();
  if (cd) {
    g_settings.cdrom_readahead_sectors = 0;
    cdrom_initialize();
  }
}

static void finish_peripheral_bus(bool cd)
{
  if (cd) cdrom_shutdown();
  spu_shutdown();
  finish_cpu();
}

static u32 read_bus(u32 address, memory_access_size_t size)
{
  bus_memory_read_handler_t* handlers = bus_offset_read_handler_array(g_cpu_state.memory_handlers, size);
  return handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address);
}

static void write_bus(u32 address, u32 value, memory_access_size_t size)
{
  bus_memory_write_handler_t* handlers = bus_offset_write_handler_array(g_cpu_state.memory_handlers, size);
  handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address, value);
}

static void check_spu_word(bool wide, bool increment, bool write)
{
  start_peripheral_bus(false);
  write_bus(0x1F801014, 0x200901E1 | (wide ? 0x1000 : 0) | (increment ? 0x2000 : 0), MEMORY_ACCESS_SIZE_WORD);
  spu_write_register(0x180, 0x1234);
  spu_write_register(0x182, 0x5678);
  u32 value = 0;
  if (write)
    write_bus(0x1F801D80, 0x11223344, MEMORY_ACCESS_SIZE_WORD);
  else
    value = read_bus(0x1F801D80, MEMORY_ACCESS_SIZE_WORD);
  const u32 left = spu_read_register(0x180), right = spu_read_register(0x182);
  finish_peripheral_bus(false);
  printf("spu-device-bus wide=%u increment=%u write=%u value=%08X left=%04X right=%04X\n", wide, increment, write,
         value, left, right);
  fflush(stdout);
  if (write) {
    EXPECT_EQ(left, wide ? (increment ? 0x3344 : 0x1122) : (increment ? 0xFF33 : 0xFF11));
    EXPECT_EQ(right, increment ? (wide ? 0x1122 : 0xFF11) : 0x5678);
  } else {
    EXPECT_EQ(value, wide ? (increment ? 0x56781234 : 0x12341234) : (increment ? 0x78783434 : 0x34343434));
    EXPECT_EQ(left, 0x1234);
    EXPECT_EQ(right, 0x5678);
  }
}

static void check_spu_byte(bool wide, bool odd)
{
  start_peripheral_bus(false);
  write_bus(0x1F801014, 0x200921E1 | (wide ? 0x1000 : 0), MEMORY_ACCESS_SIZE_WORD);
  spu_write_register(0x180, 0x1234);
  spu_write_register(0x182, 0x5678);
  write_bus(0x1F801D80 + odd, 0x12345678, MEMORY_ACCESS_SIZE_BYTE);
  const u32 left = spu_read_register(0x180), right = spu_read_register(0x182);
  finish_peripheral_bus(false);
  EXPECT_EQ(left, wide ? (odd ? 0x1234 : 0x5678) : 0xFF78);
  EXPECT_EQ(right, 0x5678);
}

#define SPU_WORD_CASES(name, wide, increment) \
  TEST(DeviceBusPeripherals, name##_read)     \
  {                                           \
    check_spu_word(wide, increment, false);   \
  }                                           \
  TEST(DeviceBusPeripherals, name##_write)    \
  {                                           \
    check_spu_word(wide, increment, true);    \
  }
SPU_WORD_CASES(spu16_increment, true, true)
SPU_WORD_CASES(spu16_hold, true, false)
SPU_WORD_CASES(spu8_increment, false, true)
SPU_WORD_CASES(spu8_hold, false, false)
TEST(DeviceBusPeripherals, spu16_even_byte)
{
  check_spu_byte(true, false);
}
TEST(DeviceBusPeripherals, spu16_odd_byte)
{
  check_spu_byte(true, true);
}
TEST(DeviceBusPeripherals, spu8_even_byte)
{
  check_spu_byte(false, false);
}
TEST(DeviceBusPeripherals, spu8_odd_byte)
{
  check_spu_byte(false, true);
}

static void check_cd_response(bool increment, bool word)
{
  start_peripheral_bus(true);
  write_bus(0x1F801018, 0x00020943 | (increment ? 0x2000 : 0), MEMORY_ACCESS_SIZE_WORD);
  cdrom_write_register(1, 0x0F); /* Getmode returns status, mode and filter fields. */
  run_slice(30000);              /* No-disc acknowledgement is scheduled after 15000 ticks. */
  const u8 status = cdrom_read_register(0);
  EXPECT_TRUE(status & 0x20); /* A response is queued. */
  const u32 value = read_bus(0x1F801800, word ? MEMORY_ACCESS_SIZE_WORD : MEMORY_ACCESS_SIZE_HALFWORD);
  const u32 next  = cdrom_read_register(1);
  finish_peripheral_bus(true);
  const u32 held = word ? (u32)status * 0x01010101u : (u32)status * 0x0101u;
  EXPECT_EQ(value, increment ? (word ? 0xFF001000u | status : 0x1000u | status) : held);
  EXPECT_EQ(next, increment ? 0x20 : 0x10);
}

TEST(DeviceBusPeripherals, cd_half_increment)
{
  check_cd_response(true, false);
}
TEST(DeviceBusPeripherals, cd_half_hold)
{
  check_cd_response(false, false);
}
TEST(DeviceBusPeripherals, cd_word_increment)
{
  check_cd_response(true, true);
}
TEST(DeviceBusPeripherals, cd_word_hold)
{
  check_cd_response(false, true);
}
