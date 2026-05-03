/*
 * SDL2 gamepad backend. See sdl_input_source.h for SCOPE NOTES.
 */

#include "sdl_input_source.h"
#include "input_manager.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <SDL.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(SDL);

enum {
  MOTOR_INDEX_LARGE = 0,
  MOTOR_INDEX_SMALL = 1,
  RUMBLE_DURATION_MS = 100000, /* very long; we re-issue on stop. */
};

typedef struct {
  const char*             name;
  generic_input_binding_t generic_neg;
  generic_input_binding_t generic_pos;
} axis_info_t;

static const axis_info_t s_axis_info[SDL_CONTROLLER_AXIS_MAX] = {
  /* SDL_CONTROLLER_AXIS_LEFTX        */ { "LeftX",        GENERIC_INPUT_BINDING_LEFT_STICK_LEFT,  GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT },
  /* SDL_CONTROLLER_AXIS_LEFTY        */ { "LeftY",        GENERIC_INPUT_BINDING_LEFT_STICK_UP,    GENERIC_INPUT_BINDING_LEFT_STICK_DOWN  },
  /* SDL_CONTROLLER_AXIS_RIGHTX       */ { "RightX",       GENERIC_INPUT_BINDING_RIGHT_STICK_LEFT, GENERIC_INPUT_BINDING_RIGHT_STICK_RIGHT },
  /* SDL_CONTROLLER_AXIS_RIGHTY       */ { "RightY",       GENERIC_INPUT_BINDING_RIGHT_STICK_UP,   GENERIC_INPUT_BINDING_RIGHT_STICK_DOWN },
  /* SDL_CONTROLLER_AXIS_TRIGGERLEFT  */ { "LeftTrigger",  GENERIC_INPUT_BINDING_UNKNOWN,          GENERIC_INPUT_BINDING_L2 },
  /* SDL_CONTROLLER_AXIS_TRIGGERRIGHT */ { "RightTrigger", GENERIC_INPUT_BINDING_UNKNOWN,          GENERIC_INPUT_BINDING_R2 },
};

typedef struct {
  const char*             name;
  generic_input_binding_t generic;
} button_info_t;

static const button_info_t s_button_info[SDL_CONTROLLER_BUTTON_MAX] = {
  { "A",             GENERIC_INPUT_BINDING_CROSS    },
  { "B",             GENERIC_INPUT_BINDING_CIRCLE   },
  { "X",             GENERIC_INPUT_BINDING_SQUARE   },
  { "Y",             GENERIC_INPUT_BINDING_TRIANGLE },
  { "Back",          GENERIC_INPUT_BINDING_SELECT   },
  { "Guide",         GENERIC_INPUT_BINDING_SYSTEM   },
  { "Start",         GENERIC_INPUT_BINDING_START    },
  { "LeftStick",     GENERIC_INPUT_BINDING_L3       },
  { "RightStick",    GENERIC_INPUT_BINDING_R3       },
  { "LeftShoulder",  GENERIC_INPUT_BINDING_L1       },
  { "RightShoulder", GENERIC_INPUT_BINDING_R1       },
  { "DPadUp",        GENERIC_INPUT_BINDING_DPAD_UP    },
  { "DPadDown",      GENERIC_INPUT_BINDING_DPAD_DOWN  },
  { "DPadLeft",      GENERIC_INPUT_BINDING_DPAD_LEFT  },
  { "DPadRight",     GENERIC_INPUT_BINDING_DPAD_RIGHT },
  { "Misc1",         GENERIC_INPUT_BINDING_UNKNOWN  },
  { "Paddle1",       GENERIC_INPUT_BINDING_UNKNOWN  },
  { "Paddle2",       GENERIC_INPUT_BINDING_UNKNOWN  },
  { "Paddle3",       GENERIC_INPUT_BINDING_UNKNOWN  },
  { "Paddle4",       GENERIC_INPUT_BINDING_UNKNOWN  },
  { "Touchpad",      GENERIC_INPUT_BINDING_UNKNOWN  },
};

static const char* const s_hat_direction_names[4] = { "North", "East", "South", "West" };

