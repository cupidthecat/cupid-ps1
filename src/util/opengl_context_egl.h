/*
 *

 * Linux X11 (xlib) platform is wired up - Wayland/XCB/surfaceless-ext-base
 * deferred.  This header is internal to util/ and pulled in by the xlib
 * subclass file.
 */
#ifndef CUPID_UTIL_OPENGL_CONTEXT_EGL_H
#define CUPID_UTIL_OPENGL_CONTEXT_EGL_H

#include "common/types.h"
#include "util/opengl_context.h"

#include "glad/egl.h"

typedef struct opengl_context_egl opengl_context_egl_t;

/* Platform vfuncs the xlib (and future wayland) subclass overrides. */
typedef struct opengl_context_egl_platform_vtable {
  EGLDisplay (*get_platform_display)   (opengl_context_egl_t* self, const window_info_t* wi, Error* err);
  EGLSurface (*create_platform_surface)(opengl_context_egl_t* self, EGLConfig config,
                                        const window_info_t* wi, Error* err);
  void       (*destroy_platform_surface)(opengl_context_egl_t* self, EGLSurface surface);
  /* CreateSharedContext is platform-specific only because xlib's m_display is
   * shared via the subclass.  Pass-through to the EGL helper otherwise. */
  opengl_context_t* (*create_shared_context)(opengl_context_t* self_ctx, window_info_t* wi,
                                             opengl_surface_handle_t* out_surface, Error* err);
} opengl_context_egl_platform_vtable_t;

struct opengl_context_egl {
  opengl_context_t                              base;
  const opengl_context_egl_platform_vtable_t*   plat;

  EGLDisplay m_display;
  EGLContext m_context;
  EGLSurface m_current_surface;

  EGLConfig  m_config;

  EGLSurface m_pbuffer_surface;

  bool m_use_ext_platform_base;
  bool m_supports_negative_swap_interval;
};

/* Allocates a zeroed EGL context shell, sets the base vtable.  Subclass fills
 * in plat and version, then calls opengl_context_egl_initialize(). */
opengl_context_egl_t* opengl_context_egl_alloc(void);

 /* Initialize: load EGL, init display, walk versions trying CreateContextAndSurface.
 * Sets *out_surface and ctx->base.version on success. */
bool opengl_context_egl_initialize(opengl_context_egl_t* self, window_info_t* wi,
                                   opengl_surface_handle_t* out_surface,
                                   const opengl_context_version_t* versions, size_t version_count,
                                   Error* err);

 /* Helpers used by the platform subclass's get_platform_display /
 * create_platform_surface overrides. */
EGLDisplay opengl_context_egl_try_get_platform_display(opengl_context_egl_t* self, void* display,
                                                       EGLenum platform, const char* platform_ext);
EGLSurface opengl_context_egl_try_create_platform_surface(opengl_context_egl_t* self, EGLConfig config,
                                                          void* window, Error* err);
EGLDisplay opengl_context_egl_get_fallback_display(opengl_context_egl_t* self, void* display, Error* err);
EGLSurface opengl_context_egl_create_fallback_surface(opengl_context_egl_t* self, EGLConfig config,
                                                      void* window, Error* err);

bool opengl_context_egl_supports_surfaceless(const opengl_context_egl_t* self);

/* Used by xlib::CreateSharedContext to clone display + create context+surface. */
bool opengl_context_egl_create_context_and_surface(opengl_context_egl_t* self, window_info_t* wi,
                                                   opengl_surface_handle_t* out_surface,
                                                   const opengl_context_version_t* version,
                                                   EGLContext share_context, bool make_current, Error* err);

#endif /* CUPID_UTIL_OPENGL_CONTEXT_EGL_H */
