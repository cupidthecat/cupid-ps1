/*
 * Win32 raw input back-ends; those are dropped here. SDL is the only external
 * source. Keyboard / pointer events are dispatched directly by the input
 * manager and do not have a vtable entry.
 *
 * The C++ class hierarchy:
 *   class InputSource (virtual)  -> input_source_t  + input_source_vtable_t
 *   class SDLInputSource         -> sdl_input_source_t (embeds input_source_t)
 *
 * Concrete sources zero-init, then assign vtbl. The manager hands out the
 * input_source_t* and never pokes vtbl directly - call sites use the inline
 * input_source_* dispatch helpers below.
 */

#ifndef CUPID_UTIL_INPUT_SOURCE_H
#define CUPID_UTIL_INPUT_SOURCE_H

#include "common/small_string.h"
#include "common/types.h"

/* Pulls in input_binding_key_t, input_source_type_t, etc. */
#include "input_manager.h"

typedef struct settings_interface settings_interface_t;
typedef struct input_source       input_source_t;
typedef struct input_source_vtable input_source_vtable_t;

/* Returned from enumerate_devices(). Each entry has an identifier ("SDL-0"),
 * a human display name, and the device key. The caller frees with
 * input_device_list_free(). */
typedef struct input_device_entry {
  input_binding_key_t key;
  char*               identifier;   /* heap, free()'d by free_device_list */
  char*               display_name; /* heap, free()'d by free_device_list */
} input_device_entry_t;

struct input_device_list {
  input_device_entry_t* items;
  size_t                count;
};

void input_device_list_free(input_device_list_t* list);

/* Enumerate-effects result: type + key. Heap (items, count) pair. */
typedef struct input_effect_entry {
  u8                  binding_type; /* INPUT_BINDING_TYPE_* */
  input_binding_key_t key;
} input_effect_entry_t;

struct input_effect_list {
  input_effect_entry_t* items;
  size_t                count;
};

void input_effect_list_free(input_effect_list_t* list);

/* GetGenericBindingMapping result. (generic_id, binding string) pairs. */
typedef struct {
  u8    generic;        /* GENERIC_INPUT_BINDING_* */
  char* binding;        /* heap; free()'d by free_generic_binding_mapping */
} generic_binding_entry_t;

typedef struct {
  generic_binding_entry_t* items;
  size_t                   count;
  size_t                   capacity;
} generic_binding_mapping_t;

void generic_binding_mapping_init(generic_binding_mapping_t* m);
void generic_binding_mapping_destroy(generic_binding_mapping_t* m);
void generic_binding_mapping_push(generic_binding_mapping_t* m, u8 generic, const char* binding);

/* Vtable for input sources. Methods that don't apply (e.g. motors on a
 * keyboard-only source) just point at no-op stubs. */
struct input_source_vtable {
  bool (*initialize)(input_source_t* self, const settings_interface_t* si);
  void (*update_settings)(input_source_t* self, const settings_interface_t* si);
  bool (*reload_devices)(input_source_t* self);
  void (*shutdown)(input_source_t* self);

  void (*poll_events)(input_source_t* self);

  /* Returns true and writes *out_value on success. */
  bool (*get_current_value)(input_source_t* self, input_binding_key_t key, float* out_value);

  bool (*contains_device)(const input_source_t* self, const char* device);

  /* Parses (device, binding) into *out_key. Returns false if not handled. */
  bool (*parse_key_string)(input_source_t* self, const char* device, const char* binding,
                           input_binding_key_t* out_key);

  /* Writes a human key string into *out (e.g. "SDL-0/Cross"). */
  void (*convert_key_to_string)(input_source_t* self, input_binding_key_t key, small_string_t* out);

  /* Heap (items, count) pair via input_device_list_t. */
  void (*enumerate_devices)(input_source_t* self, input_device_list_t* out);

  /* type < 0 means "any". for_device is matched by source_type+source_index
   * when has_for_device is true. */
  void (*enumerate_effects)(input_source_t* self, int type, bool has_for_device,
                            input_binding_key_t for_device, input_effect_list_t* out);

  u32 (*get_pollable_device_count)(const input_source_t* self);

  bool (*get_generic_binding_mapping)(input_source_t* self, const char* device,
                                      generic_binding_mapping_t* mapping);

  void (*update_motor_state)(input_source_t* self, input_binding_key_t key, float intensity);
  void (*update_motor_state_pair)(input_source_t* self, input_binding_key_t large_key,
                                  input_binding_key_t small_key, float large_intensity,
                                  float small_intensity);
  void (*update_led_state)(input_source_t* self, input_binding_key_t key, float intensity);
};

