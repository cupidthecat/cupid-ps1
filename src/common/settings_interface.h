/*
 * SettingsInterface (abstract base) becomes a (vtable, struct) pair:
 *
 *   typedef struct settings_interface settings_interface_t;
 *   struct settings_interface { const settings_interface_vtable_t* vtbl; };
 *
 * Concrete implementations (INI / memory / layered) embed this struct as
 * their first member and supply a vtable.  Callers never poke vtbl directly
 * - they go through the inline wrappers (settings_interface_lookup_value,
 * settings_interface_store_value, ...).
 *
 * Type-mapping rules used here:
 *   std::string_view section/key  -> const char*  (NUL-terminated; INI keys
 *                                                   never contain NUL).
 *   std::string return            -> small_string_t* out-param.
 *   std::optional<T>              -> bool return + T* out-pointer.
 *   std::vector<std::string>      -> (char** array, size_t count) heap pair;
 *                                    free with settings_interface_free_string_list.
 *   std::vector<pair<string,string>> for key/value lists -> two parallel
 *                                    char** arrays + count, freed via
 *                                    settings_interface_free_string_list on
 *                                    each.
 */

#ifndef CUPID_COMMON_SETTINGS_INTERFACE_H
#define CUPID_COMMON_SETTINGS_INTERFACE_H

#include "small_string.h"
#include "types.h"

#include <stddef.h>

typedef struct settings_interface       settings_interface_t;
typedef struct settings_interface_vtable settings_interface_vtable_t;

 /*
 * fill in what they support; non-supported slots can panic. */
struct settings_interface_vtable {
  bool (*is_empty)(settings_interface_t* self);

  /* Reads a raw string value into *out. Returns false if the key is absent.
   * out is unmodified on failure. */
  bool (*lookup_value)(const settings_interface_t* self, const char* section,
                       const char* key, small_string_t* out);

  /* Writes a raw string value (NUL-terminated). */
  void (*store_value)(settings_interface_t* self, const char* section,
                      const char* key, const char* value);

  /* Returns a heap (char**, count) pair. Each element is a malloc()'d C
   * string. Free with settings_interface_free_string_list. count==0 means
   * the key was absent or empty. */
  void (*get_string_list)(const settings_interface_t* self, const char* section,
                          const char* key, char*** out_array, size_t* out_count);

  void (*set_string_list)(settings_interface_t* self, const char* section,
                          const char* key, const char* const* items, size_t count);

  bool (*remove_from_string_list)(settings_interface_t* self, const char* section,
                                  const char* key, const char* item);
  bool (*add_to_string_list)(settings_interface_t* self, const char* section,
                             const char* key, const char* item);

  /* Returns parallel (keys, values) heap arrays of length *out_count.
   * Each slot is malloc()'d. Free each array with
   * settings_interface_free_string_list. */
  void (*get_key_value_list)(const settings_interface_t* self, const char* section,
                             char*** out_keys, char*** out_values, size_t* out_count);

  void (*set_key_value_list)(settings_interface_t* self, const char* section,
                             const char* const* keys, const char* const* values,
                             size_t count);

  bool (*contains_value)(const settings_interface_t* self, const char* section,
                         const char* key);
  void (*delete_value)(settings_interface_t* self, const char* section,
                       const char* key);
  void (*clear_section)(settings_interface_t* self, const char* section);
  void (*remove_section)(settings_interface_t* self, const char* section);
  void (*remove_empty_sections)(settings_interface_t* self);
};

struct settings_interface {
  const settings_interface_vtable_t* vtbl;
};

/* Frees a (char** array, size_t count) pair returned by GetStringList /
 * GetKeyValueList. Each element must have been malloc()'d individually.
 * Tolerates NULL array. */
void settings_interface_free_string_list(char** array, size_t count);

ALWAYS_INLINE bool settings_interface_is_empty(settings_interface_t* si)
{
  return si->vtbl->is_empty(si);
}

ALWAYS_INLINE bool settings_interface_lookup_value(const settings_interface_t* si,
                                                   const char* section, const char* key,
                                                   small_string_t* out)
{
  return si->vtbl->lookup_value(si, section, key, out);
}

ALWAYS_INLINE void settings_interface_store_value(settings_interface_t* si,
                                                  const char* section, const char* key,
                                                  const char* value)
{
  si->vtbl->store_value(si, section, key, value);
}

ALWAYS_INLINE void settings_interface_get_string_list(const settings_interface_t* si,
                                                      const char* section, const char* key,
                                                      char*** out_array, size_t* out_count)
{
  si->vtbl->get_string_list(si, section, key, out_array, out_count);
}

ALWAYS_INLINE void settings_interface_set_string_list(settings_interface_t* si,
                                                      const char* section, const char* key,
                                                      const char* const* items, size_t count)
{
  si->vtbl->set_string_list(si, section, key, items, count);
}

ALWAYS_INLINE bool settings_interface_remove_from_string_list(settings_interface_t* si,
                                                              const char* section, const char* key,
                                                              const char* item)
{
  return si->vtbl->remove_from_string_list(si, section, key, item);
}

ALWAYS_INLINE bool settings_interface_add_to_string_list(settings_interface_t* si,
                                                         const char* section, const char* key,
                                                         const char* item)
{
  return si->vtbl->add_to_string_list(si, section, key, item);
}

ALWAYS_INLINE void settings_interface_get_key_value_list(const settings_interface_t* si,
                                                         const char* section,
                                                         char*** out_keys, char*** out_values,
                                                         size_t* out_count)
{
  si->vtbl->get_key_value_list(si, section, out_keys, out_values, out_count);
}