typedef struct {
  SDL_GameController* gamepad;       /* NULL if joystick-only */
  SDL_Joystick*       joystick;
  SDL_JoystickID      joystick_id;
  int                 player_id;
  bool                use_gamepad_rumble;
  u16                 rumble_intensity[2];

  u8*                 last_hat_state;
  u32                 num_hats;

  /* For raw joystick fallback we need to suppress events on axes/buttons
   * already handled by SDL_GameController. Tracked as bit arrays. */
  bool*               joy_button_used_in_gc;
  u32                 num_joy_buttons;
  bool*               joy_axis_used_in_gc;
  u32                 num_joy_axes;
} controller_data_t;

typedef struct {
  input_source_t          base;          /* must be first */
  controller_data_t*      controllers;
  u32                     num_controllers;
  u32                     cap_controllers;
  bool                    sdl_initialized;
} sdl_input_source_t;

static controller_data_t* find_by_joystick_id(sdl_input_source_t* s, SDL_JoystickID id)
{
  for (u32 i = 0; i < s->num_controllers; i++) {
    if (s->controllers[i].joystick_id == id)
      return &s->controllers[i];
  }
  return NULL;
}

static controller_data_t* find_by_player_id(sdl_input_source_t* s, int player_id)
{
  for (u32 i = 0; i < s->num_controllers; i++) {
    if (s->controllers[i].player_id == player_id)
      return &s->controllers[i];
  }
  return NULL;
}

static int get_free_player_id(sdl_input_source_t* s)
{
  for (int candidate = 0;; candidate++) {
    bool taken = false;
    for (u32 i = 0; i < s->num_controllers; i++) {
      if (s->controllers[i].player_id == candidate) { taken = true; break; }
    }
    if (!taken)
      return candidate;
  }
}

static void controller_data_free(controller_data_t* cd)
{
  if (cd->gamepad)
    SDL_GameControllerClose(cd->gamepad);
  else if (cd->joystick)
    SDL_JoystickClose(cd->joystick);
  free(cd->last_hat_state);
  free(cd->joy_button_used_in_gc);
  free(cd->joy_axis_used_in_gc);
  memset(cd, 0, sizeof(*cd));
}

static void* xrealloc_or_die(void* p, size_t n)
{
  void* q = realloc(p, n);
  if (!q && n != 0)
    abort();
  return q;
}

static controller_data_t* push_controller(sdl_input_source_t* s)
{
  if (s->num_controllers == s->cap_controllers) {
    u32 newcap = s->cap_controllers ? s->cap_controllers * 2 : 4;
    s->controllers = (controller_data_t*)xrealloc_or_die(s->controllers,
                                                         newcap * sizeof(controller_data_t));
    s->cap_controllers = newcap;
  }
  controller_data_t* cd = &s->controllers[s->num_controllers++];
  memset(cd, 0, sizeof(*cd));
  return cd;
}

static void erase_controller(sdl_input_source_t* s, controller_data_t* cd)
{
  controller_data_free(cd);
  size_t idx = (size_t)(cd - s->controllers);
  size_t tail = s->num_controllers - idx - 1;
  if (tail > 0)
    memmove(cd, cd + 1, tail * sizeof(controller_data_t));
  s->num_controllers--;
}

