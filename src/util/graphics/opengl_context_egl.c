/*
 *

 * subclass lives in opengl_context_egl_xlib.c.  Wayland/XCB/surfaceless-MESA
 * deferred.
 */
#include "opengl_context_egl.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"

#include "glad/gl.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

static dynamic_library_t s_egl_library;
static atomic_uint       s_egl_refcount = 0;

static bool load_egl(void)
{
  if (atomic_fetch_add_explicit(&s_egl_refcount, 1u, memory_order_acq_rel) == 0)
  {
    DebugAssert(!dynamic_library_is_open(&s_egl_library));

    small_string_t libname; small_string_init(&libname);
    dynamic_library_get_versioned_filename(&libname, "libEGL", -1, -1, -1);
    INFO_LOG("Loading EGL from %s...", small_string_c_str(&libname));

    Error e = ERROR_INIT;
    if (!dynamic_library_open(&s_egl_library, small_string_c_str(&libname), &e))
    {
      small_string_clear(&libname);
      dynamic_library_get_versioned_filename(&libname, "libEGL", 1, -1, -1);
      INFO_LOG("Loading EGL from %s...", small_string_c_str(&libname));
      Error_destroy(&e);
      e = (Error)ERROR_INIT;
      if (!dynamic_library_open(&s_egl_library, small_string_c_str(&libname), &e))
        ERROR_LOG("Failed to load EGL: %s", Error_get_description(&e));
    }
    Error_destroy(&e);
    small_string_destroy(&libname);
  }

  return dynamic_library_is_open(&s_egl_library);
}

static void unload_egl(void)
{
  DebugAssert(atomic_load_explicit(&s_egl_refcount, memory_order_acquire) > 0);
  if (atomic_fetch_sub_explicit(&s_egl_refcount, 1u, memory_order_acq_rel) == 1)
  {
    INFO_LOG("Unloading EGL.");
    dynamic_library_close(&s_egl_library);
  }
}

static GLADapiproc egl_get_sym_for_glad(const char* name)
{
  return (GLADapiproc)dynamic_library_get_symbol_address(&s_egl_library, name);
}

static bool load_glad_egl(EGLDisplay display, Error* err)
{
  const int version = gladLoadEGL(display, egl_get_sym_for_glad);
  if (version == 0)
  {
    Error_set_string(err, "Loading GLAD EGL functions failed");
    return false;
  }
  DEV_LOG("GLAD EGL Version: %d.%d", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));
  return true;
}

static const char* gpu_texture_format_name_local(gpu_texture_format_t f)
{
  switch (f)
  {
    case GPU_TEXTURE_FORMAT_RGBA8:    return "RGBA8";
    case GPU_TEXTURE_FORMAT_BGRA8:    return "BGRA8";
    case GPU_TEXTURE_FORMAT_RGB565:   return "RGB565";
    case GPU_TEXTURE_FORMAT_RGBA5551: return "RGBA5551";
    case GPU_TEXTURE_FORMAT_UNKNOWN:  return "Unknown";
    default:                          return "?";
  }
}

ALWAYS_INLINE bool window_info_is_surfaceless_inline(const window_info_t* wi)
{
  return wi->type == WINDOW_INFO_TYPE_SURFACELESS;
}

static void  egl_destroy             (opengl_context_t* ctx);
static void* egl_get_proc_address    (opengl_context_t* ctx, const char* name);
static opengl_surface_handle_t egl_create_surface  (opengl_context_t* ctx, window_info_t* wi, Error* err);
static void  egl_destroy_surface     (opengl_context_t* ctx, opengl_surface_handle_t handle);
static void  egl_resize_surface      (opengl_context_t* ctx, window_info_t* wi, opengl_surface_handle_t handle);
static bool  egl_swap_buffers        (opengl_context_t* ctx);
static bool  egl_is_current          (const opengl_context_t* ctx);
static bool  egl_make_current        (opengl_context_t* ctx, opengl_surface_handle_t s, Error* err);
static bool  egl_done_current        (opengl_context_t* ctx);
static bool  egl_supports_negative_swap_interval(const opengl_context_t* ctx);
static bool  egl_set_swap_interval   (opengl_context_t* ctx, s32 interval, Error* err);
static opengl_context_t* egl_create_shared_context(opengl_context_t* ctx, window_info_t* wi,
                                                   opengl_surface_handle_t* out_surface, Error* err);