ALWAYS_INLINE void settings_interface_set_key_value_list(settings_interface_t* si,
                                                         const char* section,
                                                         const char* const* keys,
                                                         const char* const* values, size_t count)
{
  si->vtbl->set_key_value_list(si, section, keys, values, count);
}

ALWAYS_INLINE bool settings_interface_contains_value(const settings_interface_t* si,
                                                     const char* section, const char* key)
{
  return si->vtbl->contains_value(si, section, key);
}

ALWAYS_INLINE void settings_interface_delete_value(settings_interface_t* si,
                                                   const char* section, const char* key)
{
  si->vtbl->delete_value(si, section, key);
}

ALWAYS_INLINE void settings_interface_clear_section(settings_interface_t* si, const char* section)
{
  si->vtbl->clear_section(si, section);
}

ALWAYS_INLINE void settings_interface_remove_section(settings_interface_t* si, const char* section)
{
  si->vtbl->remove_section(si, section);
}

ALWAYS_INLINE void settings_interface_remove_empty_sections(settings_interface_t* si)
{
  si->vtbl->remove_empty_sections(si);
}

bool settings_interface_find_int_value   (const settings_interface_t* si, const char* section,
                                          const char* key, s32* value);
bool settings_interface_find_uint_value  (const settings_interface_t* si, const char* section,
                                          const char* key, u32* value);
bool settings_interface_find_float_value (const settings_interface_t* si, const char* section,
                                          const char* key, float* value);
bool settings_interface_find_double_value(const settings_interface_t* si, const char* section,
                                          const char* key, double* value);
bool settings_interface_find_bool_value  (const settings_interface_t* si, const char* section,
                                          const char* key, bool* value);
bool settings_interface_find_string_value(const settings_interface_t* si, const char* section,
                                          const char* key, small_string_t* out);

s32    settings_interface_get_int_value   (const settings_interface_t* si, const char* section,
                                           const char* key, s32 default_value);
u32    settings_interface_get_uint_value  (const settings_interface_t* si, const char* section,
                                           const char* key, u32 default_value);
float  settings_interface_get_float_value (const settings_interface_t* si, const char* section,
                                           const char* key, float default_value);
double settings_interface_get_double_value(const settings_interface_t* si, const char* section,
                                           const char* key, double default_value);
bool   settings_interface_get_bool_value  (const settings_interface_t* si, const char* section,
                                           const char* key, bool default_value);

/* Returns true and fills *out when the key is present; otherwise assigns
 * default_value (NULL → empty string) and returns false. */
bool settings_interface_get_string_value (const settings_interface_t* si, const char* section,
                                          const char* key, const char* default_value,
                                          small_string_t* out);

u8  settings_interface_get_saturated_u8 (const settings_interface_t* si, const char* section,
                                         const char* key, u8 default_value);
s16 settings_interface_get_saturated_s16(const settings_interface_t* si, const char* section,
                                         const char* key, s16 default_value);
u16 settings_interface_get_saturated_u16(const settings_interface_t* si, const char* section,
                                         const char* key, u16 default_value);

/* Optional reads: bool return + out-pointer + has_default sentinel.
 * If the key is absent and has_default is false, returns false and *out is
 * untouched; if has_default is true, returns true and *out = default_value. */
bool settings_interface_get_optional_int_value   (const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  s32 default_value, s32* out);
bool settings_interface_get_optional_uint_value  (const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  u32 default_value, u32* out);
bool settings_interface_get_optional_float_value (const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  float default_value, float* out);
bool settings_interface_get_optional_double_value(const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  double default_value, double* out);
bool settings_interface_get_optional_bool_value  (const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  bool default_value, bool* out);

/* Optional string read: returns true and fills *out when key is present or
 * has_default is true (with default_value=NULL meaning empty string). */
bool settings_interface_get_optional_string_value(const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  const char* default_value, small_string_t* out);

void settings_interface_set_int_value   (settings_interface_t* si, const char* section,
                                         const char* key, s32 value);
void settings_interface_set_uint_value  (settings_interface_t* si, const char* section,
                                         const char* key, u32 value);
void settings_interface_set_float_value (settings_interface_t* si, const char* section,
                                         const char* key, float value);
void settings_interface_set_double_value(settings_interface_t* si, const char* section,
                                         const char* key, double value);
void settings_interface_set_bool_value  (settings_interface_t* si, const char* section,
                                         const char* key, bool value);
void settings_interface_set_string_value(settings_interface_t* si, const char* section,
                                         const char* key, const char* value);

/* Optional setters: when has_value is false, key is deleted instead. */
void settings_interface_set_optional_int_value   (settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, s32 value);
void settings_interface_set_optional_uint_value  (settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, u32 value);
void settings_interface_set_optional_float_value (settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, float value);
void settings_interface_set_optional_double_value(settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, double value);
void settings_interface_set_optional_bool_value  (settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, bool value);
void settings_interface_set_optional_string_value(settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, const char* value);

/* Copy helpers: read from src and write to dst (or delete from dst if absent
 * in src). */
void settings_interface_copy_bool_value       (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_int_value        (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_uint_value       (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_float_value      (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_double_value     (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_string_value     (settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);
void settings_interface_copy_string_list_value(settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key);

/* Clears the section in dst then writes every key from src as a string. */
void settings_interface_copy_section(settings_interface_t* dst, const settings_interface_t* src,
                                     const char* section);

#endif /* CUPID_COMMON_SETTINGS_INTERFACE_H */
