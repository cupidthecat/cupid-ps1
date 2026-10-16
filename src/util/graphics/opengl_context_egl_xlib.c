#include "opengl_context_egl_xlib.h"
#include "opengl_context_egl.h"

#include "common/error.h"
#include "common/log.h"

#include <stdlib.h>

LOG_CHANNEL(GPUDevice);

static EGLDisplay xlib_get_platform_display(opengl_context_egl_t* self, const window_info_t* wi, Error* err)
{
  EGLDisplay dpy = opengl_context_egl_try_get_platform_display(self, wi->display_connection,
                                                               EGL_PLATFORM_X11_KHR, "EGL_EXT_platform_x11");
  if (dpy == EGL_NO_DISPLAY)
    dpy = opengl_context_egl_get_fallback_display(self, wi->display_connection, err);
  return dpy;
}

static EGLSurface xlib_create_platform_surface(opengl_context_egl_t* self, EGLConfig config,
                                               const window_info_t* wi, Error* err)
{
  /* This is hideous.. the EXT version requires a pointer to the window, whereas the base
   * version requires the window itself, casted to void*. */
  void* win = wi->window_handle;
  EGLSurface surface = opengl_context_egl_try_create_platform_surface(self, config, &win, err);
  if (surface == EGL_NO_SURFACE)
    surface = opengl_context_egl_create_fallback_surface(self, config, wi->window_handle, err);
  return surface;
}

static void xlib_destroy_platform_surface(opengl_context_egl_t* self, EGLSurface surface)
{
  eglDestroySurface(self->m_display, surface);
}

static opengl_context_t* xlib_create_shared_context(opengl_context_t* self_ctx, window_info_t* wi,
                                                    opengl_surface_handle_t* out_surface, Error* err);

/* Platform vtable for xlib.  Set by the factory before initialize. */
static const opengl_context_egl_platform_vtable_t s_xlib_plat = {
   .get_platform_display     = xlib_get_platform_display,
  .create_platform_surface  = xlib_create_platform_surface,
  .destroy_platform_surface = xlib_destroy_platform_surface,
  .create_shared_context    = xlib_create_shared_context, 
};

opengl_context_t* opengl_context_egl_xlib_create(window_info_t* wi, opengl_surface_handle_t* out_surface,
                                                 const opengl_context_version_t* versions, size_t version_count,
                                                 Error* err)
{
  opengl_context_egl_t* self = opengl_context_egl_alloc();
  if (!self) { Error_set_string(err, "alloc failed"); return NULL; }
  self->plat = &s_xlib_plat;

  if (!opengl_context_egl_initialize(self, wi, out_surface, versions, version_count, err))
  {
    /* destroy via vtable to release EGL refcount cleanly */
    self->base.vt->destroy(&self->base);
    return NULL;
  }
  return &self->base;
}

static opengl_context_t* xlib_create_shared_context(opengl_context_t* self_ctx, window_info_t* wi,
                                                    opengl_surface_handle_t* out_surface, Error* err)
{
  opengl_context_egl_t* parent = (opengl_context_egl_t*)self_ctx;
  opengl_context_egl_t* shared = opengl_context_egl_alloc();
  if (!shared) { Error_set_string(err, "alloc failed"); return NULL; }
  shared->plat       = &s_xlib_plat;
  shared->m_display  = parent->m_display;     /* clone display for the xlib path */

  if (!opengl_context_egl_create_context_and_surface(shared, wi, out_surface, &parent->base.version,
                                                     parent->m_context, false, err))
  {
    shared->base.vt->destroy(&shared->base);
    return NULL;
  }
  return &shared->base;
}