static const opengl_context_vtable_t s_egl_vtable = {
   .destroy                          = egl_destroy,
  .get_proc_address                 = egl_get_proc_address,
  .create_surface                   = egl_create_surface,
  .destroy_surface                  = egl_destroy_surface,
  .resize_surface                   = egl_resize_surface,
  .swap_buffers                     = egl_swap_buffers,
  .is_current                       = egl_is_current,
  .make_current                     = egl_make_current,
  .done_current                     = egl_done_current,
  .supports_negative_swap_interval  = egl_supports_negative_swap_interval,
  .set_swap_interval                = egl_set_swap_interval,
  .create_shared_context            = egl_create_shared_context, 
};

opengl_context_egl_t* opengl_context_egl_alloc(void)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)calloc(1, sizeof(*self));
  if (!self) return NULL;
  self->base.vt              = &s_egl_vtable;
  self->m_display            = EGL_NO_DISPLAY;
  self->m_context            = EGL_NO_CONTEXT;
  self->m_current_surface    = EGL_NO_SURFACE;
  self->m_pbuffer_surface    = EGL_NO_SURFACE;
  load_egl();
  return self;
}

static void egl_destroy(opengl_context_t* ctx)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;

  if (self->m_context != EGL_NO_CONTEXT && eglGetCurrentContext() == self->m_context)
    eglMakeCurrent(self->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (self->m_pbuffer_surface != EGL_NO_SURFACE)
    eglDestroySurface(self->m_display, self->m_pbuffer_surface);

  if (self->m_context != EGL_NO_CONTEXT)
    eglDestroyContext(self->m_display, self->m_context);

  unload_egl();
  free(self);
}

EGLDisplay opengl_context_egl_try_get_platform_display(opengl_context_egl_t* self, void* display,
                                                       EGLenum platform, const char* platform_ext)
{
  const char* extensions_str = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!extensions_str)
  {
    ERROR_LOG("No extensions supported.");
    return EGL_NO_DISPLAY;
  }

  EGLDisplay dpy = EGL_NO_DISPLAY;
  if (platform_ext && strstr(extensions_str, platform_ext))
  {
    DEV_LOG("Using EGL platform %s.", platform_ext);

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display_ext)
    {
      dpy = get_platform_display_ext(platform, display, NULL);
      self->m_use_ext_platform_base = (dpy != EGL_NO_DISPLAY);
      if (!self->m_use_ext_platform_base)
      {
        const EGLint err = eglGetError();
        ERROR_LOG("eglGetPlatformDisplayEXT() failed: %d (0x%X)", (int)err, (unsigned)err);
      }
    }
    else
    {
      WARNING_LOG("eglGetPlatformDisplayEXT() was not found");
    }
  }
  else
  {
    WARNING_LOG("%s is not supported.", platform_ext ? platform_ext : "(null)");
  }

  return dpy;
}

