/*
 * Linux-only port. See input_manager.h for the SCOPE NOTES describing the
 * dropped sub-features (USB key code tables, ImGui hooks, macros, mouse
 * pointer, profiles, hooks, sensor calibration). What stays is enough to
 * load bindings from a settings file, dispatch events from SDL keyboard +
 * gamepads into per-binding callbacks, and pump simple rumble.
 */

#include "input_manager.h"
#include "input_source.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/threading.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(InputManager);

/* Constants                                                          */

enum {
  /* Indices in s_state.input_sources where external (non-keyboard, non- */
  FIRST_EXTERNAL_INPUT_SOURCE = (u32)INPUT_SOURCE_POINTER + 1u,
  LAST_EXTERNAL_INPUT_SOURCE  = (u32)INPUT_SOURCE_COUNT,
};

/* Binding storage                                                    */

/* One InputBinding: 1..MAX_KEYS_PER_BINDING keys (chord), one handler. */
typedef struct input_binding {
  input_binding_key_t    keys[INPUT_MAX_KEYS_PER_BINDING];
  /* exactly one of axis_handler / button_handler is non-NULL. */
  input_axis_handler_t   axis_handler;
  input_button_handler_t button_handler;
  void*                  user;
  u8                     num_keys;
  u8                     full_mask;
  u8                     current_mask;
} input_binding_t;

typedef struct {
  u64                 pad_and_bind_index;
  input_binding_key_t binding;
  input_source_t*     source;
  float               last_intensity;
} pad_vibration_binding_t;

typedef struct {
  input_binding_key_t key;       /* MaskDirection()'d */
  input_binding_t*    binding;   /* shared pointer; refcount tracked separately */
} binding_map_entry_t;

/* Module state                                                       */

typedef struct {
  /* Bindings */
  binding_map_entry_t* binding_map;
  size_t               binding_map_count;
  size_t               binding_map_capacity;

  input_binding_t**    owned_bindings;
  size_t               owned_bindings_count;
  size_t               owned_bindings_capacity;

  /* Vibration */
  pad_vibration_binding_t* pad_vibration_array;
  size_t                   pad_vibration_count;
  size_t                   pad_vibration_capacity;

  input_source_t* input_sources[INPUT_SOURCE_COUNT];

  /* App focus */
  bool application_in_background;
  bool ignore_input_events;
  bool disable_background_input;

  gamepad_button_type_t last_gamepad_button_type;

  pthread_mutex_t sources_mutex;
} input_manager_state_t;

static input_manager_state_t s_state;
static bool                  s_state_initialized = false;

static void state_init_once(void)
{
  if (s_state_initialized)
    return;
  memset(&s_state, 0, sizeof(s_state));
  pthread_mutex_init(&s_state.sources_mutex, NULL);
  s_state_initialized = true;
}

/* Forward declarations                                               */

static void   add_binding_internal(const char* binding_str, input_axis_handler_t axis_handler,
                                   input_button_handler_t button_handler, void* user);
static bool   parse_keyboard_key(const char* source, const char* sub_binding, input_binding_key_t* out);
static bool   parse_pointer_key(const char* source, const char* sub_binding, input_binding_key_t* out);
static bool   split_binding(const char* binding, const char** src_data, u32* src_len,
                            const char** sub_data, u32* sub_len);

/* Source name table                                                  */

static const char* const s_source_names[INPUT_SOURCE_COUNT] = {
  "Keyboard",
  "Pointer",
  "SDL", 
};

const char* input_manager_source_to_string(input_source_type_t t)
{
  if ((u32)t >= INPUT_SOURCE_COUNT)
    return "Unknown";
  return s_source_names[t];
}

bool input_manager_parse_source_string(const char* str, input_source_type_t* out)
{
  for (u32 i = 0; i < INPUT_SOURCE_COUNT; i++) {
    if (strcmp(str, s_source_names[i]) == 0) {
      *out = (input_source_type_t)i;
      return true;
    }
  }
  return false;
}

bool input_manager_get_source_default_enabled(input_source_type_t t)
{
  switch (t) {
    case INPUT_SOURCE_KEYBOARD:
    case INPUT_SOURCE_POINTER:
    case INPUT_SOURCE_SDL:
      return true;
    default:
      return false;
  }
}

