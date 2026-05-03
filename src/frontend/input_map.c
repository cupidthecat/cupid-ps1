/*
 * Keyboard + gamepad -> analog_controller bind-state mapping.  See
 * input_map.h for the public surface.
 *
 * Bind index layout (from core/analog_controller.h):
 *   0..16  : ANALOG_CONTROLLER_BUTTON_*
 *   17..24 : ANALOG_CONTROLLER_HALFAXIS_BIND_START_INDEX + half axis
 *   25..26 : motor binds (unused here)
 *   27     : LED bind (unused here)
 */

#include "frontend/input_map.h"

#include "core/analog_controller.h"
#include "core/controller.h"
#include "core/guncon.h"
#include "core/justifier.h"
#include "core/pad.h"
#include "core/playstation_mouse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  /* Half-axis bind indices, computed once. */
  HA_LLEFT  = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_LLEFT,
  HA_LRIGHT = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_LRIGHT,
  HA_LDOWN  = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_LDOWN,
  HA_LUP    = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_LUP,
  HA_RLEFT  = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_RLEFT,
  HA_RRIGHT = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_RRIGHT,
  HA_RDOWN  = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_RDOWN,
  HA_RUP    = (u32)ANALOG_CONTROLLER_BUTTON_COUNT + (u32)ANALOG_CONTROLLER_HALF_AXIS_RUP,
};

#define MAX_OPEN_GAMEPADS 4

typedef struct {
  SDL_GameController* gc;
  SDL_JoystickID      instance_id;
} open_gamepad_t;

static open_gamepad_t s_pads[MAX_OPEN_GAMEPADS];
static u32            s_pad_count;

void input_map_on_controller_added(s32 device_index)
{
  if (!SDL_IsGameController(device_index))
    return;
  if (s_pad_count >= MAX_OPEN_GAMEPADS)
    return;

  SDL_GameController* gc = SDL_GameControllerOpen(device_index);
  if (!gc)
    return;

  SDL_Joystick* j = SDL_GameControllerGetJoystick(gc);
  s_pads[s_pad_count].gc          = gc;
  s_pads[s_pad_count].instance_id = SDL_JoystickInstanceID(j);
  s_pad_count++;
}

void input_map_on_controller_removed(SDL_JoystickID instance_id)
{
  for (u32 i = 0; i < s_pad_count; i++) {
    if (s_pads[i].instance_id != instance_id)
      continue;
    SDL_GameControllerClose(s_pads[i].gc);
    /* Compact the array. */
    for (u32 j = i + 1; j < s_pad_count; j++)
      s_pads[j - 1] = s_pads[j];
    s_pad_count--;
    return;
  }
}

void input_map_open_all_gamepads(void)
{
  const int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++)
    input_map_on_controller_added((s32)i);
}

void input_map_close_all_gamepads(void)
{
  for (u32 i = 0; i < s_pad_count; i++) {
    if (s_pads[i].gc)
      SDL_GameControllerClose(s_pads[i].gc);
    s_pads[i].gc = NULL;
  }
  s_pad_count = 0;
}

static controller_t* get_pad0(void)
{
  return pad_get_controller(0);
}

static void set_btn(controller_t* c, u32 bind_index, bool pressed)
{
  controller_set_bind_state(c, bind_index, pressed ? 1.0f : 0.0f);
}

static void set_axis(controller_t* c, u32 bind_index, float value)
{
  if (value < 0.0f) value = 0.0f;
  if (value > 1.0f) value = 1.0f;
  controller_set_bind_state(c, bind_index, value);
}

static u8 s_kbd[SDL_NUM_SCANCODES];

void input_map_handle_key_event(const SDL_KeyboardEvent* ke)
{
  if (!ke) return;
  const SDL_Scancode sc = ke->keysym.scancode;
  if ((unsigned)sc >= (unsigned)SDL_NUM_SCANCODES) return;
  s_kbd[sc] = (ke->state == SDL_PRESSED) ? 1u : 0u;
}

/* Mouse / lightgun cache - fed by SDL_MOUSE* events from the main loop.
 * Lightguns read s_mouse_x/y / s_window_w/h to derive normalized [0..1]
 * coordinates each frame.  The PSX mouse drains s_mouse_dx/dy as deltas. */
static s32   s_window_w = 800;
static s32   s_window_h = 600;
static s32   s_mouse_x  = 0;
static s32   s_mouse_y  = 0;
static float s_mouse_dx = 0.0f;
static float s_mouse_dy = 0.0f;
static u8    s_mouse_btns;  /* bit 0 = left, bit 1 = middle, bit 2 = right */