EGLSurface opengl_context_egl_try_create_platform_surface(opengl_context_egl_t* self, EGLConfig config,
                                                          void* window, Error* err)
{
  EGLSurface surface = EGL_NO_SURFACE;
  if (self->m_use_ext_platform_base)
  {
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface_ext =
      (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    if (create_platform_window_surface_ext)
    {
      surface = create_platform_window_surface_ext(self->m_display, config, window, NULL);
      if (surface == EGL_NO_SURFACE)
      {
        const EGLint err_code = eglGetError();
        Error_set_string_fmt(err, "eglCreatePlatformWindowSurfaceEXT() failed: %d (0x%X)",
                             (int)err_code, (unsigned)err_code);
      }
    }
    else
    {
      ERROR_LOG("eglCreatePlatformWindowSurfaceEXT() not found");
    }
  }
  return surface;
}

EGLDisplay opengl_context_egl_get_fallback_display(opengl_context_egl_t* self, void* display, Error* err)
{
  (void)self;
  WARNING_LOG("Using fallback eglGetDisplay() path.");
  EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)display);
  if (dpy == EGL_NO_DISPLAY)
  {
    const EGLint e = eglGetError();
    Error_set_string_fmt(err, "eglGetDisplay() failed: %d (0x%X)", (int)e, (unsigned)e);
  }
  return dpy;
}

EGLSurface opengl_context_egl_create_fallback_surface(opengl_context_egl_t* self, EGLConfig config,
                                                      void* win, Error* err)
{
  WARNING_LOG("Using fallback eglCreateWindowSurface() path.");
  EGLSurface surface = eglCreateWindowSurface(self->m_display, config, (EGLNativeWindowType)win, NULL);
  if (surface == EGL_NO_SURFACE)
  {
    const EGLint e = eglGetError();
    Error_set_string_fmt(err, "eglCreateWindowSurface() failed: %d (0x%X)", (int)e, (unsigned)e);
  }
  return surface;
}

bool opengl_context_egl_supports_surfaceless(const opengl_context_egl_t* self)
{
  const bool gles = opengl_context_is_gles(&self->base);
  return (!gles || GLAD_GL_OES_surfaceless_context) && GLAD_EGL_KHR_surfaceless_context;
}

