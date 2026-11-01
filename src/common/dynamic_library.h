#ifndef CUPID_COMMON_DYNAMIC_LIBRARY_H
#define CUPID_COMMON_DYNAMIC_LIBRARY_H

#include "types.h"

typedef struct Error Error;
typedef struct small_string small_string_t;

typedef struct {
  void* handle;
} dynamic_library_t;

void dynamic_library_init(dynamic_library_t* lib);
void dynamic_library_destroy(dynamic_library_t* lib);

bool  dynamic_library_open  (dynamic_library_t* lib, const char* filename, Error* err);
void  dynamic_library_close (dynamic_library_t* lib);
bool  dynamic_library_is_open(const dynamic_library_t* lib);
void  dynamic_library_adopt (dynamic_library_t* lib, void* handle);
void* dynamic_library_get_symbol_address(const dynamic_library_t* lib, const char* name);
void* dynamic_library_get_handle(const dynamic_library_t* lib);

/* Linux versioned names use libNAME.so.M.m.p; Windows uses NAME-M-m-p.dll.
 * Missing version components are omitted. Unprefixed names only add a suffix. */
void dynamic_library_get_unprefixed_filename(small_string_t* out, const char* filename);
void dynamic_library_get_versioned_filename (small_string_t* out, const char* libname,
                                             int major, int minor, int patch);

#endif /* CUPID_COMMON_DYNAMIC_LIBRARY_H */
