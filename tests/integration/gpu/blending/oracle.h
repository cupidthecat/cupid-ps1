#ifndef CUPID_TEST_GPU_BLENDING_ORACLE_H
#define CUPID_TEST_GPU_BLENDING_ORACLE_H
#include "common/types.h"
#include "util/platform/window_info.h"

static inline u16 blend_test_color(u32 value, bool foreground) {
  const u32 g = (value * (foreground ? 5u : 7u) + (foreground ? 7u : 3u)) & 31u;
  const u32 b = (value * (foreground ? 13u : 11u) + (foreground ? 1u : 5u)) & 31u;
  return (u16)(value | g << 5 | b << 10);
}
static inline u16 blend_test_native(u32 rgba) {
  return (u16)(((rgba & 255u) >> 3) | ((((rgba >> 8) & 255u) >> 3) << 5) | ((((rgba >> 16) & 255u) >> 3) << 10) |
               ((rgba >> 31) << 15));
}
static inline u16 blend_test_foreground(u32 index, bool masked, bool special) {
  const u16 colors[] = {0, 0x8000, 1, 0x8001, 0x7FFF, 0xFFFF, 0x7C00, 0xFC00};
  return special ? colors[index % 8] : (u16)(blend_test_color(index, true) | (masked ? 0x8000 : 0));
}
static inline u16 blend_test_combine(u16 background, u16 foreground, u32 mode) {
  u32 result = 0;
  for (u32 channel = 0; channel < 3; channel++) {
    const s32 b = (background >> (channel * 5)) & 31, f = (foreground >> (channel * 5)) & 31;
    s32       value = mode == 0 ? (b + f) / 2 : mode == 1 ? b + f : mode == 2 ? b - f : b + f / 4;
    if (value < 0) value = 0;
    if (value > 31) value = 31;
    result |= (u32)value << (channel * 5);
  }
  return (u16)result;
}
static inline u16 blend_test_dither(u16 color, u32 x, u32 y) {
  const s32 matrix[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
  u16       result       = 0;
  for (u32 channel = 0; channel < 3; channel++) {
    s32 value = (((color >> (channel * 5)) & 31) * 8 + matrix[y & 3][x & 3]) >> 3;
    if (value < 0) value = 0;
    result |= (u16)((u32)value << (channel * 5));
  }
  return result;
}
u32 gpu_gl_blend_atlas_checks(const window_info_t* wi, u32* count);
u32 gpu_gl_blend_order_checks(const window_info_t* wi, u32* count);
#endif