static EGLSurface egl_get_pbuffer_surface(opengl_context_egl_t* self, Error* err)
{
  if (self->m_pbuffer_surface != EGL_NO_SURFACE)
    return self->m_pbuffer_surface;

  EGLint attrib_list[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
  self->m_pbuffer_surface = eglCreatePbufferSurface(self->m_display, self->m_config, attrib_list);
  if (self->m_pbuffer_surface == EGL_NO_SURFACE)
  {
    const EGLint e = eglGetError();
    if (err) Error_set_string_fmt(err, "eglCreatePbufferSurface() failed: %d", (int)e);
    else     ERROR_LOG("eglCreatePbufferSurface() failed: %d", (int)e);
    return EGL_NO_SURFACE;
  }
  DEV_LOG("Created pbuffer surface");
  return self->m_pbuffer_surface;
}

static EGLSurface egl_get_surfaceless_surface(opengl_context_egl_t* self)
{
  return opengl_context_egl_supports_surfaceless(self) ? EGL_NO_SURFACE : egl_get_pbuffer_surface(self, NULL);
}

static bool egl_check_config_surface_format(opengl_context_egl_t* self, EGLConfig config, gpu_texture_format_t fmt)
{
  int red_size, green_size, blue_size, alpha_size;
  if (!eglGetConfigAttrib(self->m_display, config, EGL_RED_SIZE,   &red_size)   ||
      !eglGetConfigAttrib(self->m_display, config, EGL_GREEN_SIZE, &green_size) ||
      !eglGetConfigAttrib(self->m_display, config, EGL_BLUE_SIZE,  &blue_size)  ||
      !eglGetConfigAttrib(self->m_display, config, EGL_ALPHA_SIZE, &alpha_size))
    return false;

  switch (fmt)
  {
    case GPU_TEXTURE_FORMAT_RGBA8:    return red_size == 8 && green_size == 8 && blue_size == 8 && alpha_size == 8;
    case GPU_TEXTURE_FORMAT_RGB565:   return red_size == 5 && green_size == 6 && blue_size == 5;
    case GPU_TEXTURE_FORMAT_RGBA5551: return red_size == 5 && green_size == 5 && blue_size == 5 && alpha_size == 1;
    case GPU_TEXTURE_FORMAT_UNKNOWN:  return true;
    default:                          return false;
  }
}

static void egl_update_window_info_size(opengl_context_egl_t* self, window_info_t* wi, EGLSurface surface)
{
  EGLint w, h;
  if (eglQuerySurface(self->m_display, surface, EGL_WIDTH, &w) &&
      eglQuerySurface(self->m_display, surface, EGL_HEIGHT, &h))
  {
    wi->surface_width  = (u16)w;
    wi->surface_height = (u16)h;
  }
  else
  {
    ERROR_LOG("eglQuerySurface() failed: 0x%X", (unsigned)eglGetError());
  }

  int red_size = 0, green_size = 0, blue_size = 0, alpha_size = 0;
  eglGetConfigAttrib(self->m_display, self->m_config, EGL_RED_SIZE,   &red_size);
  eglGetConfigAttrib(self->m_display, self->m_config, EGL_GREEN_SIZE, &green_size);
  eglGetConfigAttrib(self->m_display, self->m_config, EGL_BLUE_SIZE,  &blue_size);
  eglGetConfigAttrib(self->m_display, self->m_config, EGL_ALPHA_SIZE, &alpha_size);

  if (red_size == 5 && green_size == 6 && blue_size == 5)
    wi->surface_format = GPU_TEXTURE_FORMAT_RGB565;
  else if (red_size == 5 && green_size == 5 && blue_size == 5 && alpha_size == 1)
    wi->surface_format = GPU_TEXTURE_FORMAT_RGBA5551;
  else if (red_size == 8 && green_size == 8 && blue_size == 8 && alpha_size == 8)
    wi->surface_format = GPU_TEXTURE_FORMAT_RGBA8;
  else
  {
    ERROR_LOG("Unknown surface format: R=%d, G=%d, B=%d, A=%d", red_size, green_size, blue_size, alpha_size);
    wi->surface_format = GPU_TEXTURE_FORMAT_RGBA8;
  }
}

static bool egl_create_context_internal(opengl_context_egl_t* self, bool surfaceless,
                                        gpu_texture_format_t surface_format,
                                        const opengl_context_version_t* version,
                                        EGLContext share_context, Error* err)
{
  DEV_LOG("Trying version %d.%d (%s)", version->major_version, version->minor_version,
          version->profile == OPENGL_CONTEXT_PROFILE_ES   ? "ES"
        : version->profile == OPENGL_CONTEXT_PROFILE_CORE ? "Core" : "None");

  int surface_attribs[16];
  int n = 0;
  surface_attribs[n++] = EGL_RENDERABLE_TYPE;
  surface_attribs[n++] = (version->profile == OPENGL_CONTEXT_PROFILE_ES) ?
                           ((version->major_version >= 3) ? EGL_OPENGL_ES3_BIT
                          : ((version->major_version == 2) ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT)) :
                           EGL_OPENGL_BIT;
  surface_attribs[n++] = EGL_SURFACE_TYPE;
  surface_attribs[n++] = surfaceless ? 0 : EGL_WINDOW_BIT;

  if (surface_format == GPU_TEXTURE_FORMAT_UNKNOWN)
    surface_format = GPU_TEXTURE_FORMAT_RGBA8;

  switch (surface_format)
  {
    case GPU_TEXTURE_FORMAT_RGBA8:
      surface_attribs[n++] = EGL_RED_SIZE;   surface_attribs[n++] = 8;
      surface_attribs[n++] = EGL_GREEN_SIZE; surface_attribs[n++] = 8;
      surface_attribs[n++] = EGL_BLUE_SIZE;  surface_attribs[n++] = 8;
      surface_attribs[n++] = EGL_ALPHA_SIZE; surface_attribs[n++] = 8;
      break;
    case GPU_TEXTURE_FORMAT_RGB565:
      surface_attribs[n++] = EGL_RED_SIZE;   surface_attribs[n++] = 5;
      surface_attribs[n++] = EGL_GREEN_SIZE; surface_attribs[n++] = 6;
      surface_attribs[n++] = EGL_BLUE_SIZE;  surface_attribs[n++] = 5;
      break;
    default:
      Error_set_string_fmt(err, "Unsupported texture format %s", gpu_texture_format_name_local(surface_format));
      return false;
  }

  surface_attribs[n++] = EGL_NONE;
  surface_attribs[n++] = 0;

  EGLint num_configs = 0;
  if (!eglChooseConfig(self->m_display, surface_attribs, NULL, 0, &num_configs) || num_configs == 0)
  {
    Error_set_string_fmt(err, "eglChooseConfig() failed: 0x%x", (unsigned)eglGetError());
    return false;
  }

  enum { MAX_EGL_CONFIGS = 64 };
  EGLConfig configs[MAX_EGL_CONFIGS];
  if (num_configs > MAX_EGL_CONFIGS) num_configs = MAX_EGL_CONFIGS;
  if (!eglChooseConfig(self->m_display, surface_attribs, configs, num_configs, &num_configs))
  {
    Error_set_string_fmt(err, "eglChooseConfig() failed: 0x%x", (unsigned)eglGetError());
    return false;
  }

  EGLConfig chosen = configs[0];
  bool found_match = false;
  for (EGLint i = 0; i < num_configs; ++i)
  {
    if (egl_check_config_surface_format(self, configs[i], surface_format))
    {
      chosen = configs[i];
      found_match = true;
      break;
    }
  }
  if (!found_match)
    WARNING_LOG("No EGL configs matched exactly, using first.");

  int attribs[8];
  int na = 0;
  if (version->profile != OPENGL_CONTEXT_PROFILE_NONE)
  {
    attribs[na++] = EGL_CONTEXT_MAJOR_VERSION; attribs[na++] = version->major_version;
    attribs[na++] = EGL_CONTEXT_MINOR_VERSION; attribs[na++] = version->minor_version;
  }
  attribs[na++] = EGL_NONE;
  attribs[na++] = 0;

  if (!eglBindAPI(version->profile == OPENGL_CONTEXT_PROFILE_ES ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
  {
    Error_set_string_fmt(err, "eglBindAPI(%s) failed",
                         version->profile == OPENGL_CONTEXT_PROFILE_ES ? "EGL_OPENGL_ES_API" : "EGL_OPENGL_API");
    return false;
  }

  self->m_context = eglCreateContext(self->m_display, chosen, share_context, attribs);
  if (self->m_context == EGL_NO_CONTEXT)
  {
    Error_set_string_fmt(err, "eglCreateContext() failed: 0x%x", (unsigned)eglGetError());
    return false;
  }

  INFO_LOG("Got version %d.%d (%s)", version->major_version, version->minor_version,
           version->profile == OPENGL_CONTEXT_PROFILE_ES   ? "ES"
         : version->profile == OPENGL_CONTEXT_PROFILE_CORE ? "Core" : "None");

  EGLint min_swap = 0, max_swap = 0;
  self->m_supports_negative_swap_interval = false;
  if (eglGetConfigAttrib(self->m_display, chosen, EGL_MIN_SWAP_INTERVAL, &min_swap) &&
      eglGetConfigAttrib(self->m_display, chosen, EGL_MAX_SWAP_INTERVAL, &max_swap))
  {
    VERBOSE_LOG("EGL_MIN_SWAP_INTERVAL = %d", (int)min_swap);
    VERBOSE_LOG("EGL_MAX_SWAP_INTERVAL = %d", (int)max_swap);
    self->m_supports_negative_swap_interval = (min_swap <= -1);
  }
  INFO_LOG("Negative swap interval/tear-control is %ssupported",
           self->m_supports_negative_swap_interval ? "" : "NOT ");

  self->m_config       = chosen;
  self->base.version   = *version;
  return true;
}

bool opengl_context_egl_create_context_and_surface(opengl_context_egl_t* self, window_info_t* wi,
                                                   opengl_surface_handle_t* out_surface,
                                                   const opengl_context_version_t* version,
                                                   EGLContext share_context, bool make_current, Error* err)
{
  if (!egl_create_context_internal(self, window_info_is_surfaceless_inline(wi),
                                   wi->surface_format, version, share_context, err))
    return false;

  EGLSurface esurface;
  if (window_info_is_surfaceless_inline(wi))
  {
    if (!opengl_context_egl_supports_surfaceless(self))
    {
      esurface = egl_get_pbuffer_surface(self, err);
      if (esurface == EGL_NO_SURFACE)
      {
        ERROR_LOG("Failed to create pbuffer surface for context");
        eglDestroyContext(self->m_display, self->m_context);
        self->m_context = EGL_NO_CONTEXT;
        return false;
      }
    }
    else
    {
      esurface = EGL_NO_SURFACE;
    }
    *out_surface = NULL;
  }
  else
  {
    esurface = self->plat->create_platform_surface(self, self->m_config, wi, err);
    if (esurface == EGL_NO_SURFACE)
    {
      ERROR_LOG("Failed to create surface for context");
      eglDestroyContext(self->m_display, self->m_context);
      self->m_context = EGL_NO_CONTEXT;
      return false;
    }

    egl_update_window_info_size(self, wi, esurface);
    *out_surface = (opengl_surface_handle_t)esurface;
  }

  if (make_current)
  {
    if (!eglMakeCurrent(self->m_display, esurface, esurface, self->m_context))
    {
      Error_set_string_fmt(err, "eglMakeCurrent() failed: 0x%X", (unsigned)eglGetError());
      if (esurface != EGL_NO_SURFACE && esurface != self->m_pbuffer_surface)
        self->plat->destroy_platform_surface(self, esurface);
      eglDestroyContext(self->m_display, self->m_context);
      self->m_context = EGL_NO_CONTEXT;
      return false;
    }
    self->m_current_surface = esurface;
  }

  return true;
}

bool opengl_context_egl_initialize(opengl_context_egl_t* self, window_info_t* wi,
                                   opengl_surface_handle_t* out_surface,
                                   const opengl_context_version_t* versions, size_t version_count,
                                   Error* err)
{
  if (!load_glad_egl(EGL_NO_DISPLAY, err))
    return false;

  self->m_display = self->plat->get_platform_display(self, wi, err);
  if (self->m_display == EGL_NO_DISPLAY)
    return false;

  int egl_major = 0, egl_minor = 0;
  if (!eglInitialize(self->m_display, &egl_major, &egl_minor))
  {
    const int e = (int)eglGetError();
    Error_set_string_fmt(err, "eglInitialize() failed: %d (0x%X)", e, (unsigned)e);
    return false;
  }
  DEV_LOG("eglInitialize() version: %d.%d", egl_major, egl_minor);

  /* Re-init GLAD with the actual display so display-bound extensions resolve. */
  if (!load_glad_egl(self->m_display, err))
    return false;

  if (!GLAD_EGL_KHR_surfaceless_context)
    WARNING_LOG("EGL implementation does not support surfaceless contexts, emulating with pbuffers");

  Error context_error = ERROR_INIT;
  for (size_t i = 0; i < version_count; ++i)
  {
    Error_destroy(&context_error);
    context_error = (Error)ERROR_INIT;
    if (opengl_context_egl_create_context_and_surface(self, wi, out_surface, &versions[i],
                                                      EGL_NO_CONTEXT, true, &context_error))
    {
      Error_destroy(&context_error);
      return true;
    }

    WARNING_LOG("Failed to create %d.%d (%s) context: %s",
                versions[i].major_version, versions[i].minor_version,
                versions[i].profile == OPENGL_CONTEXT_PROFILE_ES   ? "ES"
              : versions[i].profile == OPENGL_CONTEXT_PROFILE_CORE ? "Core" : "None", 
                Error_get_description(&context_error));
  }
  Error_destroy(&context_error);

  Error_set_string(err, "Failed to create any context versions");
  return false;
}

static void* egl_get_proc_address(opengl_context_t* ctx, const char* name)
{
  (void)ctx;
  return (void*)eglGetProcAddress(name);
}

static opengl_surface_handle_t egl_create_surface(opengl_context_t* ctx, window_info_t* wi, Error* err)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  if (window_info_is_surfaceless_inline(wi))
  {
    Error_set_string(err, "Trying to create a surfaceless surface.");
    return NULL;
  }

  EGLSurface surface = self->plat->create_platform_surface(self, self->m_config, wi, err);
  if (surface == EGL_NO_SURFACE)
    return NULL;

  egl_update_window_info_size(self, wi, surface);
  return (opengl_surface_handle_t)surface;
}

static void egl_destroy_surface(opengl_context_t* ctx, opengl_surface_handle_t handle)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  if (!handle) return;

  EGLSurface surface = (EGLSurface)handle;
  if (self->m_current_surface == surface)
  {
    eglMakeCurrent(self->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   opengl_context_egl_supports_surfaceless(self) ? self->m_context : EGL_NO_CONTEXT);
    self->m_current_surface = EGL_NO_SURFACE;
  }
  self->plat->destroy_platform_surface(self, surface);
}

static void egl_resize_surface(opengl_context_t* ctx, window_info_t* wi, opengl_surface_handle_t handle)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  if (!handle) return;
  egl_update_window_info_size(self, wi, (EGLSurface)handle);
}

