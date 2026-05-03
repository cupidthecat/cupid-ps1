#include "settings_interface.h"

#include "small_string.h"
#include "string_util.h"
#include "types.h"

#include <limits.h>
#include <stdlib.h>

void settings_interface_free_string_list(char** array, size_t count)
{
  if (!array)
    return;
  for (size_t i = 0; i < count; i++)
    free(array[i]);
  free(array);
}

/* Helper: read raw key into a stack scratch buffer, then parse with
 * string_util_from_chars_*. Returns false when the key is absent. */
NEVER_INLINE bool settings_interface_find_int_value(const settings_interface_t* si, const char* section,
                                                    const char* key, s32* value)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (!si->vtbl->lookup_value(si, section, key, &scratch.s)) {
    small_string_destroy(&scratch.s);
    return false;
  }
  s32 parsed = 0;
  const bool ok = string_util_from_chars_s32(scratch.s.buffer, scratch.s.length, 10, &parsed, NULL);
  small_string_destroy(&scratch.s);
  if (ok)
    *value = parsed;
  return ok;
}

NEVER_INLINE bool settings_interface_find_uint_value(const settings_interface_t* si, const char* section,
                                                     const char* key, u32* value)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (!si->vtbl->lookup_value(si, section, key, &scratch.s)) {
    small_string_destroy(&scratch.s);
    return false;
  }
  u32 parsed = 0;
  const bool ok = string_util_from_chars_u32(scratch.s.buffer, scratch.s.length, 10, &parsed, NULL);
  small_string_destroy(&scratch.s);
  if (ok)
    *value = parsed;
  return ok;
}

NEVER_INLINE bool settings_interface_find_float_value(const settings_interface_t* si, const char* section,
                                                      const char* key, float* value)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (!si->vtbl->lookup_value(si, section, key, &scratch.s)) {
    small_string_destroy(&scratch.s);
    return false;
  }
  float parsed = 0.0f;
  const bool ok = string_util_from_chars_float(scratch.s.buffer, scratch.s.length, &parsed, NULL);
  small_string_destroy(&scratch.s);
  if (ok)
    *value = parsed;
  return ok;
}

NEVER_INLINE bool settings_interface_find_double_value(const settings_interface_t* si, const char* section,
                                                       const char* key, double* value)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (!si->vtbl->lookup_value(si, section, key, &scratch.s)) {
    small_string_destroy(&scratch.s);
    return false;
  }
  double parsed = 0.0;
  const bool ok = string_util_from_chars_double(scratch.s.buffer, scratch.s.length, &parsed, NULL);
  small_string_destroy(&scratch.s);
  if (ok)
    *value = parsed;
  return ok;
}

NEVER_INLINE bool settings_interface_find_bool_value(const settings_interface_t* si, const char* section,
                                                     const char* key, bool* value)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (!si->vtbl->lookup_value(si, section, key, &scratch.s)) {
    small_string_destroy(&scratch.s);
    return false;
  }
  bool parsed = false;
  const bool ok = string_util_from_chars_bool(scratch.s.buffer, scratch.s.length, &parsed);
  small_string_destroy(&scratch.s);
  if (ok)
    *value = parsed;
  return ok;
}

bool settings_interface_find_string_value(const settings_interface_t* si, const char* section,
                                          const char* key, small_string_t* out)
{
  return si->vtbl->lookup_value(si, section, key, out);
}

s32 settings_interface_get_int_value(const settings_interface_t* si, const char* section,
                                     const char* key, s32 default_value)
{
  s32 value;
  return settings_interface_find_int_value(si, section, key, &value) ? value : default_value;
}

u32 settings_interface_get_uint_value(const settings_interface_t* si, const char* section,
                                      const char* key, u32 default_value)
{
  u32 value;
  return settings_interface_find_uint_value(si, section, key, &value) ? value : default_value;
}

float settings_interface_get_float_value(const settings_interface_t* si, const char* section,
                                         const char* key, float default_value)
{
  float value;
  return settings_interface_find_float_value(si, section, key, &value) ? value : default_value;
}

double settings_interface_get_double_value(const settings_interface_t* si, const char* section,
                                           const char* key, double default_value)
{
  double value;
  return settings_interface_find_double_value(si, section, key, &value) ? value : default_value;
}

bool settings_interface_get_bool_value(const settings_interface_t* si, const char* section,
                                       const char* key, bool default_value)
{
  bool value;
  return settings_interface_find_bool_value(si, section, key, &value) ? value : default_value;
}

bool settings_interface_get_string_value(const settings_interface_t* si, const char* section,
                                         const char* key, const char* default_value,
                                         small_string_t* out)
{
  if (si->vtbl->lookup_value(si, section, key, out))
    return true;
  small_string_assign_cstr(out, default_value ? default_value : "");
  return false;
}

 /*
 * then clamps to T's [min, max]. We provide one implementation per width via
 * a tiny macro since the bodies are identical. */
