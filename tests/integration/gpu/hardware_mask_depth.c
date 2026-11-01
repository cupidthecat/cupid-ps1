/* cupid-ps1 OpenGL destination-mask history through draws and transfers. */
#include "common/error.h"
#include "core/config/settings.h"
#include "core/state/save_state_version.h"
#include "core/gpu/hardware/gpu_hw.h"
#include "core/gpu/software/gpu_sw_rasterizer.h"
#include "util/state/state_wrapper.h"
#include "util/graphics/opengl_device.h"
#include "util/graphics/opengl_texture.h"
#include "util/platform/window_info.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>

void host_pump_messages_on_core_thread(void);
void host_input_manager_poll_sources(void);
void host_wait_for_all_async_tasks(void);
void host_pump_messages_on_core_thread(void) {}
void host_input_manager_poll_sources(void) {}
void host_wait_for_all_async_tasks(void) {}

enum { RECTANGLE, TRIANGLE, LINE };
enum { PLAIN, ROLLOVER, UPLOAD_SET, UPLOAD_CLEAR, FILL_CLEAR, COPY_SET, COPY_CLEAR, RESTORE, RESIZE, NATURAL_ROLLOVER };

/* Read the GPU texture through a temporary read framebuffer. The software VRAM
 * mirror cannot satisfy this check. Restore the device's existing read binding. */
static u32 read_color(gpu_hw_t* hw, u32 x, u32 y) {
  GLint  old;
  GLuint framebuffer;
  u32    color = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw->vram_texture->id, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels((GLint)(x * hw->resolution_scale), (GLint)(y * hw->resolution_scale), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
               &color);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old);
  glDeleteFramebuffers(1, &framebuffer);
  return color;
}

static void rectangle(gpu_backend_t* be, s32 x, u32 color, bool set_mask) {
  const gpu_backend_draw_rectangle_cmd_t cmd = {.base   = {.num_vertices = 4, .set_mask_while_drawing = set_mask},
                                                .width  = 1,
                                                .height = 1,
                                                .x      = x,
                                                .y      = 20,
                                                .color  = color};
  be->vtable->draw_rectangle(be, &cmd);
}

static void draw(gpu_backend_t* be, u32 shape) {
  const gpu_backend_draw_cmd_t base = {.num_vertices = 3, .check_mask_before_draw = true};
  if (shape == RECTANGLE) {
    const gpu_backend_draw_rectangle_cmd_t cmd = {
        .base = base, .width = 2, .height = 1, .x = 20, .y = 20, .color = 0x00FF0000};
    be->vtable->draw_rectangle(be, &cmd);
  } else if (shape == TRIANGLE) {
    const gpu_backend_draw_polygon_cmd_t cmd        = {.base = base};
    const gpu_backend_polygon_vertex_t   vertices[] = {{.x = 20, .y = 20, .color = 0x00FF0000},
                                                       {.x = 24, .y = 20, .color = 0x00FF0000},
                                                       {.x = 20, .y = 24, .color = 0x00FF0000}};
    be->vtable->draw_polygon(be, &cmd, vertices);
  } else {
    gpu_backend_draw_line_cmd_t cmd            = {.base = base};
    cmd.base.num_vertices                      = 2;
    const gpu_backend_line_vertex_t vertices[] = {{.x = 20, .y = 20, .color = 0x00FF0000},
                                                  {.x = 24, .y = 20, .color = 0x00FF0000}};
    be->vtable->draw_line(be, &cmd, vertices);
  }
}

