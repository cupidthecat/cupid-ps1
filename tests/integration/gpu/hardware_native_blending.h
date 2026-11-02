#ifndef CUPID_TEST_GPU_HARDWARE_NATIVE_BLENDING_H
#define CUPID_TEST_GPU_HARDWARE_NATIVE_BLENDING_H
#include "common/types.h"
#include "util/platform/window_info.h"
u32 gpu_gl_native_blend_checks(const window_info_t* wi, u32* count);
#endif
