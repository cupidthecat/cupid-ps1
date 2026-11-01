/* cupid-ps1 CPU block order and DMA raster placement regressions. */
#include "tests/core/mdec/mdec_spatial_fixture.h"

static void check_spatial(u32 depth, bool dma) {
  u32 failures = 0;
  for (u32 old = 0; old < 2; old++) {
    for (u32 signed_output = 0; signed_output < 2; signed_output++) {
      for (u32 bit15 = 0; bit15 < (depth == 3 ? 2u : 1u); bit15++) {
        decode_spatial_quad(old, depth, signed_output, bit15);
        u32       words[192];
        const u32 count = spatial_word_count(depth);
        if (dma) {
          spatial_dma(0x50000, count);
          memcpy(words, bus_get_unprotected_ram_pointer() + 0x50000, count * 4);
        } else {
          for (u32 i = 0; i < count; i++)
            words[i] = mdec_read_register(0);
        }
        u8 expected[768];
        spatial_expected(expected, depth, signed_output, bit15, !dma);
        const u8* actual = (const u8*)words;
        u32       wrong  = 0;
        for (u32 i = 0; i < count * 4; i++)
          wrong += actual[i] != expected[i];
        printf("mdec-spatial depth=%u dma=%u old=%u signed=%u bit15=%u bytes_wrong=%u\n", depth, dma, old,
               signed_output, bit15, wrong);
        failures += wrong != 0;
        end_decode();
      }
    }
  }
  EXPECT_EQ(failures, 0);
}

TEST(MdecSpatial, cpu_port_15bit_blocks) {
  check_spatial(3, false);
}
TEST(MdecSpatial, cpu_port_24bit_blocks) {
  check_spatial(2, false);
}
TEST(MdecSpatial, dma_15bit_raster_control) {
  check_spatial(3, true);
}
TEST(MdecSpatial, dma_24bit_raster_control) {
  check_spatial(2, true);
}