void input_map_set_window_size(s32 width, s32 height)
{
  if (width  > 0) s_window_w = width;
  if (height > 0) s_window_h = height;
}

void input_map_handle_mouse_motion(s32 x, s32 y, s32 xrel, s32 yrel)
{
  s_mouse_x   = x;
  s_mouse_y   = y;
  s_mouse_dx += (float)xrel;
  s_mouse_dy += (float)yrel;
}

void input_map_handle_mouse_button(u8 sdl_button, bool pressed)
{
  u8 mask = 0;
  switch (sdl_button) {
    case SDL_BUTTON_LEFT:   mask = 0x1; break;
    case SDL_BUTTON_MIDDLE: mask = 0x2; break;
    case SDL_BUTTON_RIGHT:  mask = 0x4; break;
    default: return;
  }
  if (pressed) s_mouse_btns |= mask;
  else         s_mouse_btns &= (u8)~mask;
}

/* Set the four GunCon/Justifier RELATIVE_* half-axes to encode an absolute
 * normalized [0..1] window pointer.  gc_get_normalized_pointer() decodes
 * these back into the same (px, py) we feed gpu_query_light_gun_position. */
static void apply_lightgun_pointer(controller_t* c, u32 left_idx, u32 right_idx,
                                   u32 up_idx, u32 down_idx)
{
  const float nx = (s_window_w > 0) ? ((float)s_mouse_x / (float)s_window_w) : 0.5f;
  const float ny = (s_window_h > 0) ? ((float)s_mouse_y / (float)s_window_h) : 0.5f;

  const float cx = (nx * 2.0f) - 1.0f;  /* -1..1 */
  const float cy = (ny * 2.0f) - 1.0f;

  const float left  = (cx < 0.0f) ? -cx : 0.0f;
  const float right = (cx > 0.0f) ?  cx : 0.0f;
  const float up    = (cy < 0.0f) ? -cy : 0.0f;
  const float down  = (cy > 0.0f) ?  cy : 0.0f;

  controller_set_bind_state(c, left_idx,  left);
  controller_set_bind_state(c, right_idx, right);
  controller_set_bind_state(c, up_idx,    up);
  controller_set_bind_state(c, down_idx,  down);
}

static void apply_lightgun_buttons(controller_t* c, u32 trigger_idx,
                                   u32 alt1_idx, u32 alt2_idx)
{
  controller_set_bind_state(c, trigger_idx, (s_mouse_btns & 0x1) ? 1.0f : 0.0f);
  controller_set_bind_state(c, alt1_idx,    (s_mouse_btns & 0x4) ? 1.0f : 0.0f);
  controller_set_bind_state(c, alt2_idx,    (s_mouse_btns & 0x2) ? 1.0f : 0.0f);
}

static void apply_guncon(controller_t* c)
{
  apply_lightgun_pointer(c,
                         (u32)GUNCON_BIND_RELATIVE_LEFT,  (u32)GUNCON_BIND_RELATIVE_RIGHT,
                         (u32)GUNCON_BIND_RELATIVE_UP,    (u32)GUNCON_BIND_RELATIVE_DOWN);
  /* Trigger=L-mouse, A=R-mouse, B=middle.  ShootOffscreen on RShift. */
  apply_lightgun_buttons(c,
                         (u32)GUNCON_BIND_TRIGGER, (u32)GUNCON_BIND_A, (u32)GUNCON_BIND_B);
  controller_set_bind_state(c, (u32)GUNCON_BIND_SHOOT_OFFSCREEN,
                            s_kbd[SDL_SCANCODE_RSHIFT] ? 1.0f : 0.0f);
}

static void apply_justifier(controller_t* c)
{
  apply_lightgun_pointer(c,
                         (u32)JUSTIFIER_BIND_RELATIVE_LEFT,  (u32)JUSTIFIER_BIND_RELATIVE_RIGHT,
                         (u32)JUSTIFIER_BIND_RELATIVE_UP,    (u32)JUSTIFIER_BIND_RELATIVE_DOWN);
  /* Trigger=L-mouse, Start=R-mouse, Back=middle.  ShootOffscreen on RShift. */
  apply_lightgun_buttons(c,
                         (u32)JUSTIFIER_BIND_TRIGGER, (u32)JUSTIFIER_BIND_START, (u32)JUSTIFIER_BIND_BACK);
  controller_set_bind_state(c, (u32)JUSTIFIER_BIND_SHOOT_OFFSCREEN,
                            s_kbd[SDL_SCANCODE_RSHIFT] ? 1.0f : 0.0f);
}

