/*
 * Linux-only port. Drops macOS/Win32 input back-ends, USB-key-code translation
 * tables, ImGui integration, input-profile UI, controller mapping wizard,
 * macro buttons, mouse pointer / sensor pipelines. The remaining surface is
 * what the headless boot path needs:
 *   - Define the InputBindingKey packed key + source/subclass/modifier enums.
 *   - Hold a single "input source" external (SDL) plus the keyboard pseudo-
 *     source.
 *   - Parse Keyboard/{ScancodeName} and {Source-N}/{...} bindings out of a
 *     settings_interface_t.
 *   - Dispatch raw events (key/value) into per-binding callbacks.
 *
 *   - "axis" handler: float in [0,1] (or -1..1 for FullAxis).
 *   - "button" handler: int (0=release, 1=press, -1=cancelled chord).
 *
 * Callbacks are (function pointer, void* user) pairs. The handler takes
 * ownership of nothing - the binding stores the (cb,user) pair until it is
 * cleared.
 */

#ifndef CUPID_UTIL_INPUT_MANAGER_H
#define CUPID_UTIL_INPUT_MANAGER_H

#include "common/small_string.h"
#include "common/types.h"

typedef struct settings_interface settings_interface_t;
typedef struct input_source       input_source_t;

typedef enum {
  INPUT_SOURCE_KEYBOARD = 0,
  INPUT_SOURCE_POINTER  = 1,
  INPUT_SOURCE_SDL      = 2,
  INPUT_SOURCE_COUNT    = 3,
} input_source_type_t;

typedef enum {
  INPUT_SUBCLASS_NONE             = 0,

  /* Pointer */
  INPUT_SUBCLASS_POINTER_BUTTON   = 0,
  INPUT_SUBCLASS_POINTER_AXIS     = 1,

  /* Controller (overlap with pointer is intentional; source_type tells them
   * apart). */
  INPUT_SUBCLASS_CONTROLLER_BUTTON = 0,
  INPUT_SUBCLASS_CONTROLLER_AXIS   = 1,
  INPUT_SUBCLASS_CONTROLLER_HAT    = 2,
  INPUT_SUBCLASS_CONTROLLER_SENSOR = 3,
  INPUT_SUBCLASS_CONTROLLER_MOTOR  = 4,
  INPUT_SUBCLASS_CONTROLLER_HAPTIC = 5,
  INPUT_SUBCLASS_CONTROLLER_LED    = 6,
} input_subclass_t;

typedef enum {
  INPUT_MODIFIER_NONE      = 0,
  INPUT_MODIFIER_NEGATE    = 1,  /* axis * -1 */
  INPUT_MODIFIER_FULL_AXIS = 2,  /* axis * 0.5 + 0.5 */
} input_modifier_t;

/* input_binding_type_t and generic_input_binding_t are defined canonically
 * in core/input_types.h.  Pull them in here. */
#include "core/input_types.h"

/* generic_input_binding_t is defined canonically in core/input_types.h. */

typedef union {
  u64 bits;
  /*   source_type    : 4
   *   source_index   : 8  (controller / pointer index)
   *   source_subtype : 3  (button vs axis vs hat ...)
   *   modifier       : 2  (None / Negate / FullAxis)
   *   invert         : 1
   *   <14 unused>
   *   data           : 32 (axis / button / etc)
   *
   * The high 32 bits are the explicit `data` field. The low 32 bits hold the
   * packed metadata. The whole thing fits in a u64 for fast unordered_map-
   * style key compare. */
  struct {
    u32 source_type    : 4;
    u32 source_index   : 8;
    u32 source_subtype : 3;
    u32 modifier       : 2;
    u32 invert         : 1;
    u32 unused         : 14;
    u32 data;
  } bits_;
} input_binding_key_t;

/* Returns key with modifier and invert cleared, used for hash-map lookup. */
ALWAYS_INLINE input_binding_key_t input_binding_key_mask_direction(input_binding_key_t k)
{
  k.bits_.modifier = INPUT_MODIFIER_NONE;
  k.bits_.invert   = 0;
  return k;
}

ALWAYS_INLINE bool input_binding_key_equals(input_binding_key_t a, input_binding_key_t b)
{
  return a.bits == b.bits;
}

typedef void (*input_axis_handler_t)(void* user, float value);

typedef void (*input_button_handler_t)(void* user, s32 pressed);

typedef struct {
  const char* name;
  const char* category;
  const char* display_name;
  void      (*handler)(s32 pressed);
} hotkey_info_t;