static bool check(gpu_hw_t* hw, u32 shape, u32 operation) {
  gpu_backend_t* be = &hw->base;
  be->vtable->clear_vram(be);
  hw->current_depth          = 1;
  const bool seed_protection = operation != UPLOAD_SET && operation != COPY_SET;
  rectangle(be, 20, 0x000000FF, seed_protection);
  rectangle(be, 21, 0x0000FF00, false);
  be->vtable->flush_render(be);
  const u32 seed_mask = read_color(hw, 20, 20), seed_clear = read_color(hw, 21, 20);
  if (seed_mask != (seed_protection ? 0xFF0000FF : 0x000000FF) || seed_clear != 0x0000FF00) {
    printf("FAIL setup scale=%u shape=%u operation=%u masked=%08X clear=%08X gl=%04X\n", hw->resolution_scale, shape,
           operation, seed_mask, seed_clear, glGetError());
    return false;
  }

  bool expect_mask = true;
  if (operation == UPLOAD_SET || operation == UPLOAD_CLEAR) {
    const u16                           pixels[] = {operation == UPLOAD_SET ? 0x801F : 0x03E0, 0x03E0};
    const gpu_backend_update_vram_cmd_t cmd      = {.x = 20, .y = 20, .width = 2, .height = 1};
    be->vtable->update_vram(be, &cmd, pixels);
    expect_mask = operation == UPLOAD_SET;
  } else if (operation == FILL_CLEAR) {
    const gpu_backend_fill_vram_cmd_t cmd = {.x = 20, .y = 20, .width = 2, .height = 1, .color = 0x000000FF};
    be->vtable->fill_vram(be, &cmd);
    expect_mask = false;
  } else if (operation == COPY_SET || operation == COPY_CLEAR) {
    rectangle(be, 30, 0x000000FF, operation == COPY_SET);
    rectangle(be, 31, 0x0000FF00, false);
    const gpu_backend_copy_vram_cmd_t cmd = {
        .src_x = 30, .src_y = 20, .dst_x = 20, .dst_y = 20, .width = 2, .height = 1};
    be->vtable->copy_vram(be, &cmd);
    expect_mask = operation == COPY_SET;
  } else if (operation == NATURAL_ROLLOVER) {
    /* Each rectangle consumes one real depth ID. Cross the limit without
     * changing the counter or extending the renderer's resource limits. */
    for (u32 i = 0; i < 65535; i++)
      rectangle(be, 40, 0x0000FF00, false);
    be->vtable->flush_render(be);
    if (hw->current_depth >= 65534) return false;
  } else if (operation == RESTORE) {
    const size_t capacity = 2 * 1024 * 1024;
    u8*          saved    = malloc(capacity);
    if (!saved) return false;
    state_wrapper_t state;
    state_wrapper_init_write(&state, saved, capacity, SAVE_STATE_VERSION);
    bool         valid = be->vtable->do_state(be, &state);
    const size_t size  = state_wrapper_get_position(&state);
    rectangle(be, 20, 0x0000FF00, false);
    be->vtable->flush_render(be);
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    valid = valid && be->vtable->do_state(be, &state);
    free(saved);
    if (!valid) return false;
  } else if (operation == RESIZE) {
    const u8 original               = hw->resolution_scale;
    const u8 other                  = original == 1 ? 2 : 1;
    Error    error                  = ERROR_INIT;
    g_settings.gpu_resolution_scale = other;
    bool valid                      = gpu_hw_change_resolution_scale(hw, other, &error);
    if (valid) {
      draw(be, shape);
      be->vtable->flush_render(be);
      valid = read_color(hw, 20, 20) == 0xFF0000FF && read_color(hw, 21, 20) == 0x00FF0000;
    }
    g_settings.gpu_resolution_scale = original;
    valid                           = gpu_hw_change_resolution_scale(hw, original, &error) && valid;
    if (!valid) fprintf(stderr, "Resize preservation failed: %s\n", Error_get_description(&error));
    Error_destroy(&error);
    if (!valid) return false;
  }
  if (operation == ROLLOVER) hw->current_depth = 65534;
  draw(be, shape);
  be->vtable->flush_render(be);
  const u32    masked = read_color(hw, 20, 20), clear = read_color(hw, 21, 20);
  const GLenum error = glGetError();
  const bool passed  = masked == (expect_mask ? 0xFF0000FF : 0x00FF0000) && clear == 0x00FF0000 && error == GL_NO_ERROR;
  printf("%s scale=%u shape=%u operation=%u masked=%08X clear=%08X depth=%d gl=%04X\n", passed ? "PASS" : "FAIL",
         hw->resolution_scale, shape, operation, masked, clear, hw->current_depth, error);
  return passed;
}

