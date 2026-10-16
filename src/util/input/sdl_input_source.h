/*
 * Linux SDL2 gamepad backend.  Scope is the minimum needed to bind a
 * controller to the PS1 pad: open / close on hot-plug, button + axis +
 * hat events, basic SDL_GameControllerRumble.  No LED/RGB/sensor/touchpad
 * support.
 *
 * Consumed by input_manager.c's source factory; no public symbols other
 * than the factory are exported.
 */

#ifndef CUPID_UTIL_SDL_INPUT_SOURCE_H
#define CUPID_UTIL_SDL_INPUT_SOURCE_H

#include "input_source.h"

input_source_t* sdl_input_source_create(void);

#endif /* CUPID_UTIL_SDL_INPUT_SOURCE_H */
