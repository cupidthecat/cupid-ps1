/*
 * Win32 (WGL), macOS (AGL), Android, XCB paths dropped.  Single-impl collapse
 * keeps vtable on the base struct so future Wayland support can share the EGL
 * parent without changing call sites.
 */

#ifndef CUPID_UTIL_OPENGL_CONTEXT_H
#define CUPID_UTIL_OPENGL_CONTEXT_H

#include "common/types.h"
#include "util/window_info.h"

typedef struct Error Error;

typedef enum {
  OPENGL_CONTEXT_PROFILE_NONE = 0,
  OPENGL_CONTEXT_PROFILE_CORE,
  OPENGL_CONTEXT_PROFILE_ES,
} opengl_context_profile_t;

typedef struct {
  opengl_context_profile_t profile;
  int                      major_version;
  int                      minor_version;
} opengl_context_version_t;

/* Opaque to most callers; sub-impl uses the internal layout in the .c. */
typedef struct opengl_context opengl_context_t;

/* SurfaceHandle is an opaque pointer-typed token.  EGL backend casts EGLSurface */
typedef void* opengl_surface_handle_t;
#define OPENGL_MAIN_SURFACE ((opengl_surface_handle_t)NULL)

typedef struct opengl_context_vtable {
  void  (*destroy)              (opengl_context_t* ctx);
  void* (*get_proc_address)     (opengl_context_t* ctx, const char* name);
  opengl_surface_handle_t (*create_surface) (opengl_context_t* ctx, window_info_t* wi, Error* err);
  void  (*destroy_surface)      (opengl_context_t* ctx, opengl_surface_handle_t handle);
  void  (*resize_surface)       (opengl_context_t* ctx, window_info_t* wi, opengl_surface_handle_t handle);
  bool  (*swap_buffers)         (opengl_context_t* ctx);
  bool  (*is_current)           (const opengl_context_t* ctx);
  bool  (*make_current)         (opengl_context_t* ctx, opengl_surface_handle_t surface, Error* err);
  bool  (*done_current)         (opengl_context_t* ctx);
  bool  (*supports_negative_swap_interval)(const opengl_context_t* ctx);
  bool  (*set_swap_interval)    (opengl_context_t* ctx, s32 interval, Error* err);
  /* Returns owning ctx; surface set in *out_surface; caller destroys both. */
  opengl_context_t* (*create_shared_context)(opengl_context_t* ctx, window_info_t* wi,
                                             opengl_surface_handle_t* out_surface, Error* err);
} opengl_context_vtable_t;

struct opengl_context {
  const opengl_context_vtable_t* vt;
  opengl_context_version_t       version;
};

/* Tries each version in versions[count] until one creates.  Loads GLAD-GL after
 * the context is current.  Sets *out_surface (may be OPENGL_MAIN_SURFACE if
 * surfaceless) on success.  prefer_gles_context promotes the ES versions ahead
 * of Core; on cupid-ps1 this is currently a no-op until GLES paths land. */
opengl_context_t* opengl_context_create(window_info_t* wi, opengl_surface_handle_t* out_surface,
                                        bool prefer_gles_context, Error* err);

ALWAYS_INLINE bool opengl_context_is_gles(const opengl_context_t* ctx)
{
  return ctx->version.profile == OPENGL_CONTEXT_PROFILE_ES;
}

/* Convenience dispatchers (one-line forwarders to vtable). */
ALWAYS_INLINE void  opengl_context_destroy(opengl_context_t* ctx) { if (ctx) ctx->vt->destroy(ctx); }
ALWAYS_INLINE void* opengl_context_get_proc_address(opengl_context_t* ctx, const char* name)
{ return ctx->vt->get_proc_address(ctx, name); }
ALWAYS_INLINE opengl_surface_handle_t opengl_context_create_surface(opengl_context_t* ctx, window_info_t* wi, Error* err)
{ return ctx->vt->create_surface(ctx, wi, err); }
ALWAYS_INLINE void opengl_context_destroy_surface(opengl_context_t* ctx, opengl_surface_handle_t h)
{ ctx->vt->destroy_surface(ctx, h); }
ALWAYS_INLINE void opengl_context_resize_surface(opengl_context_t* ctx, window_info_t* wi, opengl_surface_handle_t h)
{ ctx->vt->resize_surface(ctx, wi, h); }
ALWAYS_INLINE bool opengl_context_swap_buffers(opengl_context_t* ctx) { return ctx->vt->swap_buffers(ctx); }
ALWAYS_INLINE bool opengl_context_is_current(const opengl_context_t* ctx) { return ctx->vt->is_current(ctx); }
ALWAYS_INLINE bool opengl_context_make_current(opengl_context_t* ctx, opengl_surface_handle_t s, Error* err)
{ return ctx->vt->make_current(ctx, s, err); }
ALWAYS_INLINE bool opengl_context_done_current(opengl_context_t* ctx) { return ctx->vt->done_current(ctx); }
ALWAYS_INLINE bool opengl_context_supports_negative_swap_interval(const opengl_context_t* ctx)
{ return ctx->vt->supports_negative_swap_interval(ctx); }
ALWAYS_INLINE bool opengl_context_set_swap_interval(opengl_context_t* ctx, s32 i, Error* err)
{ return ctx->vt->set_swap_interval(ctx, i, err); }
ALWAYS_INLINE opengl_context_t* opengl_context_create_shared(opengl_context_t* ctx, window_info_t* wi,
                                                             opengl_surface_handle_t* s, Error* err)
{ return ctx->vt->create_shared_context(ctx, wi, s, err); }

#endif /* CUPID_UTIL_OPENGL_CONTEXT_H */
