/*
 * Concrete settings_interface_t backed by an INI file.  Subclasses
 * settings_interface_t by embedding it as the first member; the vtable
 * pointer is wired up in ini_settings_interface_init.
 *
 * Replaces SimpleIni third-party header with hand-rolled INI parser
 * (see ini_settings_interface.c for grammar details).
 *
 * Storage: a sorted array of sections by name; each section holds a
 * sorted array of (key, value) entries by key.  For string-list keys
 * contiguous run.  Keys and values are independently malloc()'d
 * NUL-terminated strings.
 *
 * Lifecycle:
 *   ini_settings_interface_t s;
 *   ini_settings_interface_init(&s, "/path/to/foo.ini");
 *   ini_settings_interface_load(&s, &err);
 *   ... use settings_interface_lookup_value(&s.base, ...) ...
 *   ini_settings_interface_save(&s, &err);
 *   ini_settings_interface_destroy(&s);
 */

#ifndef CUPID_UTIL_INI_SETTINGS_INTERFACE_H
#define CUPID_UTIL_INI_SETTINGS_INTERFACE_H

#include "common/settings_interface.h"
#include "common/types.h"

#include <stddef.h>

typedef struct Error Error;

/* Sorted (key, value) pair owned by an ini_section_t. */
typedef struct {
  char* key;
  char* value;
} ini_kvp_t;

typedef struct {
  char*      name;     /* heap, NUL-terminated; "" for the implicit pre-header section */
  ini_kvp_t* entries;
  size_t     entry_count;
  size_t     entry_cap;
} ini_section_t;

typedef struct {
  /* Must be first so a (ini_settings_interface_t*) converts to a
   * (settings_interface_t*) for vtable dispatch. */
  settings_interface_t base;

  char*           filename;       /* heap, may be NULL/empty */
  ini_section_t*  sections;
  size_t          section_count;
  size_t          section_cap;
  bool            dirty;
} ini_settings_interface_t;

bool ini_settings_interface_init(ini_settings_interface_t* self, const char* filename);

void ini_settings_interface_destroy(ini_settings_interface_t* self);

/* Replaces the on-disk path; marks dirty if it differs. */
void ini_settings_interface_set_filename(ini_settings_interface_t* self, const char* filename);

const char* ini_settings_interface_get_filename(const ini_settings_interface_t* self);
bool        ini_settings_interface_is_dirty   (const ini_settings_interface_t* self);

bool ini_settings_interface_load(ini_settings_interface_t* self, Error* error);

/* Parses INI text directly without touching the filesystem.  data does not
 * need to be NUL-terminated; len is the byte length. */
bool ini_settings_interface_load_from_string(ini_settings_interface_t* self,
                                             const char* data, size_t len);

/* Writes back to self->filename atomically (write-temp + rename).  Returns
 * false (with *error filled) on any I/O failure. */
bool ini_settings_interface_save(ini_settings_interface_t* self, Error* error);

/* Drops every section and key in memory but keeps the filename. */
void ini_settings_interface_clear(ini_settings_interface_t* self);

#endif /* CUPID_UTIL_INI_SETTINGS_INTERFACE_H */