static bool open_device(sdl_input_source_t* s, int sdl_index, bool is_gamecontroller)
{
  SDL_GameController* gc = NULL;
  SDL_Joystick*       joy = NULL;

  if (is_gamecontroller) {
    gc = SDL_GameControllerOpen(sdl_index);
    if (!gc) {
      ERROR_LOG("SDL_GameControllerOpen(%d) failed: %s", sdl_index, SDL_GetError());
      return false;
    }
    joy = SDL_GameControllerGetJoystick(gc);
  } else {
    joy = SDL_JoystickOpen(sdl_index);
    if (!joy) {
      ERROR_LOG("SDL_JoystickOpen(%d) failed: %s", sdl_index, SDL_GetError());
      return false;
    }
  }

  SDL_JoystickID jid = SDL_JoystickInstanceID(joy);
  if (find_by_joystick_id(s, jid)) {
    if (gc) SDL_GameControllerClose(gc); else SDL_JoystickClose(joy);
    return false;
  }

  controller_data_t* cd = push_controller(s);
  cd->gamepad     = gc;
  cd->joystick    = joy;
  cd->joystick_id = jid;
  cd->player_id   = get_free_player_id(s);
  cd->use_gamepad_rumble = (gc && SDL_GameControllerHasRumble(gc));

  if (joy) {
    int nh = SDL_JoystickNumHats(joy);
    cd->num_hats = (nh > 0) ? (u32)nh : 0;
    if (cd->num_hats > 0)
      cd->last_hat_state = (u8*)calloc(cd->num_hats, sizeof(u8));

    int nb = SDL_JoystickNumButtons(joy);
    cd->num_joy_buttons = (nb > 0) ? (u32)nb : 0;
    if (cd->num_joy_buttons > 0)
      cd->joy_button_used_in_gc = (bool*)calloc(cd->num_joy_buttons, sizeof(bool));

    int na = SDL_JoystickNumAxes(joy);
    cd->num_joy_axes = (na > 0) ? (u32)na : 0;
    if (cd->num_joy_axes > 0)
      cd->joy_axis_used_in_gc = (bool*)calloc(cd->num_joy_axes, sizeof(bool));

    if (gc) {
      /* Mark joystick buttons / axes that are mapped through the gamepad
       * layer so we don't double-fire. */
      for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(
          gc, (SDL_GameControllerButton)b);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
            (u32)bind.value.button < cd->num_joy_buttons) {
          cd->joy_button_used_in_gc[bind.value.button] = true;
        }
      }
      for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
        SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(
          gc, (SDL_GameControllerAxis)a);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_AXIS &&
            (u32)bind.value.axis < cd->num_joy_axes) {
          cd->joy_axis_used_in_gc[bind.value.axis] = true;
        }
      }
    }
  }

  const char* name = gc ? SDL_GameControllerName(gc) : SDL_JoystickName(joy);
  INFO_LOG("Controller %d ('%s') connected, %s rumble.", cd->player_id,
           name ? name : "Unknown",
           cd->use_gamepad_rumble ? "supports" : "no");

  /* Notify host (weakly linked stub by default). */
  char ident[32];
  snprintf(ident, sizeof(ident), "SDL-%d", cd->player_id);
  input_binding_key_t key =
    input_source_make_generic_device_key(INPUT_SOURCE_SDL, (u32)cd->player_id);
  host_on_input_device_connected(key, ident, name ? name : "Unknown");

  return true;
}

static bool close_device(sdl_input_source_t* s, SDL_JoystickID jid)
{
  controller_data_t* cd = find_by_joystick_id(s, jid);
  if (!cd)
    return false;

  char ident[32];
  snprintf(ident, sizeof(ident), "SDL-%d", cd->player_id);
  input_binding_key_t key =
    input_source_make_generic_device_key(INPUT_SOURCE_SDL, (u32)cd->player_id);

  input_manager_clear_bind_state_from_source(key);

  host_on_input_device_disconnected(key, ident);

  erase_controller(s, cd);
  return true;
}

static float axis_int_to_float(int16_t v)
{
  /* Clamp negative range so -32768 doesn't blow past -1.0. */
  return (v < 0) ? ((float)v / 32768.0f) : ((float)v / 32767.0f);
}

static void handle_button_event(sdl_input_source_t* s, const SDL_ControllerButtonEvent* ev)
{
  controller_data_t* cd = find_by_joystick_id(s, ev->which);
  if (!cd)
    return;
  input_binding_key_t key =
    input_source_make_generic_button_key(INPUT_SOURCE_SDL, (u32)cd->player_id, ev->button);
  input_manager_invoke_events(key, ev->state == SDL_PRESSED ? 1.0f : 0.0f,
                              ((u32)ev->button < SDL_CONTROLLER_BUTTON_MAX)
                                ? s_button_info[ev->button].generic
                                : GENERIC_INPUT_BINDING_UNKNOWN);
}