input_source_t* input_manager_get_source(input_source_type_t t)
{
  state_init_once();
  if ((u32)t >= INPUT_SOURCE_COUNT)
    return NULL;
  return s_state.input_sources[t];
}

/* Dynamic-array helpers                                              */

static void* xrealloc(void* p, size_t n)
{
  void* q = realloc(p, n);
  if (!q && n != 0)
    abort();
  return q;
}

static void binding_map_push(input_binding_key_t key, input_binding_t* b)
{
  if (s_state.binding_map_count == s_state.binding_map_capacity) {
    size_t newcap = s_state.binding_map_capacity ? s_state.binding_map_capacity * 2 : 16;
    s_state.binding_map = (binding_map_entry_t*)xrealloc(s_state.binding_map,
                                                         newcap * sizeof(binding_map_entry_t));
    s_state.binding_map_capacity = newcap;
  }
  s_state.binding_map[s_state.binding_map_count++] = (binding_map_entry_t){ key, b };
}

static void owned_bindings_push(input_binding_t* b)
{
  if (s_state.owned_bindings_count == s_state.owned_bindings_capacity) {
    size_t newcap = s_state.owned_bindings_capacity ? s_state.owned_bindings_capacity * 2 : 16;
    s_state.owned_bindings = (input_binding_t**)xrealloc(s_state.owned_bindings,
                                                         newcap * sizeof(input_binding_t*));
    s_state.owned_bindings_capacity = newcap;
  }
  s_state.owned_bindings[s_state.owned_bindings_count++] = b;
}

static void clear_bindings(void)
{
  free(s_state.binding_map);
  s_state.binding_map = NULL;
  s_state.binding_map_count = 0;
  s_state.binding_map_capacity = 0;

  for (size_t i = 0; i < s_state.owned_bindings_count; i++)
    free(s_state.owned_bindings[i]);
  free(s_state.owned_bindings);
  s_state.owned_bindings = NULL;
  s_state.owned_bindings_count = 0;
  s_state.owned_bindings_capacity = 0;

  free(s_state.pad_vibration_array);
  s_state.pad_vibration_array = NULL;
  s_state.pad_vibration_count = 0;
  s_state.pad_vibration_capacity = 0;
}

/* Keyboard helpers (SDL_GetKeyName / SDL_GetKeyFromName)             */

#include <SDL.h>

input_binding_key_t input_manager_make_keyboard_key(u32 code)
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type = INPUT_SOURCE_KEYBOARD;
  k.bits_.data        = code;
  return k;
}

bool input_manager_keyboard_string_to_code(const char* str, u32 len, u32* out_code)
{
  if (len == 0)
    return false;

  /* SDL_GetKeyFromName takes a NUL-terminated string. */
  char buf[64];
  size_t cp = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, str, cp);
  buf[cp] = '\0';

  SDL_Keycode kc = SDL_GetKeyFromName(buf);
  if (kc == SDLK_UNKNOWN)
    return false;
  *out_code = (u32)kc;
  return true;
}

const char* input_manager_keyboard_code_to_string(u32 code)
{
  const char* name = SDL_GetKeyName((SDL_Keycode)code);
  if (!name || name[0] == '\0')
    return NULL;
  return name;
}

input_binding_key_t input_manager_make_pointer_button_key(u32 index, u32 button_index)
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = INPUT_SOURCE_POINTER;
  k.bits_.source_index   = index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_POINTER_BUTTON;
  k.bits_.data           = button_index;
  return k;
}

/* Binding parse                                                      */

static bool split_binding(const char* binding, const char** src_data, u32* src_len,
                          const char** sub_data, u32* sub_len)
{
  if (!binding)
    return false;
  const char* slash = strchr(binding, '/');
  if (!slash)
    return false;
  *src_data = binding;
  *src_len  = (u32)(slash - binding);
  *sub_data = slash + 1;
  *sub_len  = (u32)strlen(slash + 1);
  return true;
}

