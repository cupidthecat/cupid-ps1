#include "tests/core/mdec/mdec_decode_fixture.h"

static void check_color_packing(bool signed_output)
{
  static const struct {
    s16 cr_dc, cb_dc, y_dc;
    s16 rgb[3];
  } cases[] = {{-32, 0, 0, {-11, 5, 0}},
               {0, -32, 0, {0, 2, -14}},
               {32, 32, -32, {3, -16, 6}},
               {0, 0, -32, {-8, -8, -8}},
               {0, 0, 32, {8, 8, 8}}};
  static const struct {
    u32 depth, bit15, words, pixel_bytes;
  } formats[]      = {{2, 0, 192, 3}, {3, 0, 128, 2}, {3, 1, 128, 2}};
  u32 observations = 0, failures = 0;
  for (u32 index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
    for (u32 format = 0; format < 3; format++) {
      for (u32 restore = 0; restore < 2; restore++) {
        for (u32 input_dma = 0; input_dma < 2; input_dma++) {
          for (u32 output_dma = 0; output_dma < 2; output_dma++) {
            begin_decode(true);
            mdec_write_register(0, 0x40000001);
            for (u32 i = 0; i < 32; i++)
              mdec_write_register(0, 0x01010101);
            mdec_write_register(0, 0x60000000);
            for (u32 i = 0; i < 32; i++)
              mdec_write_register(0, 0x7FFF7FFF);
            mdec_write_register(0, 0x20000006u | (formats[format].depth << 27) | (formats[format].bit15 << 25) |
                                       (signed_output ? 0x04000000u : 0));
            u32 input[6];
            for (u32 block = 0; block < 6; block++) {
              const s16 dc = block == 0 ? cases[index].cr_dc : (block == 1 ? cases[index].cb_dc : cases[index].y_dc);
              input[block] = 0xFE000400u | ((u32)(u16)dc & 0x3FFu);
            }
            if (input_dma) {
              mdec_dma_write(input, 6);
            } else {
              for (u32 block = 0; block < 6; block++)
                mdec_write_register(0, input[block]);
            }
            EXPECT_EQ(mdec_dbg_fifo_status() >> 24, 2);
            const tick_count_t until_copy = timing_event_get_ticks_until_next_execution(find_mdec_copy_event());
            EXPECT_TRUE(until_copy > 0);
            if (restore) restore_coefficients();
            EXPECT_EQ(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()), until_copy);
            const coefficient_snapshot_t decoded = read_coefficients();
            EXPECT_TRUE(decoded.pending);
            EXPECT_EQ(decoded.remaining, 0);
            for (u32 block = 0; block < 6; block++) {
              const s16 expected =
                  block == 0 ? cases[index].cr_dc / 4 : (block == 1 ? cases[index].cb_dc / 4 : cases[index].y_dc / 4);
              for (u32 i = 0; i < 64; i++)
                EXPECT_EQ(decoded.blocks[block][i], expected);
            }
            u8 rgb[3];
            for (u32 component = 0; component < 3; component++)
              rgb[component] = (u8)(cases[index].rgb[component] + (signed_output ? 0 : 128));
            const u32 expected_packed = (u32)rgb[0] | ((u32)rgb[1] << 8) | ((u32)rgb[2] << 16);
            u32       packed_wrong    = 0;
            for (u32 pixel = 0; pixel < 256; pixel++)
              packed_wrong += decoded.rgb[pixel] != expected_packed;
            u8 expected[3] = {rgb[0], rgb[1], rgb[2]};
            if (formats[format].pixel_bytes == 2) {
              const u16 packed15 =
                  (u16)((rgb[0] >> 3) | ((rgb[1] >> 3) << 5) | ((rgb[2] >> 3) << 10) | (formats[format].bit15 << 15));
              expected[0] = (u8)packed15;
              expected[1] = (u8)(packed15 >> 8);
              expected[2] = 0;
            }
            run_slice(until_copy + 1);
            EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, formats[format].words);
            u32 words[192];
            if (output_dma) {
              mdec_dma_read(words, formats[format].words);
            } else {
              for (u32 i = 0; i < formats[format].words; i++)
                words[i] = mdec_read_register(0);
            }
            EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, 0);
            const u8* bytes    = (const u8*)words;
            u32       wrong[3] = {0};
            for (u32 pixel = 0; pixel < 256; pixel++)
              for (u32 component = 0; component < formats[format].pixel_bytes; component++)
                wrong[component] += bytes[pixel * formats[format].pixel_bytes + component] != expected[component];
            printf("mdec-color-packing signed=%u case=%u depth=%u bit15=%u restore=%u input_dma=%u output_dma=%u "
                   "ticks=%d first=%02X/%02X/%02X expected=%02X/%02X/%02X packed_wrong=%u wrong=%u/%u/%u status=%08X\n",
                   signed_output, index, formats[format].depth == 2 ? 24 : 15, formats[format].bit15, restore,
                   input_dma, output_dma, until_copy, bytes[0], bytes[1],
                   formats[format].pixel_bytes == 3 ? bytes[2] : 0, expected[0], expected[1], expected[2], packed_wrong,
                   wrong[0], wrong[1], wrong[2], mdec_read_register(4));
            fflush(stdout);
            failures += packed_wrong != 0 || wrong[0] != 0 || wrong[1] != 0 || wrong[2] != 0;
            observations++;
            end_decode();
          }
        }
      }
    }
  }
  EXPECT_EQ(observations, 120);
  EXPECT_EQ(failures, 0);
}

TEST(MdecColorPacking, unsigned_control)
{
  check_color_packing(false);
}
TEST(MdecColorPacking, signed_channels)
{
  check_color_packing(true);
}