static bool check_truncated_restore(gpu_hw_t* hw) {
  gpu_backend_t* be = &hw->base;
  be->vtable->clear_vram(be);
  rectangle(be, 20, 0x000000FF, true);
  const size_t capacity = 2 * 1024 * 1024;
  u8*          saved    = malloc(capacity);
  if (!saved) return false;
  state_wrapper_t state;
  state_wrapper_init_write(&state, saved, capacity, SAVE_STATE_VERSION);
  bool         valid = be->vtable->do_state(be, &state);
  const size_t size  = state_wrapper_get_position(&state);
  valid              = valid && size == sizeof(g_vram) + sizeof(g_gpu_clut);
  rectangle(be, 20, 0x0000FF00, false);
  be->vtable->flush_render(be);
  if (valid) {
    state_wrapper_init_read(&state, saved, size - 1, SAVE_STATE_VERSION);
    valid =
        !be->vtable->do_state(be, &state) && state_wrapper_has_error(&state) && read_color(hw, 20, 20) == 0x0000FF00;
    state_wrapper_init_read(&state, saved, size, SAVE_STATE_VERSION);
    valid = be->vtable->do_state(be, &state) && valid;
    draw(be, RECTANGLE);
    be->vtable->flush_render(be);
    valid = read_color(hw, 20, 20) == 0xFF0000FF && glGetError() == GL_NO_ERROR && valid;
  }
  free(saved);
  printf("%s scale=%u truncated state rejection and recovery\n", valid ? "PASS" : "FAIL", hw->resolution_scale);
  return valid;
}

int main(int argc, char** argv) {
  const u32 first_scale = argc == 2 ? (u32)atoi(argv[1]) : 1;
  const u32 last_scale  = argc == 2 ? first_scale : 2;
  if (first_scale < 1 || last_scale > 2) return 1;
  if (SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window* window =
      SDL_CreateWindow("cupid-ps1 GPU mask checks", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    fprintf(stderr, "SDL window: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(window, &wm) || wm.subsystem != SDL_SYSWM_X11) {
    fprintf(stderr, "GPU mask checks require X11\n");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  window_info_t wi;
  window_info_init(&wi);
  wi.type               = WINDOW_INFO_TYPE_XLIB;
  wi.display_connection = wm.info.x11.display;
  wi.window_handle      = (void*)(uintptr_t)wm.info.x11.window;
  wi.surface_width      = 64;
  wi.surface_height     = 64;
  wi.surface_scale      = 1;
  wi.surface_format     = GPU_TEXTURE_FORMAT_RGBA8;
  Error error           = ERROR_INIT;
  settings_init(&g_settings);
  if (!opengl_device_create(&wi, GPU_VSYNC_MODE_DISABLED, GPU_DEVICE_CREATE_NONE, &error)) {
    fprintf(stderr, "GPU device: %s\n", Error_get_description(&error));
    settings_destroy(&g_settings);
    Error_destroy(&error);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  g_settings.gpu_use_software_renderer_for_readbacks = false;
  u32 count = 0, failures = 0;
  for (u32 scale = first_scale; scale <= last_scale; scale++) {
    g_settings.gpu_resolution_scale = (u8)scale;
    gpu_backend_t* be               = gpu_backend_create_hardware_opengl(&wi, &error);
    if (!be) {
      fprintf(stderr, "GPU backend: %s\n", Error_get_description(&error));
      failures++;
      break;
    }
    const gpu_backend_set_drawing_area_cmd_t area = {.new_area = {.left = 0, .top = 0, .right = 1023, .bottom = 511}};
    be->vtable->drawing_area_changed(be, &area);
    for (u32 shape = RECTANGLE; shape <= LINE; shape++) {
      for (u32 operation = PLAIN; operation <= RESIZE; operation++) {
        count++;
        failures += !check((gpu_hw_t*)be, shape, operation);
      }
    }
    count++;
    failures += !check((gpu_hw_t*)be, RECTANGLE, NATURAL_ROLLOVER);
    count++;
    failures += !check_truncated_restore((gpu_hw_t*)be);
    gpu_backend_destroy(be);
  }
  opengl_device_destroy();
  settings_destroy(&g_settings);
  Error_destroy(&error);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("GPU mask depth: %u checks, %u failures\n", count, failures);
  return failures ? 1 : 0;
}
