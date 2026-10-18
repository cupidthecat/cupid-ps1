#include "tests/core/gpu/gpu_reset_display_fixture.h"

static void check_display_reset(bool restore, bool controls)
{
  const u16 addresses[][2] = {{0, 0}, {64, 32}, {768, 300}, {1000, 500}};
  u32       observations = 0, failures = 0;
  for (u32 command = 0; command < (controls ? 2u : 4u); command++) {
    for (u32 pal = 0; pal < 2; pal++) {
      for (u32 address = 0; address < 4; address++) {
        display_fixture_t fixture;
        begin_display(&fixture);
        const gpu_backend_update_display_cmd_t reset = read_display();
        gpu_write_register(4, 0x08000000 | (pal ? 8u : 0u));
        const gpu_backend_update_display_cmd_t base = read_display();
        const u16                              x = addresses[address][0], y = addresses[address][1];
        gpu_write_register(4, 0x05000000 | x | ((u32)y << 10));
        const gpu_backend_update_display_cmd_t changed = read_display();
        EXPECT_EQ(changed.display_vram_left, (base.display_vram_left + x) % 1024);
        EXPECT_EQ(changed.display_vram_top, (base.display_vram_top + y) % 512);
        if (restore) restore_display();
        gpu_write_register(4, controls ? ((command + 1) << 24) : (command << 30));
        const gpu_backend_update_display_cmd_t actual   = read_display();
        const gpu_backend_update_display_cmd_t expected = controls ? changed : reset;
        const bool                             equal    = display_equal(&actual, &expected);
        printf("gpu-reset-display controls=%u restore=%u command=%u pal=%u address=%u,%u before=%u,%u after=%u,%u "
               "expected=%u,%u complete_config_equal=%u\n",
               controls, restore, command, pal, x, y, changed.display_vram_left, changed.display_vram_top,
               actual.display_vram_left, actual.display_vram_top, expected.display_vram_left, expected.display_vram_top,
               equal);
        failures += !equal;
        observations++;
        end_display(&fixture);
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, controls ? 16 : 32);
  EXPECT_EQ(failures, 0);
}

TEST(GpuResetDisplay, fresh)
{
  check_display_reset(false, false);
}
TEST(GpuResetDisplay, restored)
{
  check_display_reset(true, false);
}
TEST(GpuResetDisplay, acknowledge_and_fifo_controls)
{
  check_display_reset(true, true);
}