static void apply_psmouse(controller_t* c)
{
  /* Drain accumulated motion; floor() in the SIO handler turns the rest
   * into a residual that carries to the next packet. */
  if (s_mouse_dx != 0.0f) {
    controller_set_bind_state(c, (u32)PSMOUSE_BIND_POINTER_X, s_mouse_dx);
    s_mouse_dx = 0.0f;
  }
  if (s_mouse_dy != 0.0f) {
    controller_set_bind_state(c, (u32)PSMOUSE_BIND_POINTER_Y, s_mouse_dy);
    s_mouse_dy = 0.0f;
  }
  controller_set_bind_state(c, (u32)PSMOUSE_BIND_LEFT,  (s_mouse_btns & 0x1) ? 1.0f : 0.0f);
  controller_set_bind_state(c, (u32)PSMOUSE_BIND_RIGHT, (s_mouse_btns & 0x4) ? 1.0f : 0.0f);
}

#include "core/negcon.h"
#include "core/jogcon.h"
#include "core/ddgo_controller.h"

static void apply_keyboard_standard(controller_t* c, const u8* k)
{
  /* D-pad. */
  set_btn(c, ANALOG_CONTROLLER_BUTTON_UP,    k[SDL_SCANCODE_UP]    != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_DOWN,  k[SDL_SCANCODE_DOWN]  != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_LEFT,  k[SDL_SCANCODE_LEFT]  != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_RIGHT, k[SDL_SCANCODE_RIGHT] != 0);

  /* Face buttons. */
  set_btn(c, ANALOG_CONTROLLER_BUTTON_CROSS,    k[SDL_SCANCODE_Z] != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_CIRCLE,   k[SDL_SCANCODE_X] != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_SQUARE,   k[SDL_SCANCODE_A] != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_TRIANGLE, k[SDL_SCANCODE_S] != 0);

  /* Shoulder buttons. */
  set_btn(c, ANALOG_CONTROLLER_BUTTON_L1, k[SDL_SCANCODE_LSHIFT] != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_L2, k[SDL_SCANCODE_LCTRL]  != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_R1, k[SDL_SCANCODE_RSHIFT] != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_R2, k[SDL_SCANCODE_RCTRL]  != 0);

  /* Start / Select. */
  set_btn(c, ANALOG_CONTROLLER_BUTTON_START,  k[SDL_SCANCODE_RETURN]    != 0);
  set_btn(c, ANALOG_CONTROLLER_BUTTON_SELECT, k[SDL_SCANCODE_BACKSPACE] != 0);

  set_axis(c, HA_LLEFT,  k[SDL_SCANCODE_A] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_LRIGHT, k[SDL_SCANCODE_D] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_LUP,    k[SDL_SCANCODE_W] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_LDOWN,  k[SDL_SCANCODE_S] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_RLEFT,  k[SDL_SCANCODE_J] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_RRIGHT, k[SDL_SCANCODE_L] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_RUP,    k[SDL_SCANCODE_I] != 0 ? 1.0f : 0.0f);
  set_axis(c, HA_RDOWN,  k[SDL_SCANCODE_K] != 0 ? 1.0f : 0.0f);
}

 /* Negcon racing pad: 8 buttons (start/dpad/R/B/A) + steering axis (half-axes
 * LEFT/RIGHT) + I/II throttle/brake half-axes + L paddle.  See core/negcon.h. */