static void handle_axis_event(sdl_input_source_t* s, const SDL_ControllerAxisEvent* ev)
{
  controller_data_t* cd = find_by_joystick_id(s, ev->which);
  if (!cd)
    return;
  const float v = axis_int_to_float(ev->value);
  input_binding_key_t key =
    input_source_make_generic_axis_key(INPUT_SOURCE_SDL, (u32)cd->player_id, ev->axis);
  generic_input_binding_t generic = ((u32)ev->axis < SDL_CONTROLLER_AXIS_MAX)
    ? (v < 0.0f ? s_axis_info[ev->axis].generic_neg : s_axis_info[ev->axis].generic_pos)
    : GENERIC_INPUT_BINDING_UNKNOWN;
  input_manager_invoke_events(key, v, generic);
}

static void handle_joy_button_event(sdl_input_source_t* s, const SDL_JoyButtonEvent* ev)
{
  controller_data_t* cd = find_by_joystick_id(s, ev->which);
  if (!cd)
    return;
  if ((u32)ev->button < cd->num_joy_buttons && cd->joy_button_used_in_gc[ev->button])
    return; /* already fired through GC mapping */
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = INPUT_SOURCE_SDL;
  k.bits_.source_index   = (u32)cd->player_id & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_BUTTON;
  k.bits_.data           = (u32)ev->button + (u32)SDL_CONTROLLER_BUTTON_MAX;
  input_manager_invoke_events(k, ev->state == SDL_PRESSED ? 1.0f : 0.0f,
                              GENERIC_INPUT_BINDING_UNKNOWN);
}

static void handle_joy_axis_event(sdl_input_source_t* s, const SDL_JoyAxisEvent* ev)
{
  controller_data_t* cd = find_by_joystick_id(s, ev->which);
  if (!cd)
    return;
  if ((u32)ev->axis < cd->num_joy_axes && cd->joy_axis_used_in_gc[ev->axis])
    return;
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = INPUT_SOURCE_SDL;
  k.bits_.source_index   = (u32)cd->player_id & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
  k.bits_.data           = (u32)ev->axis + (u32)SDL_CONTROLLER_AXIS_MAX;
  input_manager_invoke_events(k, axis_int_to_float(ev->value), GENERIC_INPUT_BINDING_UNKNOWN);
}

static void handle_joy_hat_event(sdl_input_source_t* s, const SDL_JoyHatEvent* ev)
{
  controller_data_t* cd = find_by_joystick_id(s, ev->which);
  if (!cd || (u32)ev->hat >= cd->num_hats)
    return;

  static const u8 dir_bits[4] = {
    SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT,
  };
  const u8 prev = cd->last_hat_state[ev->hat];
  const u8 now  = ev->value;
  cd->last_hat_state[ev->hat] = now;

  for (u8 d = 0; d < 4; d++) {
    const bool was = (prev & dir_bits[d]) != 0;
    const bool is  = (now  & dir_bits[d]) != 0;
    if (was == is)
      continue;
    input_binding_key_t k = input_source_make_generic_hat_key(
      INPUT_SOURCE_SDL, (u32)cd->player_id, ev->hat, d, 4);
    input_manager_invoke_events(k, is ? 1.0f : 0.0f, GENERIC_INPUT_BINDING_UNKNOWN);
  }
}

static bool sdl_initialize(input_source_t* base, const settings_interface_t* si)
{
  (void)si;
  sdl_input_source_t* s = (sdl_input_source_t*)base;

  /* SDL_InitSubSystem is reference-counted; multiple init calls are fine. */
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    ERROR_LOG("SDL_InitSubSystem(JOYSTICK|GAMECONTROLLER) failed: %s", SDL_GetError());
    return false;
  }
  s->sdl_initialized = true;

  /* Open already-attached gamepads. */
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (SDL_IsGameController(i))
      open_device(s, i, true);
    else
      open_device(s, i, false);
  }

  return true;
}

static void sdl_update_settings(input_source_t* base, const settings_interface_t* si)
{
  (void)base; (void)si;
  /* Settings (LED colors, advanced hints) are out of scope. */
}

static bool sdl_reload_devices(input_source_t* base)
{
  (void)base;
  return false;
}

