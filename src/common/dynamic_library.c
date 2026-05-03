#include "dynamic_library.h"

#include "assert.h"
#include "error.h"
#include "log.h"
#include "small_string.h"

#include <dlfcn.h>
#include <string.h>

LOG_CHANNEL(DynamicLibrary);

void dynamic_library_init(dynamic_library_t* lib)
{
  lib->handle = NULL;
}

void dynamic_library_destroy(dynamic_library_t* lib)
{
  dynamic_library_close(lib);
}

bool dynamic_library_is_open(const dynamic_library_t* lib)
{
  return lib->handle != NULL;
}

void dynamic_library_get_unprefixed_filename(small_string_t* out, const char* filename)
{
  small_string_assign_cstr(out, filename);
  small_string_append_cstr(out, ".so");
}

void dynamic_library_get_versioned_filename(small_string_t* out, const char* libname,
                                            int major, int minor, int patch)
{
  const char* prefix = (strncmp(libname, "lib", 3) == 0) ? "" : "lib";
  if (major >= 0 && minor >= 0 && patch >= 0)
    small_string_sprintf(out, "%s%s.so.%d.%d.%d", prefix, libname, major, minor, patch);
  else if (major >= 0 && minor >= 0)
    small_string_sprintf(out, "%s%s.so.%d.%d", prefix, libname, major, minor);
  else if (major >= 0)
    small_string_sprintf(out, "%s%s.so.%d", prefix, libname, major);
  else
    small_string_sprintf(out, "%s%s.so", prefix, libname);
}

bool dynamic_library_open(dynamic_library_t* lib, const char* filename, Error* err)
{
  lib->handle = dlopen(filename, RTLD_NOW);
  if (!lib->handle) {
    const char* msg = dlerror();
    Error_set_string_fmt(err, "Loading %s failed: %s", filename, msg ? msg : "<UNKNOWN>");
    return false;
  }
  return true;
}

void dynamic_library_adopt(dynamic_library_t* lib, void* handle)
{
  AssertMsg(handle, "Handle is valid");
  dynamic_library_close(lib);
  lib->handle = handle;
}

void dynamic_library_close(dynamic_library_t* lib)
{
  if (lib->handle) {
    dlclose(lib->handle);
    lib->handle = NULL;
  }
}

void* dynamic_library_get_symbol_address(const dynamic_library_t* lib, const char* name)
{
  return dlsym(lib->handle, name);
}

void* dynamic_library_get_handle(const dynamic_library_t* lib)
{
  return lib->handle;
}