static void apply_keyboard_negcon(controller_t* c, const u8* k)
{
  const u32 hax_steer_l = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_LEFT;
  const u32 hax_steer_r = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_RIGHT;
  const u32 hax_i       = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_I;
  const u32 hax_ii      = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_II;
  const u32 hax_l       = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_L;

  set_btn(c, NEGCON_BUTTON_UP,    k[SDL_SCANCODE_UP]     != 0);
  set_btn(c, NEGCON_BUTTON_DOWN,  k[SDL_SCANCODE_DOWN]   != 0);
  set_btn(c, NEGCON_BUTTON_LEFT,  k[SDL_SCANCODE_LEFT]   != 0);
  set_btn(c, NEGCON_BUTTON_RIGHT, k[SDL_SCANCODE_RIGHT]  != 0);
  set_btn(c, NEGCON_BUTTON_START, k[SDL_SCANCODE_RETURN] != 0);
  set_btn(c, NEGCON_BUTTON_A,     k[SDL_SCANCODE_Z]      != 0);
  set_btn(c, NEGCON_BUTTON_B,     k[SDL_SCANCODE_X]      != 0);
  set_btn(c, NEGCON_BUTTON_R,     k[SDL_SCANCODE_RSHIFT] != 0);

  set_axis(c, hax_steer_l, k[SDL_SCANCODE_A]      != 0 ? 1.0f : 0.0f);
  set_axis(c, hax_steer_r, k[SDL_SCANCODE_D]      != 0 ? 1.0f : 0.0f);
  set_axis(c, hax_i,       k[SDL_SCANCODE_W]      != 0 ? 1.0f : 0.0f);
  set_axis(c, hax_ii,      k[SDL_SCANCODE_S]      != 0 ? 1.0f : 0.0f);
  set_axis(c, hax_l,       k[SDL_SCANCODE_LSHIFT] != 0 ? 1.0f : 0.0f);
}

/* JogCon: standard PS1 layout 0-15 + MODE=16 + jog wheel half-axes. */
static void apply_keyboard_jogcon(controller_t* c, const u8* k)
{
  apply_keyboard_standard(c, k);
  set_btn(c, JOGCON_BUTTON_MODE, k[SDL_SCANCODE_TAB] != 0);
  const u32 hax_l = JOGCON_HALFAXIS_BIND_START_INDEX + (u32)JOGCON_HALFAXIS_STEERING_LEFT;
  const u32 hax_r = JOGCON_HALFAXIS_BIND_START_INDEX + (u32)JOGCON_HALFAXIS_STEERING_RIGHT;
  set_axis(c, hax_l, k[SDL_SCANCODE_Q] != 0 ? 1.0f : 0.0f);
  set_axis(c, hax_r, k[SDL_SCANCODE_E] != 0 ? 1.0f : 0.0f);
}

 /* Densha de Go: virtual lever positions on number row keys, Start/Select
 * on Enter/Backspace, A/B/C buttons on Z/X/C. */
static void apply_keyboard_ddgo(controller_t* c, const u8* k)
{
  set_btn(c, DDGO_BIND_START,  k[SDL_SCANCODE_RETURN]    != 0);
  set_btn(c, DDGO_BIND_SELECT, k[SDL_SCANCODE_BACKSPACE] != 0);
  set_btn(c, DDGO_BIND_A, k[SDL_SCANCODE_Z] != 0);
  set_btn(c, DDGO_BIND_B, k[SDL_SCANCODE_X] != 0);
  set_btn(c, DDGO_BIND_C, k[SDL_SCANCODE_C] != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_OFF, k[SDL_SCANCODE_GRAVE] != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_1,   k[SDL_SCANCODE_1]     != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_2,   k[SDL_SCANCODE_2]     != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_3,   k[SDL_SCANCODE_3]     != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_4,   k[SDL_SCANCODE_4]     != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_POWER_5,   k[SDL_SCANCODE_5]     != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_RELEASED, k[SDL_SCANCODE_6] != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_1, k[SDL_SCANCODE_7]      != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_2, k[SDL_SCANCODE_8]      != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_3, k[SDL_SCANCODE_9]      != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_4, k[SDL_SCANCODE_0]      != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_5, k[SDL_SCANCODE_MINUS]  != 0);
  set_btn(c, DDGO_BIND_VIRTUAL_BRAKE_6, k[SDL_SCANCODE_EQUALS] != 0);
}

void input_map_apply_keyboard(void)
{
  controller_t* c = get_pad0();
  if (!c)
    return;

  const u8* k = s_kbd;
  switch (controller_get_type(c)) {
    case CONTROLLER_TYPE_NEGCON:
    case CONTROLLER_TYPE_NEGCON_RUMBLE:
      apply_keyboard_negcon(c, k);
      break;
    case CONTROLLER_TYPE_JOGCON:
      apply_keyboard_jogcon(c, k);
      break;
    case CONTROLLER_TYPE_DDGO_CONTROLLER:
      apply_keyboard_ddgo(c, k);
      break;
    case CONTROLLER_TYPE_GUNCON:
      apply_guncon(c);
      break;
    case CONTROLLER_TYPE_JUSTIFIER:
      apply_justifier(c);
      break;
    case CONTROLLER_TYPE_PLAYSTATION_MOUSE:
      apply_psmouse(c);
      break;
    default:
      apply_keyboard_standard(c, k);
      break;
  }
}