static bool starts_with(const char* data, u32 len, const char* prefix)
{
  size_t plen = strlen(prefix);
  return (len >= plen && memcmp(data, prefix, plen) == 0);
}

static bool parse_keyboard_key(const char* source, const char* sub_binding, input_binding_key_t* out)
{
  if (strcmp(source, "Keyboard") != 0)
    return false;
  u32 code;
  if (!input_manager_keyboard_string_to_code(sub_binding, (u32)strlen(sub_binding), &code))
    return false;
  *out = input_manager_make_keyboard_key(code);
  return true;
}

static bool parse_pointer_key(const char* source, const char* sub_binding, input_binding_key_t* out)
{
  if (!starts_with(source, (u32)strlen(source), "Pointer-"))
    return false;
  s32 idx = 0;
  if (!string_util_from_chars_s32(source + 8, (u32)(strlen(source) - 8), 10, &idx, NULL) || idx < 0)
    return false;

  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type  = INPUT_SOURCE_POINTER;
  k.bits_.source_index = (u32)idx & 0xFFu;

  if (starts_with(sub_binding, (u32)strlen(sub_binding), "Button")) {
    s32 btn = 0;
    if (!string_util_from_chars_s32(sub_binding + 6, (u32)(strlen(sub_binding) - 6), 10, &btn, NULL) ||
        btn < 0) {
      return false;
    }
    k.bits_.source_subtype = INPUT_SUBCLASS_POINTER_BUTTON;
    k.bits_.data           = (u32)btn;
    *out = k;
    return true;
  }

  static const char* const button_names[] = { "LeftButton", "RightButton", "MiddleButton" };
  for (u32 i = 0; i < (u32)(sizeof(button_names) / sizeof(button_names[0])); i++) {
    if (strcmp(sub_binding, button_names[i]) == 0) {
      k.bits_.source_subtype = INPUT_SUBCLASS_POINTER_BUTTON;
      k.bits_.data           = i;
      *out = k;
      return true;
    }
  }

  return false;
}

bool input_manager_parse_binding_key(const char* binding, input_binding_key_t* out_key)
{
  state_init_once();
  const char *src_d, *sub_d;
  u32 src_l, sub_l;
  if (!split_binding(binding, &src_d, &src_l, &sub_d, &sub_l))
    return false;

  /* Make NUL-terminated copies (binding strings are short). */
  char src[64], sub[128];
  size_t cp_s = (src_l < sizeof(src) - 1) ? src_l : sizeof(src) - 1;
  size_t cp_b = (sub_l < sizeof(sub) - 1) ? sub_l : sizeof(sub) - 1;
  memcpy(src, src_d, cp_s); src[cp_s] = '\0';
  memcpy(sub, sub_d, cp_b); sub[cp_b] = '\0';

  if (starts_with(src, (u32)cp_s, "Keyboard"))
    return parse_keyboard_key(src, sub, out_key);
  if (starts_with(src, (u32)cp_s, "Pointer"))
    return parse_pointer_key(src, sub, out_key);

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++) {
    input_source_t* s = s_state.input_sources[i];
    if (s && input_source_parse_key_string(s, src, sub, out_key))
      return true;
  }
  return false;
}

bool input_manager_parse_binding_and_get_source(const char* binding, input_binding_key_t* out_key,
                                                input_source_t** out_source)
{
  state_init_once();
  const char *src_d, *sub_d;
  u32 src_l, sub_l;
  if (!split_binding(binding, &src_d, &src_l, &sub_d, &sub_l))
    return false;

  char src[64], sub[128];
  size_t cp_s = (src_l < sizeof(src) - 1) ? src_l : sizeof(src) - 1;
  size_t cp_b = (sub_l < sizeof(sub) - 1) ? sub_l : sizeof(sub) - 1;
  memcpy(src, src_d, cp_s); src[cp_s] = '\0';
  memcpy(sub, sub_d, cp_b); sub[cp_b] = '\0';

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++) {
    input_source_t* s = s_state.input_sources[i];
    if (s && input_source_parse_key_string(s, src, sub, out_key)) {
      *out_source = s;
      return true;
    }
  }
  return false;
}

/* Convert key -> string                                              */