static void sdl_shutdown(input_source_t* base)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;

  while (s->num_controllers > 0)
    close_device(s, s->controllers[0].joystick_id);

  free(s->controllers);
  s->controllers     = NULL;
  s->num_controllers = 0;
  s->cap_controllers = 0;

  if (s->sdl_initialized) {
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    s->sdl_initialized = false;
  }
}

static void sdl_poll_events(input_source_t* base)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_CONTROLLERDEVICEADDED:
        open_device(s, ev.cdevice.which, true);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        close_device(s, ev.cdevice.which);
        break;
      case SDL_JOYDEVICEADDED:
        if (!SDL_IsGameController(ev.jdevice.which))
          open_device(s, ev.jdevice.which, false);
        break;
      case SDL_JOYDEVICEREMOVED:
        if (!find_by_joystick_id(s, ev.jdevice.which))
          break;
        close_device(s, ev.jdevice.which);
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP:
        handle_button_event(s, &ev.cbutton);
        break;
      case SDL_CONTROLLERAXISMOTION:
        handle_axis_event(s, &ev.caxis);
        break;
      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        handle_joy_button_event(s, &ev.jbutton);
        break;
      case SDL_JOYAXISMOTION:
        handle_joy_axis_event(s, &ev.jaxis);
        break;
      case SDL_JOYHATMOTION:
        handle_joy_hat_event(s, &ev.jhat);
        break;
      default:
        break;
    }
  }
}

static bool sdl_get_current_value(input_source_t* base, input_binding_key_t key, float* out_value)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  if (key.bits_.source_type != INPUT_SOURCE_SDL)
    return false;
  controller_data_t* cd = find_by_player_id(s, (int)key.bits_.source_index);
  if (!cd)
    return false;

  if (key.bits_.source_subtype == INPUT_SUBCLASS_CONTROLLER_BUTTON) {
    if (cd->gamepad && key.bits_.data < SDL_CONTROLLER_BUTTON_MAX) {
      *out_value = SDL_GameControllerGetButton(cd->gamepad, (SDL_GameControllerButton)key.bits_.data)
                     ? 1.0f
                     : 0.0f;
      return true;
    }
    return false;
  }
  if (key.bits_.source_subtype == INPUT_SUBCLASS_CONTROLLER_AXIS) {
    if (cd->gamepad && key.bits_.data < SDL_CONTROLLER_AXIS_MAX) {
      Sint16 v = SDL_GameControllerGetAxis(cd->gamepad, (SDL_GameControllerAxis)key.bits_.data);
      *out_value = axis_int_to_float(v);
      return true;
    }
    return false;
  }
  return false;
}

static bool sdl_contains_device(const input_source_t* base, const char* device)
{
  (void)base;
  return device && strncmp(device, "SDL-", 4) == 0;
}

