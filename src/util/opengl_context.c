/*
 *

 * Win32, macOS, Android, XCB factory branches dropped.
 */
#include "opengl_context.h"
#include "opengl_context_egl_xlib.h"

#include "common/error.h"
#include "common/log.h"

#include "glad/gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

static void disable_broken_extensions(const char* gl_vendor, const char* gl_renderer, const char* gl_version)
{
  if (strstr(gl_vendor, "ARM"))
  {
    /* GL_{EXT,OES}_copy_image seem to be implemented on the CPU in the Mali drivers...
     * Older drivers don't implement timer queries correctly either. */
    int gl_major_version, gl_minor_version, unused_version, major_version, patch_version;
    if ((sscanf(gl_version, "OpenGL ES %d.%d v%d.r%dp%d", &gl_major_version, &gl_minor_version, &unused_version,
                &major_version, &patch_version) == 5 &&
         gl_major_version >= 3 && gl_minor_version >= 2 && major_version >= 32) ||
        (sscanf(gl_version, "OpenGL ES %d.%d v%d.g%dp%d", &gl_major_version, &gl_minor_version, &unused_version,
                &major_version, &patch_version) == 5 &&
         gl_major_version >= 3 && gl_minor_version >= 2 && major_version > 0))
    {
      VERBOSE_LOG("Newer Mali driver detected, disabling GL_{EXT,OES}_copy_image.");
      GLAD_GL_EXT_copy_image = 0;
      GLAD_GL_OES_copy_image = 0;
    }
    else
    {
      VERBOSE_LOG("Older Mali driver detected, disabling GL_{EXT,OES}_copy_image, disjoint_timer_query.");
      GLAD_GL_EXT_copy_image = 0;
      GLAD_GL_OES_copy_image = 0;
      GLAD_GL_EXT_disjoint_timer_query = 0;
    }
  }
  else if (strstr(gl_vendor, "Qualcomm") && strstr(gl_renderer, "Adreno"))
  {
    /* Framebuffer fetch appears to be broken in drivers ?? >= 464 < 502. */
    int gl_major_version = 0, gl_minor_version = 0, major_version = 0;
    if (sscanf(gl_version, "OpenGL ES %d.%d V@%d", &gl_major_version, &gl_minor_version, &major_version) == 3 &&
        gl_major_version >= 3 && gl_minor_version >= 2 && major_version < 502)
    {
      VERBOSE_LOG("Disabling GL_EXT_shader_framebuffer_fetch on Adreno version %d", major_version);
      GLAD_GL_EXT_shader_framebuffer_fetch = 0;
      GLAD_GL_ARM_shader_framebuffer_fetch = 0;
    }
    else
    {
      VERBOSE_LOG("Keeping GL_EXT_shader_framebuffer_fetch on Adreno version %d", major_version);
    }
  }
  else if (strstr(gl_vendor, "Imagination Technologies") && strstr(gl_renderer, "PowerVR"))
  {
    GLAD_GL_EXT_shader_framebuffer_fetch = 0;
    GLAD_GL_ARM_shader_framebuffer_fetch = 0;
    VERBOSE_LOG("Disabling GL_EXT_shader_framebuffer_fetch on PowerVR driver.");
  }

  /* If we're missing GLES 3.2, but have OES_draw_elements_base_vertex, redirect the function pointers. */
  if (!glad_glDrawElementsBaseVertex && GLAD_GL_OES_draw_elements_base_vertex && !GLAD_GL_ES_VERSION_3_2)
  {
    glad_glDrawElementsBaseVertex = glad_glDrawElementsBaseVertexOES;
    glad_glDrawRangeElementsBaseVertex = glad_glDrawRangeElementsBaseVertexOES;
    glad_glDrawElementsInstancedBaseVertex = glad_glDrawElementsInstancedBaseVertexOES;
  }
}

/* Trampoline for glad's loader callback, which is plain C function pointer. */
static opengl_context_t* s_context_being_created;
static GLADapiproc context_being_created_get_proc(const char* name)
{
  return (GLADapiproc)opengl_context_get_proc_address(s_context_being_created, name);
}