void input_manager_convert_key_to_string(input_binding_type_t binding_type, input_binding_key_t key,
                                         small_string_t* out)
{
  small_string_clear(out);
  if (key.bits_.source_type == INPUT_SOURCE_KEYBOARD) {
    const char* name = input_manager_keyboard_code_to_string(key.bits_.data);
    if (name)
      small_string_append_sprintf(out, "Keyboard/%s", name);
    return;
  }

  if (key.bits_.source_type == INPUT_SOURCE_POINTER) {
    if (key.bits_.source_subtype == INPUT_SUBCLASS_POINTER_BUTTON) {
      static const char* const btn_names[] = { "LeftButton", "RightButton", "MiddleButton" };
      if (key.bits_.data < (u32)(sizeof(btn_names) / sizeof(btn_names[0])))
        small_string_append_sprintf(out, "Pointer-%u/%s", key.bits_.source_index, btn_names[key.bits_.data]);
      else
        small_string_append_sprintf(out, "Pointer-%u/Button%u", key.bits_.source_index, key.bits_.data);
    }
    return;
  }

  if (key.bits_.source_type < INPUT_SOURCE_COUNT && s_state.input_sources[key.bits_.source_type]) {
    input_source_t* s = s_state.input_sources[key.bits_.source_type];
    input_source_convert_key_to_string(s, key, out);

     /*
     * never need that here without a UI consumer. */
    (void)binding_type;
  }
}

/* Add binding (chord-aware)                                          */

/* Splits a chord string ("A & B & C") into parts and calls the consumer for
 * each. Whitespace around each part is stripped. Returns the number of parts
 * pushed. */
static u32 for_each_chord_part(const char* binding, void (*cb)(const char* part, void* user), void* user)
{
  if (!binding)
    return 0;
  u32 count = 0;
  const char* p = binding;
  while (*p) {
    /* Skip leading whitespace */
    while (*p && string_util_is_whitespace(*p))
      p++;
    if (!*p)
      break;

    const char* start = p;
    while (*p && *p != '&')
      p++;
    /* Trim trailing whitespace */
    const char* end = p;
    while (end > start && string_util_is_whitespace(*(end - 1)))
      end--;

    if (end > start) {
      char part[160];
      size_t plen = (size_t)(end - start);
      if (plen >= sizeof(part))
        plen = sizeof(part) - 1;
      memcpy(part, start, plen);
      part[plen] = '\0';
      cb(part, user);
      count++;
    }

    if (*p == '&')
      p++;
  }
  return count;
}

typedef struct {
  input_binding_t*       binding;
  input_axis_handler_t   axis_handler;
  input_button_handler_t button_handler;
  void*                  user;
  bool                   failed;
} chord_build_ctx_t;

static void chord_part_cb(const char* part, void* user)
{
  chord_build_ctx_t* ctx = (chord_build_ctx_t*)user;
  if (ctx->failed)
    return;

  input_binding_key_t key;
  if (!input_manager_parse_binding_key(part, &key)) {
    ERROR_LOG("Invalid binding part: '%s'", part);
    ctx->failed = true;
    return;
  }

  if (!ctx->binding) {
    ctx->binding = (input_binding_t*)calloc(1, sizeof(input_binding_t));
    ctx->binding->axis_handler   = ctx->axis_handler;
    ctx->binding->button_handler = ctx->button_handler;
    ctx->binding->user           = ctx->user;
  }

  if (ctx->binding->num_keys >= INPUT_MAX_KEYS_PER_BINDING) {
    ERROR_LOG("Too many chord parts (max %u)", (unsigned)INPUT_MAX_KEYS_PER_BINDING);
    ctx->failed = true;
    return;
  }

  ctx->binding->keys[ctx->binding->num_keys] = key;
  ctx->binding->full_mask |= (u8)(1u << ctx->binding->num_keys);
  ctx->binding->num_keys++;
}