static bool sdl_parse_key_string(input_source_t* base, const char* device, const char* binding,
                                 input_binding_key_t* out_key)
{
  (void)base;
  if (!device || strncmp(device, "SDL-", 4) != 0 || !binding || !*binding)
    return false;

  s32 player = 0;
  if (!string_util_from_chars_s32(device + 4, (u32)strlen(device + 4), 10, &player, NULL) ||
      player < 0)
    return false;

  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type  = INPUT_SOURCE_SDL;
  k.bits_.source_index = (u32)player & 0xFFu;

  size_t blen = strlen(binding);

  /* "{Large|Small}Motor" */
  if (blen >= 5 && memcmp(binding + blen - 5, "Motor", 5) == 0) {
    k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_MOTOR;
    if (strcmp(binding, "LargeMotor") == 0) {
      k.bits_.data = MOTOR_INDEX_LARGE; *out_key = k; return true;
    }
    if (strcmp(binding, "SmallMotor") == 0) {
      k.bits_.data = MOTOR_INDEX_SMALL; *out_key = k; return true;
    }
    return false;
  }

  if (binding[0] == '+' || binding[0] == '-') {
    const char* axis_name = binding + 1;
    k.bits_.modifier = (binding[0] == '-') ? INPUT_MODIFIER_NEGATE : INPUT_MODIFIER_NONE;

    if (strncmp(axis_name, "Axis", 4) == 0) {
      u32 idx;
      if (string_util_from_chars_u32(axis_name + 4, (u32)strlen(axis_name + 4), 10, &idx, NULL)) {
        k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
        k.bits_.data           = idx + (u32)SDL_CONTROLLER_AXIS_MAX;
        *out_key = k;
        return true;
      }
    }

    for (u32 i = 0; i < (u32)SDL_CONTROLLER_AXIS_MAX; i++) {
      if (strcmp(axis_name, s_axis_info[i].name) == 0) {
        k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
        k.bits_.data           = i;
        *out_key = k;
        return true;
      }
    }
    return false;
  }

  if (strncmp(binding, "FullAxis", 8) == 0) {
    u32 idx;
    if (string_util_from_chars_u32(binding + 8, (u32)strlen(binding + 8), 10, &idx, NULL)) {
      k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
      k.bits_.data           = idx + (u32)SDL_CONTROLLER_AXIS_MAX;
      k.bits_.modifier       = INPUT_MODIFIER_FULL_AXIS;
      *out_key = k;
      return true;
    }
    return false;
  }

  if (strncmp(binding, "Hat", 3) == 0) {
    u32 hat_idx;
    u32 consumed = 0;
    if (string_util_from_chars_u32(binding + 3, (u32)strlen(binding + 3), 10, &hat_idx, &consumed)) {
      const char* dir_part = binding + 3 + consumed;
      for (u8 d = 0; d < 4; d++) {
        if (strcmp(dir_part, s_hat_direction_names[d]) == 0) {
          k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_HAT;
          k.bits_.data           = hat_idx * 4u + d;
          *out_key = k;
          return true;
        }
      }
    }
    return false;
  }

  /* Plain "Button{N}" or named button. */
  if (strncmp(binding, "Button", 6) == 0) {
    u32 idx;
    if (string_util_from_chars_u32(binding + 6, (u32)strlen(binding + 6), 10, &idx, NULL)) {
      k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_BUTTON;
      k.bits_.data           = idx + (u32)SDL_CONTROLLER_BUTTON_MAX;
      *out_key = k;
      return true;
    }
  }

  for (u32 i = 0; i < (u32)SDL_CONTROLLER_BUTTON_MAX; i++) {
    if (strcmp(binding, s_button_info[i].name) == 0) {
      k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_BUTTON;
      k.bits_.data           = i;
      *out_key = k;
      return true;
    }
  }
  return false;
}

static void sdl_convert_key_to_string(input_source_t* base, input_binding_key_t key, small_string_t* out)
{
  (void)base;
  small_string_clear(out);
  if (key.bits_.source_type != INPUT_SOURCE_SDL)
    return;

  const u32 idx = key.bits_.source_index;
  const u32 d   = key.bits_.data;

  switch (key.bits_.source_subtype) {
    case INPUT_SUBCLASS_CONTROLLER_AXIS: {
      const char* mod = (key.bits_.modifier == INPUT_MODIFIER_FULL_AXIS) ? "Full"
                       : (key.bits_.modifier == INPUT_MODIFIER_NEGATE)   ? "-"
                                                                         : "+";
      if (d < (u32)SDL_CONTROLLER_AXIS_MAX)
        small_string_append_sprintf(out, "SDL-%u/%s%s", idx, mod, s_axis_info[d].name);
      else
        small_string_append_sprintf(out, "SDL-%u/%sAxis%u", idx, mod, d - (u32)SDL_CONTROLLER_AXIS_MAX);
      break;
    }
    case INPUT_SUBCLASS_CONTROLLER_BUTTON:
      if (d < (u32)SDL_CONTROLLER_BUTTON_MAX)
        small_string_append_sprintf(out, "SDL-%u/%s", idx, s_button_info[d].name);
      else
        small_string_append_sprintf(out, "SDL-%u/Button%u", idx, d - (u32)SDL_CONTROLLER_BUTTON_MAX);
      break;
    case INPUT_SUBCLASS_CONTROLLER_HAT:
      small_string_append_sprintf(out, "SDL-%u/Hat%u%s", idx, d / 4u, s_hat_direction_names[d % 4u]);
      break;
    case INPUT_SUBCLASS_CONTROLLER_MOTOR:
      small_string_append_sprintf(out, "SDL-%u/%sMotor", idx,
                                  (d == MOTOR_INDEX_SMALL) ? "Small" : "Large");
      break;
    default:
      break;
  }
}

