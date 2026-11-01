/* cupid-ps1 signed color clipping, fixed-point scaling and FIFO checks. */
#include "tests/test_harness.h"
#include "core/gte/gte.h"
#include "core/config/settings.h"
#include "core/state/save_state_version.h"
#include "util/state/state_wrapper.h"

static void restore_gte_component(void)
{
  u8 saved[256];
  state_wrapper_t sw;
  state_wrapper_init_write(&sw, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(gte_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), sizeof(saved));
  gte_reset();
  state_wrapper_init_read(&sw, saved, sizeof(saved), SAVE_STATE_VERSION);
  EXPECT_TRUE(gte_do_state(&sw));
  EXPECT_FALSE(state_wrapper_has_error(&sw));
  EXPECT_EQ(state_wrapper_get_position(&sw), sizeof(saved));
}

static u32 clip_color(s32 value)
{
  if (value < 0) return 0;
  if (value > 4095) return 255;
  return (u32)value / 16;
}

TEST(GteArithmetic, signed_rgb_boundaries_fifo_flags_and_restore)
{
  const s32 values[] = {INT32_MIN, -32769, -32768, -4096, -17, -16, -15, -1,
                        0, 1, 15, 16, 4079, 4080, 4095, 4096, 32767, 32768, INT32_MAX};
  g_settings.gpu_pgxp_enable = false;
  for (u32 sample = 0; sample < countof(values); sample++) {
    for (u32 sf = 0; sf < 2; sf++) {
      for (u32 lm = 0; lm < 2; lm++) {
        for (u32 restore = 0; restore < 2; restore++) {
          gte_initialize();
          gte_write_register(6, 0xA5000000);
          gte_write_register(20, 0x11010203);
          gte_write_register(21, 0x22040506);
          gte_write_register(22, 0x33070809);
          gte_write_register(63, 0x7FFFF000);
          s32 lanes[3];
          for (u32 i = 0; i < 3; i++) {
            lanes[i] = values[(sample + i * 5) % countof(values)];
            gte_write_register(25 + i, (u32)lanes[i]);
          }
          if (restore) restore_gte_component();
          gte_execute(0x4A00003E | sf << 19 | lm << 10);
          const u32 rgb = 0xA5000000 | clip_color(lanes[0]) |
                          clip_color(lanes[1]) << 8 | clip_color(lanes[2]) << 16;
          EXPECT_EQ(gte_read_register(20), 0x22040506);
          EXPECT_EQ(gte_read_register(21), 0x33070809);
          EXPECT_EQ(gte_read_register(22), rgb);
          u32 flags = 0;
          for (u32 i = 0; i < 3; i++) {
            s32 ir = lanes[i];
            const s32 minimum = lm ? 0 : -32768;
            if (ir < minimum) { ir = minimum; flags |= 1u << (24 - i); }
            if (ir > 32767) { ir = 32767; flags |= 1u << (24 - i); }
            if (lanes[i] < 0 || lanes[i] > 4095) flags |= 1u << (21 - i);
            EXPECT_EQ(gte_read_register(9 + i), (u32)ir);
            EXPECT_EQ(gte_read_register(25 + i), (u32)lanes[i]);
          }
          if (flags & 0x7F87E000) flags |= 0x80000000;
          EXPECT_EQ(gte_read_register(63), flags);
        }
      }
    }
  }
}

TEST(GteArithmetic, color_commands_keep_signed_results_before_rgb_clipping)
{
  const u32 commands[] = {0x10, 0x11, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                          0x1E, 0x20, 0x29, 0x2A, 0x3D, 0x3E, 0x3F};
  g_settings.gpu_pgxp_enable = false;
  for (u32 command = 0; command < countof(commands); command++) {
    for (u32 sf = 0; sf < 2; sf++) {
      for (u32 lm = 0; lm < 2; lm++) {
        gte_initialize();
        const u32 op = commands[command];
        const bool depth_product = op == 0x13 || op == 0x14 || op == 0x16 || op == 0x29;
        gte_write_register(6, depth_product ? 0xA5000000 : 0xA5101010);
        gte_write_register(8, (op == 0x11 || op == 0x3E) ? 0 : 4096);
        for (u32 i = 0; i < 3; i++) {
          gte_write_register(9 + i, UINT32_MAX);
          gte_write_register(25 + i, UINT32_MAX);
          gte_write_register(45 + i, UINT32_MAX);
          gte_write_register(53 + i, UINT32_MAX);
        }
        gte_write_register(20, 0x11010203);
        gte_write_register(21, 0x22040506);
        gte_write_register(22, 0x33070809);
        gte_execute(0x4A000000 | op | sf << 19 | lm << 10);
        const bool triple = op == 0x16 || op == 0x20 || op == 0x2A || op == 0x3F;
        const bool zero_mac = lm && (op == 0x1B || op == 0x1C || op == 0x3F);
        if (gte_read_register(22) != 0xA5000000)
          printf("color op=%02X sf=%u lm=%u mac=%08X/%08X/%08X flag=%08X rgb=%08X\n", op, sf, lm,
                 gte_read_register(25), gte_read_register(26), gte_read_register(27),
                 gte_read_register(63), gte_read_register(22));
        EXPECT_EQ(gte_read_register(20), triple ? 0xA5000000 : 0x22040506);
        EXPECT_EQ(gte_read_register(21), triple ? 0xA5000000 : 0x33070809);
        EXPECT_EQ(gte_read_register(22), 0xA5000000);
        for (u32 i = 0; i < 3; i++) {
          const s32 mac = (s32)gte_read_register(25 + i);
          if (zero_mac) EXPECT_EQ(mac, 0); else EXPECT_TRUE(mac < 0);
        }
        EXPECT_EQ(gte_read_register(63) & 0x00380000, zero_mac ? 0 : 0x00380000);
      }
    }
  }
}

TEST(GteArithmetic, negative_translation_and_projection_scaling)
{
  const s32 translations[3] = {-3, -5, -9};
  const u32 commands[] = {0x01, 0x12, 0x30};
  g_settings.gpu_pgxp_enable = false;
  for (u32 command = 0; command < countof(commands); command++) {
    for (u32 sf = 0; sf < 2; sf++) {
      for (u32 lm = 0; lm < 2; lm++) {
        for (u32 restore = 0; restore < 2; restore++) {
          gte_initialize();
          for (u32 i = 0; i < 3; i++) gte_write_register(37 + i, (u32)translations[i]);
          if (restore) restore_gte_component();
          gte_execute(0x4A000000 | commands[command] | sf << 19 | lm << 10);
          for (u32 i = 0; i < 3; i++) {
            const s32 mac = translations[i] * (sf ? 1 : 4096);
            EXPECT_EQ(gte_read_register(25 + i), (u32)mac);
          }
          if (commands[command] != 0x12) EXPECT_EQ(gte_read_register(19), 0);
        }
      }
    }
  }
}

TEST(GteArithmetic, supported_command_latencies)
{
  const u8 commands[] = {0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                         0x1E, 0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F};
  const u8 cycles[] = {15, 8, 6, 8, 8, 8, 19, 13, 44, 17, 11, 14, 30, 5, 8, 17, 5, 6, 23, 5, 5, 39};
  for (u32 i = 0; i < countof(commands); i++)
    for (u32 modifier = 0; modifier < 4; modifier++)
      EXPECT_EQ(gte_get_instruction_cycles(0x4A000000 | commands[i] |
                  (modifier & 1) << 19 | (modifier >> 1) << 10), cycles[i]);
}