static void add_binding_internal(const char* binding_str, input_axis_handler_t axis_handler,
                                 input_button_handler_t button_handler, void* user)
{
  state_init_once();
  chord_build_ctx_t ctx = { NULL, axis_handler, button_handler, user, false };
  for_each_chord_part(binding_str, chord_part_cb, &ctx);

  if (ctx.failed || !ctx.binding) {
    free(ctx.binding);
    return;
  }

  /* Insert into map for every key (masked direction). */
  for (u32 i = 0; i < ctx.binding->num_keys; i++)
    binding_map_push(input_binding_key_mask_direction(ctx.binding->keys[i]), ctx.binding);

  owned_bindings_push(ctx.binding);
}

void input_manager_add_axis_binding(const char* binding, input_axis_handler_t handler, void* user)
{
  add_binding_internal(binding, handler, NULL, user);
}

void input_manager_add_button_binding(const char* binding, input_button_handler_t handler, void* user)
{
  add_binding_internal(binding, NULL, handler, user);
}

/* Has-bindings queries                                               */

bool input_manager_has_any_bindings_for_key(input_binding_key_t key)
{
  state_init_once();
  input_binding_key_t masked = input_binding_key_mask_direction(key);
  for (size_t i = 0; i < s_state.binding_map_count; i++) {
    if (s_state.binding_map[i].key.bits == masked.bits)
      return true;
  }
  return false;
}

bool input_manager_has_any_bindings_for_source(input_binding_key_t key)
{
  state_init_once();
  for (size_t i = 0; i < s_state.binding_map_count; i++) {
    const input_binding_key_t* okey = &s_state.binding_map[i].key;
    if (okey->bits_.source_type == key.bits_.source_type &&
        okey->bits_.source_index == key.bits_.source_index)
      return true;
  }
  return false;
}

/* Event dispatch                                                     */

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static void invoke_axis_handler(input_binding_t* b, float value)
{
  if (b->axis_handler)
    b->axis_handler(b->user, value);
}

static void invoke_button_handler(input_binding_t* b, s32 pressed)
{
  if (b->button_handler)
    b->button_handler(b->user, pressed);
}

static void process_event(input_binding_key_t key, float value)
{
  input_binding_key_t masked = input_binding_key_mask_direction(key);

  /* Walk all bindings whose masked key matches. We allow duplicates because
   * a key may belong to multiple chords. */
  for (size_t i = 0; i < s_state.binding_map_count; i++) {
    if (s_state.binding_map[i].key.bits != masked.bits)
      continue;

    input_binding_t* binding = s_state.binding_map[i].binding;

    /* find the matching key in the chord */
    for (u32 k = 0; k < binding->num_keys; k++) {
      input_binding_key_t bk = binding->keys[k];
      if (input_binding_key_mask_direction(bk).bits != masked.bits)
        continue;

      const u8 bit = (u8)(1u << k);
      const bool negative = (bk.bits_.modifier == INPUT_MODIFIER_NEGATE);
      const bool new_state = negative ? (value < 0.0f) : (value > 0.0f);

      float v_pass = 0.0f;
      switch (bk.bits_.modifier) {
        case INPUT_MODIFIER_NONE:
          v_pass = (value > 0.0f) ? value : 0.0f;
          break;
        case INPUT_MODIFIER_NEGATE:
          v_pass = (value < 0.0f) ? -value : 0.0f;
          break;
        case INPUT_MODIFIER_FULL_AXIS:
          v_pass = value * 0.5f + 0.5f;
          break;
        default:
          break;
      }
      if (bk.bits_.invert)
        v_pass = 1.0f - v_pass;

      if (binding->axis_handler) {
        if (v_pass >= 0.0f)
          invoke_axis_handler(binding, v_pass);
      } else {
        const u8 new_mask = new_state ? (u8)(binding->current_mask | bit)
                                      : (u8)(binding->current_mask & ~bit);
        const bool prev_full = (binding->current_mask == binding->full_mask);
        const bool now_full  = (new_mask == binding->full_mask);
        binding->current_mask = new_mask;
        if (prev_full != now_full) {
          const s32 pressed = (v_pass > 0.0f) ? 1 : 0;
          invoke_button_handler(binding, pressed);
        }
      }
      break; /* a chord only has each key once */
    }
  }
}