#define SETTINGS_DEFINE_SATURATED(suffix, T, T_MIN, T_MAX)                                                             \
  T settings_interface_get_saturated_##suffix(const settings_interface_t* si, const char* section,                     \
                                              const char* key, T default_value)                                        \
  {                                                                                                                    \
    s32 value;                                                                                                         \
    if (!settings_interface_find_int_value(si, section, key, &value))                                                  \
      return default_value;                                                                                            \
    if (value < (s32)(T_MIN))                                                                                          \
      return (T)(T_MIN);                                                                                               \
    if (value > (s32)(T_MAX))                                                                                          \
      return (T)(T_MAX);                                                                                               \
    return (T)value;                                                                                                   \
  }

SETTINGS_DEFINE_SATURATED(u8,  u8,  0,        UCHAR_MAX)
SETTINGS_DEFINE_SATURATED(s16, s16, SHRT_MIN, SHRT_MAX)
SETTINGS_DEFINE_SATURATED(u16, u16, 0,        USHRT_MAX)

#undef SETTINGS_DEFINE_SATURATED

bool settings_interface_get_optional_int_value(const settings_interface_t* si, const char* section,
                                               const char* key, bool has_default,
                                               s32 default_value, s32* out)
{
  if (settings_interface_find_int_value(si, section, key, out))
    return true;
  if (has_default) {
    *out = default_value;
    return true;
  }
  return false;
}

bool settings_interface_get_optional_uint_value(const settings_interface_t* si, const char* section,
                                                const char* key, bool has_default,
                                                u32 default_value, u32* out)
{
  if (settings_interface_find_uint_value(si, section, key, out))
    return true;
  if (has_default) {
    *out = default_value;
    return true;
  }
  return false;
}

bool settings_interface_get_optional_float_value(const settings_interface_t* si, const char* section,
                                                 const char* key, bool has_default,
                                                 float default_value, float* out)
{
  if (settings_interface_find_float_value(si, section, key, out))
    return true;
  if (has_default) {
    *out = default_value;
    return true;
  }
  return false;
}

bool settings_interface_get_optional_double_value(const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  double default_value, double* out)
{
  if (settings_interface_find_double_value(si, section, key, out))
    return true;
  if (has_default) {
    *out = default_value;
    return true;
  }
  return false;
}

bool settings_interface_get_optional_bool_value(const settings_interface_t* si, const char* section,
                                                const char* key, bool has_default,
                                                bool default_value, bool* out)
{
  if (settings_interface_find_bool_value(si, section, key, out))
    return true;
  if (has_default) {
    *out = default_value;
    return true;
  }
  return false;
}

bool settings_interface_get_optional_string_value(const settings_interface_t* si, const char* section,
                                                  const char* key, bool has_default,
                                                  const char* default_value, small_string_t* out)
{
  if (si->vtbl->lookup_value(si, section, key, out))
    return true;
  if (has_default) {
    small_string_assign_cstr(out, default_value ? default_value : "");
    return true;
  }
  return false;
}

/* Setters: render via string_util_to_chars_* into a stack scratch then
 * forward to vtbl->store_value. */
void settings_interface_set_int_value(settings_interface_t* si, const char* section,
                                      const char* key, s32 value)
{
  tiny_string_t buf;
  tiny_string_init(&buf);
  string_util_to_chars_s32(&buf.s, value, 10);
  si->vtbl->store_value(si, section, key, small_string_c_str(&buf.s));
  small_string_destroy(&buf.s);
}

void settings_interface_set_uint_value(settings_interface_t* si, const char* section,
                                       const char* key, u32 value)
{
  tiny_string_t buf;
  tiny_string_init(&buf);
  string_util_to_chars_u32(&buf.s, value, 10);
  si->vtbl->store_value(si, section, key, small_string_c_str(&buf.s));
  small_string_destroy(&buf.s);
}

void settings_interface_set_float_value(settings_interface_t* si, const char* section,
                                        const char* key, float value)
{
  tiny_string_t buf;
  tiny_string_init(&buf);
  string_util_to_chars_float(&buf.s, value);
  si->vtbl->store_value(si, section, key, small_string_c_str(&buf.s));
  small_string_destroy(&buf.s);
}

void settings_interface_set_double_value(settings_interface_t* si, const char* section,
                                         const char* key, double value)
{
  tiny_string_t buf;
  tiny_string_init(&buf);
  string_util_to_chars_double(&buf.s, value);
  si->vtbl->store_value(si, section, key, small_string_c_str(&buf.s));
  small_string_destroy(&buf.s);
}

