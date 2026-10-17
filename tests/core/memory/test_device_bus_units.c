/* cupid-ps1 external strobe sequencing and byte-enable regressions. */
#include "tests/test_harness.h"
#include "core/memory/device_bus.h"

typedef struct {
  u32 count;
  u32 address[8];
  u32 width[8];
  u32 value[8];
  u32 enabled[8];
} observed_bus_t;

static observed_bus_t observed;

static u32 read_sequence(u32 address, u32 unit_bytes)
{
  const u32 index = observed.count++;
  EXPECT_TRUE(index < 8);
  observed.address[index] = address;
  observed.width[index]   = unit_bytes;
  return unit_bytes == 1 ? 0xA0 + index : 0x1100 + index;
}

static void record_write(u32 address, u32 value, u32 unit_bytes, u32 enabled_bytes)
{
  const u32 index = observed.count++;
  EXPECT_TRUE(index < 8);
  observed.address[index] = address;
  observed.width[index]   = unit_bytes;
  observed.value[index]   = value & (unit_bytes == 1 ? 0xFF : 0xFFFF);
  observed.enabled[index] = enabled_bytes;
}

static void check_fifo_read(bool wide, bool increment, memory_access_size_t size)
{
  memset(&observed, 0, sizeof(observed));
  const u32 control       = (wide ? DEVICE_BUS_16BIT : 0) | (increment ? DEVICE_BUS_INCREMENT : 0);
  const u32 value         = device_bus_read(0x20, size, control, read_sequence);
  const u32 beats         = wide ? 2 : size == MEMORY_ACCESS_SIZE_WORD ? 4 : 2;
  const u32 addresses8[]  = {0x20, 0x21, 0x22, 0x23};
  const u32 addresses16[] = {0x20, 0x22};
  EXPECT_EQ(observed.count, beats);
  EXPECT_EQ(value, wide ? 0x11011100 : size == MEMORY_ACCESS_SIZE_WORD ? 0xA3A2A1A0 : 0xA1A0);
  for (u32 i = 0; i < beats; i++) {
    EXPECT_EQ(observed.address[i], increment ? (wide ? addresses16[i] : addresses8[i]) : 0x20);
    EXPECT_EQ(observed.width[i], wide ? 2 : 1);
  }
}

static void check_word_write(bool wide, bool increment)
{
  memset(&observed, 0, sizeof(observed));
  const u32 control = (wide ? DEVICE_BUS_16BIT : 0) | (increment ? DEVICE_BUS_INCREMENT : 0);
  device_bus_write(0x20, 0x44332211, MEMORY_ACCESS_SIZE_WORD, control, record_write);
  const u32 addresses8[]  = {0x20, 0x21, 0x22, 0x23};
  const u32 addresses16[] = {0x20, 0x22};
  const u32 values8[]     = {0x11, 0x22, 0x33, 0x44};
  const u32 values16[]    = {0x2211, 0x4433};
  EXPECT_EQ(observed.count, wide ? 2 : 4);
  for (u32 i = 0; i < observed.count; i++) {
    EXPECT_EQ(observed.address[i], increment ? (wide ? addresses16[i] : addresses8[i]) : 0x20);
    EXPECT_EQ(observed.width[i], wide ? 2 : 1);
    EXPECT_EQ(observed.value[i], wide ? values16[i] : values8[i]);
    EXPECT_EQ(observed.enabled[i], wide ? 3 : 1);
  }
}

#define READ_CASE(name, wide, increment, size) \
  TEST(DeviceBusUnits, name)                   \
  {                                            \
    check_fifo_read(wide, increment, size);    \
  }
READ_CASE(fifo8_half_hold, false, false, MEMORY_ACCESS_SIZE_HALFWORD)
READ_CASE(fifo8_half_increment, false, true, MEMORY_ACCESS_SIZE_HALFWORD)
READ_CASE(fifo8_word_hold, false, false, MEMORY_ACCESS_SIZE_WORD)
READ_CASE(fifo8_word_increment, false, true, MEMORY_ACCESS_SIZE_WORD)
READ_CASE(fifo16_word_hold, true, false, MEMORY_ACCESS_SIZE_WORD)
READ_CASE(fifo16_word_increment, true, true, MEMORY_ACCESS_SIZE_WORD)

#define WRITE_CASE(name, wide, increment) \
  TEST(DeviceBusUnits, name)              \
  {                                       \
    check_word_write(wide, increment);    \
  }
WRITE_CASE(write8_word_hold, false, false)
WRITE_CASE(write8_word_increment, false, true)
WRITE_CASE(write16_word_hold, true, false)
WRITE_CASE(write16_word_increment, true, true)

static void check_byte_lane(bool odd, bool write)
{
  memset(&observed, 0, sizeof(observed));
  if (write) {
    device_bus_write(0x20 + odd, 0x44332211, MEMORY_ACCESS_SIZE_BYTE, DEVICE_BUS_16BIT, record_write);
    EXPECT_EQ(observed.enabled[0], odd ? 2 : 1);
    EXPECT_EQ(observed.value[0], 0x2211);
  } else {
    EXPECT_EQ(device_bus_read(0x20 + odd, MEMORY_ACCESS_SIZE_BYTE, DEVICE_BUS_16BIT, read_sequence), odd ? 0x11 : 0);
  }
  EXPECT_EQ(observed.count, 1);
  EXPECT_EQ(observed.address[0], 0x20);
  EXPECT_EQ(observed.width[0], 2);
}

TEST(DeviceBusUnits, read16_even_byte)
{
  check_byte_lane(false, false);
}
TEST(DeviceBusUnits, read16_odd_byte)
{
  check_byte_lane(true, false);
}
TEST(DeviceBusUnits, write16_even_byte)
{
  check_byte_lane(false, true);
}
TEST(DeviceBusUnits, write16_odd_byte)
{
  check_byte_lane(true, true);
}
TEST(DeviceBusUnits, write8_odd_byte)
{
  memset(&observed, 0, sizeof(observed));
  device_bus_write(0x21, 0x44332211, MEMORY_ACCESS_SIZE_BYTE, 0, record_write);
  EXPECT_EQ(observed.count, 1);
  EXPECT_EQ(observed.address[0], 0x21);
  EXPECT_EQ(observed.width[0], 1);
  EXPECT_EQ(observed.enabled[0], 1);
  EXPECT_EQ(observed.value[0], 0x11);
}
TEST(DeviceBusUnits, write16_halfword)
{
  memset(&observed, 0, sizeof(observed));
  device_bus_write(0x20, 0x44332211, MEMORY_ACCESS_SIZE_HALFWORD, DEVICE_BUS_16BIT, record_write);
  EXPECT_EQ(observed.count, 1);
  EXPECT_EQ(observed.address[0], 0x20);
  EXPECT_EQ(observed.width[0], 2);
  EXPECT_EQ(observed.enabled[0], 3);
  EXPECT_EQ(observed.value[0], 0x2211);
}