void input_manager_invoke_events(input_binding_key_t key, float value, generic_input_binding_t generic)
{
  state_init_once();
  (void)generic;

  /* Background input mask: keyboard pass-through, anything else suppressed. */
  if (key.bits_.source_type > INPUT_SOURCE_POINTER && s_state.ignore_input_events)
    return;

  process_event(key, value);
}

void input_manager_clear_bind_state_from_source(input_binding_key_t key)
{
  state_init_once();
  /* Send 0 to axis handlers and -1 to button handlers for every binding tied */
  for (size_t i = 0; i < s_state.binding_map_count; i++) {
    const input_binding_key_t* mk = &s_state.binding_map[i].key;
    if (mk->bits_.source_type != key.bits_.source_type ||
        mk->bits_.source_index != key.bits_.source_index)
      continue;
    input_binding_t* b = s_state.binding_map[i].binding;
    if (b->axis_handler) {
      invoke_axis_handler(b, 0.0f);
    } else {
      if (b->current_mask == b->full_mask)
        invoke_button_handler(b, -1);
      b->current_mask = 0;
    }
  }
}

/* Bindings reload                                                    */

/* Wrapper for a string-list call: returns heap (items, count). */
static void get_string_list(const settings_interface_t* si, const char* section, const char* key,
                            char*** out_arr, size_t* out_count)
{
  settings_interface_get_string_list(si, section, key, out_arr, out_count);
}

/* Per-axis scaling applied at parse time. We keep a deadzone+scale per pad
 * binding by closing over them in the user pointer, but the headless boot
 * just needs raw axis values, so for now we don't apply the per-binding
 * deadzone. */
static float clampf_unused(float v, float lo, float hi) { (void)lo; (void)hi; return v; }
static void  unused_helpers(void) { (void)clampf_unused; (void)clampf; (void)get_string_list; }

void input_manager_reload_bindings(const settings_interface_t* binding_si,
                                   const settings_interface_t* hotkey_binding_si)
{
  state_init_once();
  unused_helpers();

  clear_bindings();

  (void)binding_si;
  (void)hotkey_binding_si;
}

void input_manager_reload_sources_and_bindings(const settings_interface_t* sources_si,
                                               const settings_interface_t* binding_si,
                                               const settings_interface_t* hotkey_binding_si)
{
  state_init_once();
  pthread_mutex_lock(&s_state.sources_mutex);

  /* SDL is the only non-keyboard/pointer source on Linux. */
  const bool sdl_enabled = sources_si
    ? settings_interface_get_bool_value(sources_si, "InputSources", "SDL",
                                        input_manager_get_source_default_enabled(INPUT_SOURCE_SDL))
    : input_manager_get_source_default_enabled(INPUT_SOURCE_SDL);

  if (sdl_enabled) {
    if (!s_state.input_sources[INPUT_SOURCE_SDL]) {
      input_source_t* src = input_source_create_sdl();
      if (src) {
        if (!input_source_initialize(src, sources_si)) {
          ERROR_LOG("SDL input source failed to initialize.");
          input_source_shutdown(src);
          free(src);
        } else {
          s_state.input_sources[INPUT_SOURCE_SDL] = src;
        }
      }
    } else {
      input_source_update_settings(s_state.input_sources[INPUT_SOURCE_SDL], sources_si);
    }
  } else {
    if (s_state.input_sources[INPUT_SOURCE_SDL]) {
      input_source_shutdown(s_state.input_sources[INPUT_SOURCE_SDL]);
      free(s_state.input_sources[INPUT_SOURCE_SDL]);
      s_state.input_sources[INPUT_SOURCE_SDL] = NULL;
    }
  }

  pthread_mutex_unlock(&s_state.sources_mutex);

  input_manager_reload_bindings(binding_si, hotkey_binding_si);
}

/* Polling                                                            */

void input_manager_poll_sources(void)
{
  state_init_once();
  pthread_mutex_lock(&s_state.sources_mutex);
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++) {
    if (s_state.input_sources[i])
      input_source_poll_events(s_state.input_sources[i]);
  }
  pthread_mutex_unlock(&s_state.sources_mutex);
}