static void sdl_enumerate_devices(input_source_t* base, input_device_list_t* out)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  out->items = NULL;
  out->count = 0;
  if (s->num_controllers == 0)
    return;

  out->items = (input_device_entry_t*)calloc(s->num_controllers, sizeof(input_device_entry_t));
  for (u32 i = 0; i < s->num_controllers; i++) {
    controller_data_t* cd = &s->controllers[i];
    char ident[32];
    snprintf(ident, sizeof(ident), "SDL-%d", cd->player_id);
    const char* name = cd->gamepad ? SDL_GameControllerName(cd->gamepad)
                                   : (cd->joystick ? SDL_JoystickName(cd->joystick) : "Unknown");
    out->items[i].key =
      input_source_make_generic_device_key(INPUT_SOURCE_SDL, (u32)cd->player_id);
    out->items[i].identifier   = strdup(ident);
    out->items[i].display_name = strdup(name ? name : "Unknown");
  }
  out->count = s->num_controllers;
}

static void sdl_enumerate_effects(input_source_t* base, int type, bool has_for_device,
                                  input_binding_key_t for_device, input_effect_list_t* out)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  out->items = NULL;
  out->count = 0;

  /* Only "Motor" effects are listed (no haptic effect chain). */
  if (type >= 0 && type != INPUT_BINDING_TYPE_MOTOR)
    return;

  size_t cap = 8;
  out->items = (input_effect_entry_t*)calloc(cap, sizeof(input_effect_entry_t));

  for (u32 i = 0; i < s->num_controllers; i++) {
    controller_data_t* cd = &s->controllers[i];
    if (!cd->use_gamepad_rumble)
      continue;
    if (has_for_device &&
        (for_device.bits_.source_type != INPUT_SOURCE_SDL ||
         for_device.bits_.source_index != (u32)cd->player_id))
      continue;

    if (out->count + 2 > cap) {
      cap *= 2;
      out->items = (input_effect_entry_t*)xrealloc_or_die(out->items,
                                                          cap * sizeof(input_effect_entry_t));
    }

    out->items[out->count].binding_type = INPUT_BINDING_TYPE_MOTOR;
    out->items[out->count].key =
      input_source_make_generic_motor_key(INPUT_SOURCE_SDL, (u32)cd->player_id, MOTOR_INDEX_LARGE);
    out->count++;

    out->items[out->count].binding_type = INPUT_BINDING_TYPE_MOTOR;
    out->items[out->count].key =
      input_source_make_generic_motor_key(INPUT_SOURCE_SDL, (u32)cd->player_id, MOTOR_INDEX_SMALL);
    out->count++;
  }
}

static u32 sdl_get_pollable_device_count(const input_source_t* base)
{
  const sdl_input_source_t* s = (const sdl_input_source_t*)base;
  return s->num_controllers;
}

static bool sdl_get_generic_binding_mapping(input_source_t* base, const char* device,
                                            generic_binding_mapping_t* mapping)
{
  (void)base;
  if (!device || strncmp(device, "SDL-", 4) != 0)
    return false;

  /* For each gamepad axis/button with a non-Unknown generic, push the
   * "{Source}{N}/+Axis" or "{Source}{N}/Button" form. We synthesise the
   * device prefix from the input string. */
  for (u32 i = 0; i < (u32)SDL_CONTROLLER_AXIS_MAX; i++) {
    if (s_axis_info[i].generic_neg != GENERIC_INPUT_BINDING_UNKNOWN) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s/-%s", device, s_axis_info[i].name);
      generic_binding_mapping_push(mapping, (u8)s_axis_info[i].generic_neg, buf);
    }
    if (s_axis_info[i].generic_pos != GENERIC_INPUT_BINDING_UNKNOWN) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s/+%s", device, s_axis_info[i].name);
      generic_binding_mapping_push(mapping, (u8)s_axis_info[i].generic_pos, buf);
    }
  }
  for (u32 i = 0; i < (u32)SDL_CONTROLLER_BUTTON_MAX; i++) {
    if (s_button_info[i].generic != GENERIC_INPUT_BINDING_UNKNOWN) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s/%s", device, s_button_info[i].name);
      generic_binding_mapping_push(mapping, (u8)s_button_info[i].generic, buf);
    }
  }
  /* Rumble */
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s/LargeMotor", device);
    generic_binding_mapping_push(mapping, (u8)GENERIC_INPUT_BINDING_LARGE_MOTOR, buf);
    snprintf(buf, sizeof(buf), "%s/SmallMotor", device);
    generic_binding_mapping_push(mapping, (u8)GENERIC_INPUT_BINDING_SMALL_MOTOR, buf);
  }
  return true;
}