static bool egl_swap_buffers(opengl_context_t* ctx)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  return eglSwapBuffers(self->m_display, self->m_current_surface);
}

static bool egl_is_current(const opengl_context_t* ctx)
{
  const opengl_context_egl_t* self = (const opengl_context_egl_t*)ctx;
  return self->m_context != EGL_NO_CONTEXT && eglGetCurrentContext() == self->m_context;
}

static bool egl_make_current(opengl_context_t* ctx, opengl_surface_handle_t s, Error* err)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  EGLSurface esurface = s ? (EGLSurface)s : egl_get_surfaceless_surface(self);
  if (esurface == self->m_current_surface)
    return true;

  if (!eglMakeCurrent(self->m_display, esurface, esurface, self->m_context))
  {
    Error_set_string_fmt(err, "eglMakeCurrent() failed: 0x%X", (unsigned)eglGetError());
    return false;
  }
  self->m_current_surface = esurface;
  return true;
}

static bool egl_done_current(opengl_context_t* ctx)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  if (!eglMakeCurrent(self->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
    return false;
  self->m_current_surface = EGL_NO_SURFACE;
  return true;
}

static bool egl_supports_negative_swap_interval(const opengl_context_t* ctx)
{
  const opengl_context_egl_t* self = (const opengl_context_egl_t*)ctx;
  return self->m_supports_negative_swap_interval;
}

static bool egl_set_swap_interval(opengl_context_t* ctx, s32 interval, Error* err)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  if (!eglSwapInterval(self->m_display, interval))
  {
    Error_set_string_fmt(err, "eglSwapInterval() failed: 0x%X", (unsigned)eglGetError());
    return false;
  }
  return true;
}

static opengl_context_t* egl_create_shared_context(opengl_context_t* ctx, window_info_t* wi,
                                                   opengl_surface_handle_t* out_surface, Error* err)
{
  opengl_context_egl_t* self = (opengl_context_egl_t*)ctx;
  /* Platform may override (xlib clones m_display).  Default: error out. */
  if (self->plat->create_shared_context)
    return self->plat->create_shared_context(ctx, wi, out_surface, err);

  Error_set_string(err, "Shared context not supported on this platform");
  return NULL;
}
