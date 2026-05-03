/*
 *

 * Frontend keyboard / gamepad -> analog_controller bind-state mapping.
 * Encapsulates the controller_set_bind_state index calculation so main.c
 * stays focused on SDL plumbing.
 */
#ifndef CUPID_FRONTEND_INPUT_MAP_H
#define CUPID_FRONTEND_INPUT_MAP_H

#include "common/types.h"

#include <SDL.h>

void input_map_apply_keyboard(void);

/* Forward a SDL_KEYDOWN / SDL_KEYUP event into the local key-state cache.
 * Call this from the event-pump loop. */
void input_map_handle_key_event(const SDL_KeyboardEvent* ke);

/* Polls all attached SDL gamepads and pushes their state onto the analog
 * controller bound to slot 0.  Keyboard input is OR-merged in, so digital
 * keys and a controller can drive the pad simultaneously. */
void input_map_apply_gamepads(void);

 /* Open / close gamepad on connect / disconnect events forwarded from the
 * main event pump. */
void input_map_on_controller_added  (s32 device_index);
void input_map_on_controller_removed(SDL_JoystickID instance_id);

void input_map_open_all_gamepads (void);
void input_map_close_all_gamepads(void);

/* Mouse / lightgun plumbing.  set_window_size feeds the current window
 * dimensions (called at startup and on SDL_WINDOWEVENT_SIZE_CHANGED) so
 * window-pixel events can be normalized.  handle_mouse_motion gets the
 * current absolute window coords (for lightguns) and the per-event delta
 * (for the PSX mouse). */
void input_map_set_window_size       (s32 width, s32 height);
void input_map_handle_mouse_motion   (s32 x, s32 y, s32 xrel, s32 yrel);
void input_map_handle_mouse_button   (u8 sdl_button, bool pressed);

#endif /* CUPID_FRONTEND_INPUT_MAP_H */