void settings_interface_set_bool_value(settings_interface_t* si, const char* section,
                                       const char* key, bool value)
{
  si->vtbl->store_value(si, section, key, value ? "true" : "false");
}

void settings_interface_set_string_value(settings_interface_t* si, const char* section,
                                         const char* key, const char* value)
{
  si->vtbl->store_value(si, section, key, value ? value : "");
}

void settings_interface_set_optional_int_value(settings_interface_t* si, const char* section,
                                               const char* key, bool has_value, s32 value)
{
  if (has_value)
    settings_interface_set_int_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_set_optional_uint_value(settings_interface_t* si, const char* section,
                                                const char* key, bool has_value, u32 value)
{
  if (has_value)
    settings_interface_set_uint_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_set_optional_float_value(settings_interface_t* si, const char* section,
                                                 const char* key, bool has_value, float value)
{
  if (has_value)
    settings_interface_set_float_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_set_optional_double_value(settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, double value)
{
  if (has_value)
    settings_interface_set_double_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_set_optional_bool_value(settings_interface_t* si, const char* section,
                                                const char* key, bool has_value, bool value)
{
  if (has_value)
    settings_interface_set_bool_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_set_optional_string_value(settings_interface_t* si, const char* section,
                                                  const char* key, bool has_value, const char* value)
{
  if (has_value)
    settings_interface_set_string_value(si, section, key, value);
  else
    si->vtbl->delete_value(si, section, key);
}

void settings_interface_copy_bool_value(settings_interface_t* dst, const settings_interface_t* src,
                                        const char* section, const char* key)
{
  bool value;
  if (settings_interface_find_bool_value(src, section, key, &value))
    settings_interface_set_bool_value(dst, section, key, value);
  else
    dst->vtbl->delete_value(dst, section, key);
}

void settings_interface_copy_int_value(settings_interface_t* dst, const settings_interface_t* src,
                                       const char* section, const char* key)
{
  s32 value;
  if (settings_interface_find_int_value(src, section, key, &value))
    settings_interface_set_int_value(dst, section, key, value);
  else
    dst->vtbl->delete_value(dst, section, key);
}

void settings_interface_copy_uint_value(settings_interface_t* dst, const settings_interface_t* src,
                                        const char* section, const char* key)
{
  u32 value;
  if (settings_interface_find_uint_value(src, section, key, &value))
    settings_interface_set_uint_value(dst, section, key, value);
  else
    dst->vtbl->delete_value(dst, section, key);
}

void settings_interface_copy_float_value(settings_interface_t* dst, const settings_interface_t* src,
                                         const char* section, const char* key)
{
  float value;
  if (settings_interface_find_float_value(src, section, key, &value))
    settings_interface_set_float_value(dst, section, key, value);
  else
    dst->vtbl->delete_value(dst, section, key);
}

void settings_interface_copy_double_value(settings_interface_t* dst, const settings_interface_t* src,
                                          const char* section, const char* key)
{
  double value;
  if (settings_interface_find_double_value(src, section, key, &value))
    settings_interface_set_double_value(dst, section, key, value);
  else
    dst->vtbl->delete_value(dst, section, key);
}

void settings_interface_copy_string_value(settings_interface_t* dst, const settings_interface_t* src,
                                          const char* section, const char* key)
{
  small_string_stack_t scratch;
  small_string_stack_init(&scratch);
  if (src->vtbl->lookup_value(src, section, key, &scratch.s))
    settings_interface_set_string_value(dst, section, key, small_string_c_str(&scratch.s));
  else
    dst->vtbl->delete_value(dst, section, key);
  small_string_destroy(&scratch.s);
}

void settings_interface_copy_string_list_value(settings_interface_t* dst, const settings_interface_t* src,
                                               const char* section, const char* key)
{
  char** items = NULL;
  size_t count = 0;
  src->vtbl->get_string_list(src, section, key, &items, &count);
  if (count > 0)
    dst->vtbl->set_string_list(dst, section, key, (const char* const*)items, count);
  else
    dst->vtbl->delete_value(dst, section, key);
  settings_interface_free_string_list(items, count);
}

void settings_interface_copy_section(settings_interface_t* dst, const settings_interface_t* src,
                                     const char* section)
{
  dst->vtbl->clear_section(dst, section);

  char** keys   = NULL;
  char** values = NULL;
  size_t count  = 0;
  src->vtbl->get_key_value_list(src, section, &keys, &values, &count);
  for (size_t i = 0; i < count; i++)
    settings_interface_set_string_value(dst, section, keys[i], values[i]);
  settings_interface_free_string_list(keys,   count);
  settings_interface_free_string_list(values, count);
}