static float scale_axis(Sint16 v)
{
  /* SDL axis -> 0.0..1.0 (positive direction only).  Negative is fed into
   * the opposite half-axis, so we clamp to >=0 here. */
  if (v <= 0) return 0.0f;
  return (float)v * (1.0f / 32767.0f);
}

static void apply_one_pad(SDL_GameController* gc)
{
  controller_t* c = get_pad0();
  if (!c)
    return;

  /* OR semantics: only press if SDL says pressed.  We don't reset to 0 here
   * because the keyboard pass already set the baseline; instead we only
   * push 1.0 when the gamepad button is down so it can override 0.0. */
#define MAYBE(bidx, gcbtn) do {                                             \
    if (SDL_GameControllerGetButton(gc, (gcbtn))) {                         \
      controller_set_bind_state(c, (u32)(bidx), 1.0f);                      \
    }                                                                       \
  } while (0)

  MAYBE(ANALOG_CONTROLLER_BUTTON_UP,       SDL_CONTROLLER_BUTTON_DPAD_UP);
  MAYBE(ANALOG_CONTROLLER_BUTTON_DOWN,     SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  MAYBE(ANALOG_CONTROLLER_BUTTON_LEFT,     SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  MAYBE(ANALOG_CONTROLLER_BUTTON_RIGHT,    SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

  MAYBE(ANALOG_CONTROLLER_BUTTON_CROSS,    SDL_CONTROLLER_BUTTON_A);
  MAYBE(ANALOG_CONTROLLER_BUTTON_CIRCLE,   SDL_CONTROLLER_BUTTON_B);
  MAYBE(ANALOG_CONTROLLER_BUTTON_SQUARE,   SDL_CONTROLLER_BUTTON_X);
  MAYBE(ANALOG_CONTROLLER_BUTTON_TRIANGLE, SDL_CONTROLLER_BUTTON_Y);

  MAYBE(ANALOG_CONTROLLER_BUTTON_L1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  MAYBE(ANALOG_CONTROLLER_BUTTON_R1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);

  MAYBE(ANALOG_CONTROLLER_BUTTON_START,  SDL_CONTROLLER_BUTTON_START);
  MAYBE(ANALOG_CONTROLLER_BUTTON_SELECT, SDL_CONTROLLER_BUTTON_BACK);

  MAYBE(ANALOG_CONTROLLER_BUTTON_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
  MAYBE(ANALOG_CONTROLLER_BUTTON_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);

  MAYBE(ANALOG_CONTROLLER_BUTTON_ANALOG, SDL_CONTROLLER_BUTTON_GUIDE);

#undef MAYBE

  /* Triggers: 0..32767 -> 0..1, drive L2/R2 as analog buttons. */
  const Sint16 lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  const Sint16 rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  if (lt > 8192)
    controller_set_bind_state(c, (u32)ANALOG_CONTROLLER_BUTTON_L2, scale_axis(lt));
  if (rt > 8192)
    controller_set_bind_state(c, (u32)ANALOG_CONTROLLER_BUTTON_R2, scale_axis(rt));

  /* Sticks. */
  const Sint16 lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
  const Sint16 ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
  const Sint16 rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
  const Sint16 ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);

  /* Each direction wins independently; values overlap with the keyboard
   * pass which is fine because set_bind_state is idempotent. */
  if (lx < 0) controller_set_bind_state(c, HA_LLEFT,  scale_axis((Sint16)-lx));
  if (lx > 0) controller_set_bind_state(c, HA_LRIGHT, scale_axis(lx));
  if (ly < 0) controller_set_bind_state(c, HA_LUP,    scale_axis((Sint16)-ly));
  if (ly > 0) controller_set_bind_state(c, HA_LDOWN,  scale_axis(ly));

  if (rx < 0) controller_set_bind_state(c, HA_RLEFT,  scale_axis((Sint16)-rx));
  if (rx > 0) controller_set_bind_state(c, HA_RRIGHT, scale_axis(rx));
  if (ry < 0) controller_set_bind_state(c, HA_RUP,    scale_axis((Sint16)-ry));
  if (ry > 0) controller_set_bind_state(c, HA_RDOWN,  scale_axis(ry));
}

void input_map_apply_gamepads(void)
{
  for (u32 i = 0; i < s_pad_count; i++) {
    if (s_pads[i].gc)
      apply_one_pad(s_pads[i].gc);
  }
}
