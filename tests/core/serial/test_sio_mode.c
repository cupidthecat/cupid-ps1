#include "tests/core/serial/serial_mode_fixture.h"

TEST(SerialModeSio, writable_bits_and_other_registers)
{
  mode_check_bits(&sio_mode_device);
}
TEST(SerialModeSio, writes_replace_the_previous_value)
{
  mode_check_replacement(&sio_mode_device);
}
TEST(SerialModeSio, hardware_and_control_reset)
{
  mode_check_resets(&sio_mode_device);
}
TEST(SerialModeSio, save_state_roundtrip)
{
  mode_check_roundtrip(&sio_mode_device);
}
TEST(SerialModeSio, legacy_state_clears_reserved_bits)
{
  mode_check_legacy_state(&sio_mode_device);
}