struct input_source {
  const input_source_vtable_t* vtbl;
};

ALWAYS_INLINE bool input_source_initialize(input_source_t* s, const settings_interface_t* si)
{
  return s->vtbl->initialize(s, si);
}
ALWAYS_INLINE void input_source_update_settings(input_source_t* s, const settings_interface_t* si)
{
  s->vtbl->update_settings(s, si);
}
ALWAYS_INLINE bool input_source_reload_devices(input_source_t* s)
{
  return s->vtbl->reload_devices(s);
}
ALWAYS_INLINE void input_source_shutdown(input_source_t* s)
{
  s->vtbl->shutdown(s);
}
ALWAYS_INLINE void input_source_poll_events(input_source_t* s)
{
  s->vtbl->poll_events(s);
}
ALWAYS_INLINE bool input_source_get_current_value(input_source_t* s, input_binding_key_t k, float* v)
{
  return s->vtbl->get_current_value(s, k, v);
}
ALWAYS_INLINE bool input_source_contains_device(const input_source_t* s, const char* d)
{
  return s->vtbl->contains_device(s, d);
}
ALWAYS_INLINE bool input_source_parse_key_string(input_source_t* s, const char* d, const char* b,
                                                 input_binding_key_t* out)
{
  return s->vtbl->parse_key_string(s, d, b, out);
}
ALWAYS_INLINE void input_source_convert_key_to_string(input_source_t* s, input_binding_key_t k,
                                                      small_string_t* out)
{
  s->vtbl->convert_key_to_string(s, k, out);
}
ALWAYS_INLINE void input_source_enumerate_devices(input_source_t* s, input_device_list_t* out)
{
  s->vtbl->enumerate_devices(s, out);
}
ALWAYS_INLINE u32 input_source_get_pollable_device_count(const input_source_t* s)
{
  return s->vtbl->get_pollable_device_count(s);
}
ALWAYS_INLINE void input_source_update_motor_state(input_source_t* s, input_binding_key_t k, float i)
{
  s->vtbl->update_motor_state(s, k, i);
}
ALWAYS_INLINE void input_source_update_motor_state_pair(input_source_t* s, input_binding_key_t lk,
                                                        input_binding_key_t sk, float li, float si_)
{
  s->vtbl->update_motor_state_pair(s, lk, sk, li, si_);
}

input_binding_key_t input_source_make_generic_device_key(input_source_type_t clazz, u32 controller_index);
input_binding_key_t input_source_make_generic_axis_key(input_source_type_t clazz, u32 controller_index,
                                                       s32 axis_index);
input_binding_key_t input_source_make_generic_button_key(input_source_type_t clazz, u32 controller_index,
                                                         s32 button_index);
input_binding_key_t input_source_make_generic_hat_key(input_source_type_t clazz, u32 controller_index,
                                                      s32 hat_index, u8 hat_direction, u32 num_directions);
input_binding_key_t input_source_make_generic_motor_key(input_source_type_t clazz, u32 controller_index,
                                                        s32 motor_index);

/* Generic controller "{Source}{N}/Button{N}" / "{Source}-{N}/+Axis{N}" parser. */
bool input_source_parse_generic_controller_key(input_source_type_t clazz, const char* source,
                                               const char* sub_binding, input_binding_key_t* out_key);

 /* Default no-op vtable methods (suitable for sources that don't implement
 * specific entries). */
bool input_source_stub_reload_devices(input_source_t* self);
bool input_source_stub_get_current_value(input_source_t* self, input_binding_key_t key, float* v);
void input_source_stub_update_motor_state(input_source_t* self, input_binding_key_t key, float intensity);
void input_source_stub_update_motor_state_pair(input_source_t* self, input_binding_key_t lk,
                                               input_binding_key_t sk, float li, float si_);
void input_source_stub_update_led_state(input_source_t* self, input_binding_key_t key, float intensity);
bool input_source_stub_get_generic_binding_mapping(input_source_t* self, const char* device,
                                                   generic_binding_mapping_t* mapping);
void input_source_stub_enumerate_effects(input_source_t* self, int type, bool has_for_device,
                                         input_binding_key_t for_device, input_effect_list_t* out);

/* SDL factory: returns a new heap-allocated SDL source ready to initialize.
 * Caller takes ownership; the source's shutdown vtable entry plus free()
 * tears it down. Returns NULL on alloc failure. */
input_source_t* input_source_create_sdl(void);

#endif /* CUPID_UTIL_INPUT_SOURCE_H */
