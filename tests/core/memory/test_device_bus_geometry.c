/* cupid-ps1 external device address-stepping regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void check_bios_address_stepping(bool increment, bool fetch)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, loop, 2);
  const u32 pattern = 0x44332211;
  memcpy(bus_get_bios_pointer() + 0x400, &pattern, sizeof(pattern));
  bus_memory_write_handler_t* writes =
      bus_offset_write_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
  const u32 control = 0x0013043F | (increment ? 0x2000 : 0);
  writes[0x1F801010 >> BUS_MEMORY_LUT_PAGE_SHIFT](0x1F801010, control);
  u32 word, half = 0, byte = 0;
  if (fetch) {
    cpu_set_pc(0xBFC00400);
    word = g_cpu_state.next_instruction.bits;
  } else {
    bus_memory_read_handler_t* reads =
        bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_WORD);
    word  = reads[0xBFC00400 >> BUS_MEMORY_LUT_PAGE_SHIFT](0xBFC00400);
    reads = bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_HALFWORD);
    half  = reads[0xBFC00400 >> BUS_MEMORY_LUT_PAGE_SHIFT](0xBFC00400);
    reads = bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_BYTE);
    byte  = reads[0xBFC00400 >> BUS_MEMORY_LUT_PAGE_SHIFT](0xBFC00400);
  }
  finish_cpu();
  printf("bios-device-bus increment=%u fetch=%u word=%08X half=%04X byte=%02X\n", increment, fetch, word, half, byte);
  fflush(stdout);
  EXPECT_EQ(word, increment ? 0x44332211 : 0x11111111);
  if (!fetch) {
    EXPECT_EQ(half, increment ? 0x2211 : 0x1111);
    EXPECT_EQ(byte, 0x11);
  }
}

TEST(DeviceBusGeometry, bios_data_with_address_stepping)
{
  check_bios_address_stepping(true, false);
}
TEST(DeviceBusGeometry, bios_data_without_address_stepping)
{
  check_bios_address_stepping(false, false);
}
TEST(DeviceBusGeometry, bios_fetch_with_address_stepping)
{
  check_bios_address_stepping(true, true);
}
TEST(DeviceBusGeometry, bios_fetch_without_address_stepping)
{
  check_bios_address_stepping(false, true);
}
