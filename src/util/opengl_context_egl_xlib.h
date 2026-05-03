#ifndef CUPID_UTIL_OPENGL_CONTEXT_EGL_XLIB_H
#define CUPID_UTIL_OPENGL_CONTEXT_EGL_XLIB_H

#include "common/types.h"
#include "util/opengl_context.h"

typedef struct Error Error;

opengl_context_t* opengl_context_egl_xlib_create(window_info_t* wi, opengl_surface_handle_t* out_surface,
                                                 const opengl_context_version_t* versions, size_t version_count,
                                                 Error* err);

#endif /* CUPID_UTIL_OPENGL_CONTEXT_EGL_XLIB_H */
