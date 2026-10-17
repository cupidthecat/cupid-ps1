/* cupid-ps1 external bus transfer units. */
#include "device_bus.h"

static u32 access_mask(u32 bytes)
{
  return bytes == 1 ? 0xFFu : bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

u32 device_bus_read(u32 address, memory_access_size_t size, u32 control, device_bus_read_unit_t read_unit)
{
  const u32 bytes = 1u << size;
  const u32 unit  = (control & DEVICE_BUS_16BIT) ? 2u : 1u;
  if (bytes < unit) {
    const u32 lane = address & (unit - 1u);
    return (read_unit(address - lane, unit) >> (lane * 8u)) & access_mask(bytes);
  }
  u32 value = 0;
  for (u32 offset = 0; offset < bytes; offset += unit) {
    const u32 next = address + ((control & DEVICE_BUS_INCREMENT) ? offset : 0);
    value |= (read_unit(next, unit) & access_mask(unit)) << (offset * 8u);
  }
  return value;
}

void device_bus_write(u32 address, u32 value, memory_access_size_t size, u32 control,
                      device_bus_write_unit_t write_unit)
{
  const u32 bytes = 1u << size;
  const u32 unit  = (control & DEVICE_BUS_16BIT) ? 2u : 1u;
  if (bytes < unit) {
    const u32 lane = address & (unit - 1u);
    write_unit(address - lane, value, unit, ((1u << bytes) - 1u) << lane);
    return;
  }
  for (u32 offset = 0; offset < bytes; offset += unit) {
    const u32 next = address + ((control & DEVICE_BUS_INCREMENT) ? offset : 0);
    write_unit(next, value >> (offset * 8u), unit, (1u << unit) - 1u);
  }
}