opengl_context_t* opengl_context_create(window_info_t* wi, opengl_surface_handle_t* out_surface,
                                        bool prefer_gles_context, Error* err)
{
  /* Linux Core-only first pass.  GLES versions kept in the table for future
   * GLES path; promoted to front when prefer_gles_context. */
  static const opengl_context_version_t vlist[] = {
    {OPENGL_CONTEXT_PROFILE_CORE, 4, 6}, {OPENGL_CONTEXT_PROFILE_CORE, 4, 5},
    {OPENGL_CONTEXT_PROFILE_CORE, 4, 4}, {OPENGL_CONTEXT_PROFILE_CORE, 4, 3},
    {OPENGL_CONTEXT_PROFILE_CORE, 4, 2}, {OPENGL_CONTEXT_PROFILE_CORE, 4, 1},
    {OPENGL_CONTEXT_PROFILE_CORE, 4, 0}, {OPENGL_CONTEXT_PROFILE_CORE, 3, 3},
    {OPENGL_CONTEXT_PROFILE_CORE, 3, 2}, {OPENGL_CONTEXT_PROFILE_ES,   3, 2},
    {OPENGL_CONTEXT_PROFILE_ES,   3, 1}, {OPENGL_CONTEXT_PROFILE_ES,   3, 0},
    {OPENGL_CONTEXT_PROFILE_CORE, 3, 1}, {OPENGL_CONTEXT_PROFILE_CORE, 3, 0},
  };
  enum { VLIST_COUNT = sizeof(vlist) / sizeof(vlist[0]) };

  opengl_context_version_t versions_to_try[VLIST_COUNT];
  size_t version_count = 0;
  if (prefer_gles_context)
  {
    for (size_t i = 0; i < VLIST_COUNT; ++i)
      if (vlist[i].profile == OPENGL_CONTEXT_PROFILE_ES) versions_to_try[version_count++] = vlist[i];
    for (size_t i = 0; i < VLIST_COUNT; ++i)
      if (vlist[i].profile != OPENGL_CONTEXT_PROFILE_ES) versions_to_try[version_count++] = vlist[i];
  }
  else
  {
    for (size_t i = 0; i < VLIST_COUNT; ++i) versions_to_try[version_count++] = vlist[i];
  }

  opengl_context_t* context = NULL;
  if (wi->type == WINDOW_INFO_TYPE_XLIB || wi->type == WINDOW_INFO_TYPE_SURFACELESS)
    context = opengl_context_egl_xlib_create(wi, out_surface, versions_to_try, version_count, err);
  else
    Error_set_string(err, "Unsupported window_info type for OpenGL context (Linux EGL+X11 only).");

  if (!context)
    return NULL;

  INFO_LOG(opengl_context_is_gles(context) ? "Created an OpenGL ES context" : "Created an OpenGL context");

  /* TODO: Not thread-safe. */
  s_context_being_created = context;

  if (!opengl_context_is_gles(context))
  {
    if (!gladLoadGL(context_being_created_get_proc))
    {
      Error_set_string(err, "Failed to load GL functions for GLAD");
      opengl_context_destroy(context);
      return NULL;
    }
  }
  else
  {
    if (!gladLoadGLES2(context_being_created_get_proc))
    {
      Error_set_string(err, "Failed to load GLES functions for GLAD");
      opengl_context_destroy(context);
      return NULL;
    }
  }

  const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
  const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
  const char* gl_version  = (const char*)glGetString(GL_VERSION);
  const char* gl_glsl_v   = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
  INFO_LOG("GL_VENDOR: %s",                  gl_vendor   ? gl_vendor   : "(null)");
  INFO_LOG("GL_RENDERER: %s",                gl_renderer ? gl_renderer : "(null)");
  INFO_LOG("GL_VERSION: %s",                 gl_version  ? gl_version  : "(null)");
  INFO_LOG("GL_SHADING_LANGUAGE_VERSION: %s",gl_glsl_v   ? gl_glsl_v   : "(null)");

  if (gl_vendor && gl_renderer && gl_version)
    disable_broken_extensions(gl_vendor, gl_renderer, gl_version);

  return context;
}
