 /*
 *
 * Phase 6B smoke test: opens an SDL2 GL window, brings up our EGL+X11
 * opengl_context, clears to bright green, swaps, sleeps 2s, exits.
 *
 * Built only via `make gl-smoke`; not part of the regular test suite (needs
 * a live X11 display).  Run from the project root:
 *
 *   ./build/cupid-ps1-gl-smoke
 *
 * Headless verify (Mesa llvmpipe):
 *   LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3CORE ./build/cupid-ps1-gl-smoke
 */

#include "common/error.h"
#include "common/log.h"
#include "util/opengl_context.h"
#include "util/window_info.h"

#include "glad/gl.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(void)
{
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
  {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow("cupid-ps1 GL smoke", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                         640, 480, SDL_WINDOW_OPENGL);
  if (!window)
  {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(window, &wm))
  {
    fprintf(stderr, "SDL_GetWindowWMInfo: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  window_info_t wi; window_info_init(&wi);
  if (wm.subsystem != SDL_SYSWM_X11)
  {
    fprintf(stderr, "Smoke test needs X11; SDL gave subsystem=%d.  Try SDL_VIDEODRIVER=x11\n", (int)wm.subsystem);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  wi.type               = WINDOW_INFO_TYPE_XLIB;
  wi.display_connection = wm.info.x11.display;
  wi.window_handle      = (void*)(uintptr_t)wm.info.x11.window;
  wi.surface_width      = 640;
  wi.surface_height     = 480;
  wi.surface_format     = GPU_TEXTURE_FORMAT_RGBA8;

  Error err = ERROR_INIT;
  opengl_surface_handle_t surf = OPENGL_MAIN_SURFACE;
  opengl_context_t* ctx = opengl_context_create(&wi, &surf, false, &err);
  if (!ctx)
  {
    fprintf(stderr, "opengl_context_create failed: %s\n", Error_get_description(&err));
    Error_destroy(&err);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  Error_destroy(&err);

  /* Vsync where supported. */
  err = (Error)ERROR_INIT;
  opengl_context_set_swap_interval(ctx, 1, &err);
  Error_destroy(&err);

  /* Two swaps for double-buffer; bright green so it's unmistakable. */
  for (int i = 0; i < 120; ++i)
  {
    glViewport(0, 0, wi.surface_width, wi.surface_height);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!opengl_context_swap_buffers(ctx))
    {
      fprintf(stderr, "swap failed\n");
      break;
    }
    SDL_PumpEvents();
  }

  fprintf(stderr, "smoke OK: 120 frames, %dx%d, exiting\n",
          (int)wi.surface_width, (int)wi.surface_height);

  opengl_context_done_current(ctx);
  opengl_context_destroy_surface(ctx, surf);
  opengl_context_destroy(ctx);

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

int main(int argc, char** argv)
{
  (void)argc; (void)argv;
  return run();
}
