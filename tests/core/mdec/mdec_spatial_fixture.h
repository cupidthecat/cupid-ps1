/* cupid-ps1 four constant blocks distinguish device order from RAM layout. */
#ifndef CUPID_TEST_MDEC_SPATIAL_FIXTURE_H
#define CUPID_TEST_MDEC_SPATIAL_FIXTURE_H
#include "tests/core/mdec/mdec_decode_fixture.h"

static u32 spatial_word_count(u32 depth) {
  return depth == 3 ? 128 : 192;
}

static void queue_spatial_quad(u32 depth, bool signed_output, bool bit15) {
  mdec_write_register(4, 0x60000000);
  mdec_write_register(0, 0x40000001);
  for (u32 i = 0; i < 32; i++)
    mdec_write_register(0, 0);
  mdec_write_register(0, 0x60000000);
  for (u32 i = 0; i < 32; i++)
    mdec_write_register(0, 0x40004000);
  mdec_write_register(0, 0x20000006 | (depth << 27) | (signed_output ? 1u << 26 : 0) | (bit15 ? 1u << 25 : 0));
  const s16 dc[] = {0, 0, -128, -64, 64, 128};
  for (u32 i = 0; i < 6; i++)
    mdec_write_register(0, 0xFE000000 | ((u32)dc[i] & 0x3FF));
  run_slice(timing_event_get_ticks_until_next_execution(find_mdec_copy_event()) + 1);
  EXPECT_EQ(mdec_dbg_fifo_status() & 0x7FF, spatial_word_count(depth));
}

static void decode_spatial_quad(bool old, u32 depth, bool signed_output, bool bit15) {
  begin_decode(old);
  queue_spatial_quad(depth, signed_output, bit15);
}

static void spatial_expected(u8* bytes, u32 depth, bool signed_output, bool bit15, bool tiled) {
  const s16 luma[] = {-16, -8, 8, 16};
  for (u32 pixel = 0; pixel < 256; pixel++) {
    const u32 block = tiled ? pixel / 64 : ((pixel / 16) / 8) * 2 + (pixel % 16) / 8;
    const u8  value = (u8)(luma[block] + (signed_output ? 0 : 128));
    if (depth == 3) {
      const u16 rgb        = (u16)((value >> 3) * 1057u) | (bit15 ? 0x8000 : 0);
      bytes[pixel * 2]     = (u8)rgb;
      bytes[pixel * 2 + 1] = (u8)(rgb >> 8);
    } else {
      memset(bytes + pixel * 3, value, 3);
    }
  }
}

static void spatial_dma(u32 address, u32 words) {
  EXPECT_EQ(words % 4, 0);
  dma_write_register(0x70, 0x80);
  dma_write_register(0x10, address);
  dma_write_register(0x14, ((words / 4) << 16) | 4);
  dma_write_register(0x18, 0x01000200);
  for (u32 i = 0; i < 16 && (dma_dbg_channel_control(1) & (1u << 24)); i++)
    run_slice(1000);
  EXPECT_EQ(dma_dbg_channel_control(1) & (1u << 24), 0);
}
#endif
