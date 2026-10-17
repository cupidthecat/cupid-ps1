/* cupid-ps1 external bus transfer units. */
#ifndef CUPID_CORE_MEMORY_DEVICE_BUS_H
#define CUPID_CORE_MEMORY_DEVICE_BUS_H

#include "core/shared/types.h"

enum {
  DEVICE_BUS_16BIT     = 1u << 12,
  DEVICE_BUS_INCREMENT = 1u << 13,
};

typedef u32 (*device_bus_read_unit_t)(u32 address, u32 unit_bytes);
typedef void (*device_bus_write_unit_t)(u32 address, u32 value, u32 unit_bytes, u32 enabled_bytes);

/* Each callback represents one strobe. Repeated addresses still call the device. */
u32  device_bus_read(u32 address, memory_access_size_t size, u32 control, device_bus_read_unit_t read_unit);
void device_bus_write(u32 address, u32 value, memory_access_size_t size, u32 control,
                      device_bus_write_unit_t write_unit);

#endif
