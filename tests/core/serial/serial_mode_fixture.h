/* cupid-ps1 serial register test helpers. */
#ifndef CUPID_TESTS_SERIAL_MODE_FIXTURE_H
#define CUPID_TESTS_SERIAL_MODE_FIXTURE_H

#include "tests/test_harness.h"
#include "core/input/pad.h"
#include "core/serial/sio.h"
#include "core/scheduler/timing_event.h"
#include "core/state/save_state_version.h"
#include "util/state/state_wrapper.h"

typedef struct {
  void (*initialize)(void);
  void (*shutdown)(void);
  void (*reset)(void);
  u32 (*read)(u32 offset);
  void (*write)(u32 offset, u32 value);
  bool (*state)(state_wrapper_t* sw);
  u16 retained_bits;
  u16 written_5678;
  u16 bit_readback[16];
} serial_mode_device_t;

static bool pad_mode_state(state_wrapper_t* sw)
{
  return pad_do_state(sw, false);
}

static const serial_mode_device_t pad_mode_device = {pad_initialize,
                                                     pad_shutdown,
                                                     pad_reset,
                                                     pad_read_register,
                                                     pad_write_register,
                                                     pad_mode_state,
                                                     0x013F,
                                                     0x0038,
                                                     {1, 2, 4, 8, 0x10, 0x20, 0, 0, 0x100, 0, 0, 0, 0, 0, 0, 0}};
static const serial_mode_device_t sio_mode_device = {sio_initialize,
                                                     sio_shutdown,
                                                     sio_reset,
                                                     sio_read_register,
                                                     sio_write_register,
                                                     sio_do_state,
                                                     0x00FF,
                                                     0x0078,
                                                     {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0, 0, 0, 0, 0, 0, 0, 0}};

static void mode_begin(const serial_mode_device_t* device)
{
  timing_events_initialize();
  device->initialize();
}

static void mode_finish(const serial_mode_device_t* device)
{
  device->shutdown();
  timing_events_shutdown();
}

static void mode_check_bits(const serial_mode_device_t* device)
{
  mode_begin(device);
  device->write(0x0A, 0x0100);
  device->write(0x0E, 0x1357);
  const u32 status = device->read(0x04);
  for (u32 bit = 0; bit < 32; bit++) {
    device->write(0x08, 1u << bit);
    EXPECT_EQ(device->read(0x08), bit < 16 ? device->bit_readback[bit] : 0);
    EXPECT_EQ(device->read(0x0A), 0x0100);
    EXPECT_EQ(device->read(0x0E), 0x1357);
    EXPECT_EQ(device->read(0x04), status);
  }
  mode_finish(device);
}

static void mode_check_replacement(const serial_mode_device_t* device)
{
  mode_begin(device);
  device->write(0x08, 0xFFFFFFFF);
  EXPECT_EQ(device->read(0x08), device->retained_bits);
  device->write(0x08, 0x12345678);
  EXPECT_EQ(device->read(0x08), device->written_5678);
  device->write(0x08, 0xFFFF0000);
  EXPECT_EQ(device->read(0x08), 0);
  device->write(0x08, device->retained_bits);
  device->write(0x08, 0);
  EXPECT_EQ(device->read(0x08), 0);
  mode_finish(device);
}

static void mode_check_resets(const serial_mode_device_t* device)
{
  mode_begin(device);
  device->write(0x08, device->retained_bits);
  EXPECT_EQ(device->read(0x08), device->retained_bits);
  device->reset();
  EXPECT_EQ(device->read(0x08), 0);
  device->write(0x08, device->retained_bits);
  device->write(0x0A, 0x0040);
  EXPECT_EQ(device->read(0x08), 0);
  mode_finish(device);
}

static size_t mode_save(const serial_mode_device_t* device, u8* data, size_t capacity)
{
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, data, capacity, SAVE_STATE_VERSION);
  EXPECT_TRUE(device->state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  return state_wrapper_get_position(&sw);
}

static void mode_load(const serial_mode_device_t* device, const u8* data, size_t size)
{
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, data, size, SAVE_STATE_VERSION);
  EXPECT_TRUE(device->state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), size);
}

static void mode_check_roundtrip(const serial_mode_device_t* device)
{
  mode_begin(device);
  u8 saved[4096], restored[4096];
  device->write(0x08, 0x12345678);
  const size_t size = mode_save(device, saved, sizeof(saved));
  device->reset();
  mode_load(device, saved, size);
  EXPECT_EQ(device->read(0x08), device->written_5678);
  EXPECT_EQ(mode_save(device, restored, sizeof(restored)), size);
  EXPECT_EQ(memcmp(saved, restored, size), 0);
  mode_finish(device);
}

static void mode_check_legacy_state(const serial_mode_device_t* device)
{
  mode_begin(device);
  u8           zero[4096], one[4096], valid[4096], restored[4096];
  const size_t size = mode_save(device, zero, sizeof(zero));
  device->write(0x08, 1);
  EXPECT_EQ(mode_save(device, one, sizeof(one)), size);
  size_t mode_offset = size;
  for (size_t i = 0; i < size; i++) {
    if (zero[i] == one[i]) continue;
    EXPECT_EQ(mode_offset, size);
    EXPECT_EQ(zero[i], 0);
    EXPECT_EQ(one[i], 1);
    mode_offset = i;
  }
  EXPECT_TRUE(mode_offset + 1 < size);
  device->write(0x08, device->retained_bits);
  EXPECT_EQ(mode_save(device, valid, sizeof(valid)), size);
  /* Older files can contain reserved bits. Keep their layout and valid bits. */
  one[mode_offset]     = 0xFF;
  one[mode_offset + 1] = 0xFF;
  mode_load(device, one, size);
  EXPECT_EQ(device->read(0x08), device->retained_bits);
  EXPECT_EQ(mode_save(device, restored, sizeof(restored)), size);
  EXPECT_EQ(memcmp(valid, restored, size), 0);
  mode_finish(device);
}

#endif
