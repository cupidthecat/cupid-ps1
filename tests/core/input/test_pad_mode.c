#include "tests/core/serial/serial_mode_fixture.h"

TEST(SerialModePad, writable_bits_and_other_registers)
{
  mode_check_bits(&pad_mode_device);
}
TEST(SerialModePad, writes_replace_the_previous_value)
{
  mode_check_replacement(&pad_mode_device);
}
TEST(SerialModePad, hardware_and_control_reset)
{
  mode_check_resets(&pad_mode_device);
}
TEST(SerialModePad, save_state_roundtrip)
{
  mode_check_roundtrip(&pad_mode_device);
}
TEST(SerialModePad, legacy_state_clears_reserved_bits)
{
  mode_check_legacy_state(&pad_mode_device);
}
