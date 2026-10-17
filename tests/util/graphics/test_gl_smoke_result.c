/* cupid-ps1 graphics smoke result and cleanup tests. */
#include "tests/test_harness.h"
#include "util/graphics/opengl_context.h"

#include "glad/gl.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <unistd.h>

static unsigned         swaps, fail_at, pumps, clears, destroyed, surfaces_destroyed, windows_destroyed, quits;
static opengl_context_t context;

static int smoke_init(Uint32 flags)
{
  return 0;
}

static SDL_Window* smoke_window(const char* title, int x, int y, int w, int h, Uint32 flags)
{
  return (SDL_Window*)&context;
}

static SDL_bool smoke_window_info(SDL_Window* window, SDL_SysWMinfo* info)
{
  info->subsystem        = SDL_SYSWM_X11;
  info->info.x11.display = NULL;
  info->info.x11.window  = 1;
  return SDL_TRUE;
}

static void smoke_destroy_window(SDL_Window* window)
{
  windows_destroyed++;
}

static void smoke_quit(void)
{
  quits++;
}

static void smoke_pump(void)
{
  pumps++;
}

static void smoke_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {}

static void smoke_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {}

static void smoke_clear(GLbitfield mask)
{
  clears++;
}

static bool smoke_swap(opengl_context_t* ctx)
{
  return ++swaps != fail_at;
}

static bool smoke_interval(opengl_context_t* ctx, s32 interval, Error* error)
{
  return true;
}

static bool smoke_done(opengl_context_t* ctx)
{
  return true;
}

static void smoke_destroy_surface(opengl_context_t* ctx, opengl_surface_handle_t surface)
{
  surfaces_destroyed++;
}

static void smoke_destroy(opengl_context_t* ctx)
{
  destroyed++;
}

static opengl_context_t* smoke_create(window_info_t* info, opengl_surface_handle_t* surface, bool prefer_gles,
                                      Error* error)
{
  static const opengl_context_vtable_t vtable = {
      .swap_buffers      = smoke_swap,
      .set_swap_interval = smoke_interval,
      .done_current      = smoke_done,
      .destroy_surface   = smoke_destroy_surface,
      .destroy           = smoke_destroy,
  };
  context.vt = &vtable;
  return &context;
}

/* Exercise the smoke entry point with controlled SDL and context operations. */
static int smoke_entry(int argc, char** argv);
#define SDL_Init              smoke_init
#define SDL_CreateWindow      smoke_window
#define SDL_GetWindowWMInfo   smoke_window_info
#define SDL_DestroyWindow     smoke_destroy_window
#define SDL_Quit              smoke_quit
#define SDL_PumpEvents        smoke_pump
#define opengl_context_create smoke_create
#undef glViewport
#define glViewport smoke_viewport
#undef glClearColor
#define glClearColor smoke_clear_color
#undef glClear
#define glClear smoke_clear
#define main    smoke_entry
#include "test_gl_context_smoke.c"
#undef main

static void check_smoke(unsigned failure)
{
  swaps = pumps = clears = destroyed = surfaces_destroyed = windows_destroyed = quits = 0;
  fail_at                                                                             = failure;
  FILE* output                                                                        = tmpfile();
  EXPECT_NOT_NULL(output);
  fflush(stderr);
  const int saved = dup(STDERR_FILENO);
  EXPECT_TRUE(saved >= 0);
  EXPECT_EQ(dup2(fileno(output), STDERR_FILENO), STDERR_FILENO);
  const int result = smoke_entry(0, NULL);
  fflush(stderr);
  EXPECT_EQ(dup2(saved, STDERR_FILENO), STDERR_FILENO);
  close(saved);
  rewind(output);
  char         text[256] = {0};
  const size_t bytes     = fread(text, 1, sizeof(text) - 1, output);
  EXPECT_FALSE(ferror(output));
  fclose(output);
  EXPECT_TRUE(bytes > 0);
  EXPECT_EQ(result, failure ? 1 : 0);
  EXPECT_EQ(swaps, failure ? failure : 120);
  EXPECT_EQ(clears, swaps);
  EXPECT_EQ(pumps, failure ? failure - 1 : 120);
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(surfaces_destroyed, 1);
  EXPECT_EQ(windows_destroyed, 1);
  EXPECT_EQ(quits, 1);
  if (failure) {
    EXPECT_NOT_NULL(strstr(text, "swap failed"));
    EXPECT_NULL(strstr(text, "smoke OK"));
  } else {
    EXPECT_NOT_NULL(strstr(text, "smoke OK: 120 frames, 640x480, exiting"));
    EXPECT_NULL(strstr(text, "swap failed"));
  }
}

TEST(GlSmokeResult, complete_run_succeeds)
{
  check_smoke(0);
}

TEST(GlSmokeResult, first_swap_failure_fails_and_cleans_up)
{
  check_smoke(1);
}

TEST(GlSmokeResult, last_swap_failure_fails_and_cleans_up)
{
  check_smoke(120);
}