enum {
  INPUT_MAX_KEYS_PER_BINDING        = 4,
  INPUT_MAX_MOTORS_PER_PAD          = 2,
  INPUT_MAX_POINTER_DEVICES         = 8,
  INPUT_NUM_MACRO_BUTTONS_PER_PAD   = 8,
};

void host_on_input_device_connected(input_binding_key_t key, const char* identifier,
                                    const char* device_name);
void host_on_input_device_disconnected(input_binding_key_t key, const char* identifier);
void host_set_relative_mouse_mode(bool relative, bool hide_cursor);

const char*         input_manager_source_to_string(input_source_type_t t);
bool                input_manager_parse_source_string(const char* str, input_source_type_t* out);
bool                input_manager_get_source_default_enabled(input_source_type_t t);
input_source_t*     input_manager_get_source(input_source_type_t t);

input_binding_key_t input_manager_make_keyboard_key(u32 code);

/* SDL_GetKeyName-based name<->code conversion. Returns false if not found. */
bool        input_manager_keyboard_string_to_code(const char* str, u32 len, u32* out_code);
const char* input_manager_keyboard_code_to_string(u32 code);

input_binding_key_t input_manager_make_pointer_button_key(u32 index, u32 button_index);

 /* Parses a single binding string ("Keyboard/X" / "SDL-0/Cross" / ...).
 * Returns true and fills *out_key on success. */
bool input_manager_parse_binding_key(const char* binding, input_binding_key_t* out_key);

/* Like above but also returns the source that owned the parse (NULL for
 * Keyboard / Pointer). */
bool input_manager_parse_binding_and_get_source(const char* binding,
                                                input_binding_key_t* out_key,
                                                input_source_t** out_source);

/* Writes a human key string into *out (e.g. "Keyboard/X"). */
void input_manager_convert_key_to_string(input_binding_type_t binding_type,
                                         input_binding_key_t key, small_string_t* out);

void input_manager_add_axis_binding  (const char* binding, input_axis_handler_t   handler, void* user);
void input_manager_add_button_binding(const char* binding, input_button_handler_t handler, void* user);

/* Returns true when the supplied key maps to at least one bound action. */
bool input_manager_has_any_bindings_for_key(input_binding_key_t key);
bool input_manager_has_any_bindings_for_source(input_binding_key_t key);

/* Fires whatever handlers are registered for the supplied key. value is the
 * raw axis value in [-1, 1] (booleans use 0 / 1). */
void input_manager_invoke_events(input_binding_key_t key, float value, generic_input_binding_t generic);

void input_manager_clear_bind_state_from_source(input_binding_key_t key);

/* Builds sources + reloads bindings from settings. Replaces any existing
 * state. Caller still owns the settings interface. */
void input_manager_reload_sources_and_bindings(const settings_interface_t* sources_si,
                                               const settings_interface_t* binding_si,
                                               const settings_interface_t* hotkey_binding_si);

/* Re-parses bindings (sources untouched). */
void input_manager_reload_bindings(const settings_interface_t* binding_si,
                                   const settings_interface_t* hotkey_binding_si);

/* Polls every external source. Call once per frame. */
void input_manager_poll_sources(void);

/* Tears down sources and clears bindings. Safe to call multiple times. */
void input_manager_close_sources(void);

/* Returns a heap-allocated array of devices across all sources. */
typedef struct input_device_list  input_device_list_t;
typedef struct input_effect_list  input_effect_list_t;
void input_manager_enumerate_devices(struct input_device_list* out);

void input_manager_set_pad_vibration_intensity(u32 pad_index, u32 bind_index, float intensity);

/* Pause / clear all motors + LEDs (used on system pause/stop). */
void input_manager_pause_vibration(void);
void input_manager_clear_effects(void);

void input_manager_on_application_background_state_changed(bool in_background);

/* Returns the SDL controller-DB name for the last-connected gamepad
 * ("Xbox" / "PlayStation" / "Unknown"). The headless boot path doesn't use
 * this, but it stays exposed for future UI work. */
typedef enum {
  GAMEPAD_BUTTON_TYPE_UNKNOWN     = 0,
  GAMEPAD_BUTTON_TYPE_XBOX        = 1,
  GAMEPAD_BUTTON_TYPE_PLAYSTATION = 2,
} gamepad_button_type_t;

gamepad_button_type_t input_manager_get_last_gamepad_button_type(void);

#endif /* CUPID_UTIL_INPUT_MANAGER_H */