void input_manager_close_sources(void)
{
  state_init_once();
  pthread_mutex_lock(&s_state.sources_mutex);
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++) {
    if (s_state.input_sources[i]) {
      input_source_shutdown(s_state.input_sources[i]);
      free(s_state.input_sources[i]);
      s_state.input_sources[i] = NULL;
    }
  }
  pthread_mutex_unlock(&s_state.sources_mutex);

  clear_bindings();
}

void input_manager_enumerate_devices(struct input_device_list* out)
{
  state_init_once();
  out->items = NULL;
  out->count = 0;

  /* Two pseudo-devices come first: keyboard + pointer-0. */
  size_t cap = 4;
  out->items = (input_device_entry_t*)calloc(cap, sizeof(input_device_entry_t));
  out->items[out->count++] = (input_device_entry_t){
     .key = { .bits = 0 },
    .identifier = strdup("Keyboard"),
    .display_name = strdup("Keyboard"), 
  };
  out->items[0].key.bits_.source_type = INPUT_SOURCE_KEYBOARD;

  out->items[out->count] = (input_device_entry_t){
     .key = { .bits = 0 },
    .identifier = strdup("Pointer-0"),
    .display_name = strdup("Mouse"), 
  };
  out->items[out->count].key.bits_.source_type = INPUT_SOURCE_POINTER;
  out->count++;

  /* Append every external source's device list. */
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++) {
    if (!s_state.input_sources[i])
      continue;
    input_device_list_t sub = { NULL, 0 };
    input_source_enumerate_devices(s_state.input_sources[i], &sub);
    if (out->count + sub.count > cap) {
      cap = out->count + sub.count + 4;
      out->items = (input_device_entry_t*)xrealloc(out->items, cap * sizeof(input_device_entry_t));
    }
    for (size_t j = 0; j < sub.count; j++) {
      out->items[out->count++] = sub.items[j];
    }
    free(sub.items); /* shallow free, ownership transferred */
  }
}

/* Vibration                                                          */

static u64 pack_pad_bind(u32 pad, u32 bind) { return ((u64)pad << 32) | (u64)bind; }

void input_manager_set_pad_vibration_intensity(u32 pad_index, u32 bind_index, float intensity)
{
  state_init_once();
  const u64 packed = pack_pad_bind(pad_index, bind_index);
  for (size_t i = 0; i < s_state.pad_vibration_count; i++) {
    pad_vibration_binding_t* v = &s_state.pad_vibration_array[i];
    if (v->pad_and_bind_index == packed && v->last_intensity != intensity) {
      v->last_intensity = intensity;
      if (v->source)
        input_source_update_motor_state(v->source, v->binding, intensity);
    }
  }
}

void input_manager_pause_vibration(void)
{
  state_init_once();
  for (size_t i = 0; i < s_state.pad_vibration_count; i++) {
    pad_vibration_binding_t* v = &s_state.pad_vibration_array[i];
    if (v->source)
      input_source_update_motor_state(v->source, v->binding, 0.0f);
  }
}

void input_manager_clear_effects(void)
{
  input_manager_pause_vibration();
}

/* Application focus                                                  */

void input_manager_on_application_background_state_changed(bool in_background)
{
  state_init_once();
  s_state.application_in_background = in_background;
  s_state.ignore_input_events       = in_background && s_state.disable_background_input;
}

gamepad_button_type_t input_manager_get_last_gamepad_button_type(void)
{
  state_init_once();
  return s_state.last_gamepad_button_type;
}

/* Weak host hooks (default no-op).                                   */

__attribute__((weak)) void host_on_input_device_connected(input_binding_key_t key, const char* identifier,
                                                          const char* device_name)
{
  (void)key; (void)identifier; (void)device_name;
  INFO_LOG("Device connected: %s (%s)", identifier ? identifier : "?",
           device_name ? device_name : "?");
}

__attribute__((weak)) void host_on_input_device_disconnected(input_binding_key_t key, const char* identifier)
{
  (void)key;
  INFO_LOG("Device disconnected: %s", identifier ? identifier : "?");
}

__attribute__((weak)) void host_set_relative_mouse_mode(bool relative, bool hide_cursor)
{
  (void)relative; (void)hide_cursor;
}

