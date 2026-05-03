/* Stub frontend host hooks for the test binary.  The library-side code
 * (system.c, input_manager.c) calls into these expecting the SDL2 frontend
 * to provide them; tests don't link the frontend, so we provide no-ops. */

#include "common/types.h"

void host_pump_messages_on_core_thread(void);
void host_input_manager_poll_sources  (void);
void host_wait_for_all_async_tasks    (void);

void host_pump_messages_on_core_thread(void) {}
void host_input_manager_poll_sources  (void) {}
void host_wait_for_all_async_tasks    (void) {}

/* Texture-cache hook called from system.c on game-serial change.  The full
 * gpu_hw_texture_cache.o is filtered out of the test link (it pulls in GL),
 * so we stub it here. */
void gpu_texture_cache_game_serial_changed(void);
void gpu_texture_cache_game_serial_changed(void) {}