static void send_rumble(controller_data_t* cd)
{
  if (cd->use_gamepad_rumble && cd->gamepad)
    SDL_GameControllerRumble(cd->gamepad, cd->rumble_intensity[0], cd->rumble_intensity[1],
                             RUMBLE_DURATION_MS);
}

static void sdl_update_motor_state(input_source_t* base, input_binding_key_t key, float intensity)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  if (key.bits_.source_subtype != INPUT_SUBCLASS_CONTROLLER_MOTOR)
    return;
  controller_data_t* cd = find_by_player_id(s, (int)key.bits_.source_index);
  if (!cd)
    return;

  const u32 motor = (key.bits_.data == MOTOR_INDEX_SMALL) ? 1u : 0u;
  const float clamped = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
  cd->rumble_intensity[motor] = (u16)lroundf(clamped * 65535.0f);
  send_rumble(cd);
}

static void sdl_update_motor_state_pair(input_source_t* base, input_binding_key_t lk,
                                        input_binding_key_t sk, float li, float si_)
{
  sdl_input_source_t* s = (sdl_input_source_t*)base;
  /* Both motors should belong to the same controller; pick the first non-zero
   * key to find it. */
  controller_data_t* cd = NULL;
  if (lk.bits != 0)
    cd = find_by_player_id(s, (int)lk.bits_.source_index);
  if (!cd && sk.bits != 0)
    cd = find_by_player_id(s, (int)sk.bits_.source_index);
  if (!cd)
    return;
  if (lk.bits != 0)
    cd->rumble_intensity[0] = (u16)lroundf((li < 0 ? 0 : (li > 1 ? 1 : li)) * 65535.0f);
  if (sk.bits != 0)
    cd->rumble_intensity[1] = (u16)lroundf((si_ < 0 ? 0 : (si_ > 1 ? 1 : si_)) * 65535.0f);
  send_rumble(cd);
}

static void sdl_update_led_state(input_source_t* base, input_binding_key_t key, float intensity)
{
  (void)base; (void)key; (void)intensity;
  /* LEDs out of scope. */
}

static const input_source_vtable_t s_sdl_vtable = {
   .initialize                  = sdl_initialize,
  .update_settings             = sdl_update_settings,
  .reload_devices              = sdl_reload_devices,
  .shutdown                    = sdl_shutdown,
  .poll_events                 = sdl_poll_events,
  .get_current_value           = sdl_get_current_value,
  .contains_device             = sdl_contains_device,
  .parse_key_string            = sdl_parse_key_string,
  .convert_key_to_string       = sdl_convert_key_to_string,
  .enumerate_devices           = sdl_enumerate_devices,
  .enumerate_effects           = sdl_enumerate_effects,
  .get_pollable_device_count   = sdl_get_pollable_device_count,
  .get_generic_binding_mapping = sdl_get_generic_binding_mapping,
  .update_motor_state          = sdl_update_motor_state,
  .update_motor_state_pair     = sdl_update_motor_state_pair,
  .update_led_state            = sdl_update_led_state, 
};

input_source_t* sdl_input_source_create(void)
{
  sdl_input_source_t* s = (sdl_input_source_t*)calloc(1, sizeof(sdl_input_source_t));
  if (!s)
    return NULL;
  s->base.vtbl = &s_sdl_vtable;
  return (input_source_t*)s;
}

/* Used by input_manager.c via input_source.h declaration. */
input_source_t* input_source_create_sdl(void)
{
  return sdl_input_source_create();
}
