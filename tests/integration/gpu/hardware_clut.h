/* cupid-ps1 actual OpenGL palette-cache checks. */
#ifndef CUPID_TESTS_GPU_HARDWARE_CLUT_H
#define CUPID_TESTS_GPU_HARDWARE_CLUT_H
#include "common/types.h"
#include "util/platform/window_info.h"
u32 gpu_gl_clut_checks(const window_info_t* wi, u32* count);
#endif
