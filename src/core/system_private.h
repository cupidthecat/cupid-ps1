/*
 * Internal system hooks for frame accounting, frame completion, memory-card
 * access and abnormal shutdown.
 */

#ifndef CUPID_CORE_SYSTEM_PRIVATE_H
#define CUPID_CORE_SYSTEM_PRIVATE_H

#include "core/system.h"

/* Frame number bumps (called by gpu / frame-done plumbing). */
void system_increment_frame_number          (void);
void system_increment_internal_frame_number (void);

/* Called by gpu_backend at the end of a guest vsync. */
void system_frame_done(void);

/* Called by memory_card.c on read/write access to fast-forward briefly. */
void system_on_memory_card_accessed(void);

/* Forces an immediate transition to Stopping; the caller's outer loop then
 * tears the system down. */
void system_abnormal_shutdown(const char* reason);

#endif /* CUPID_CORE_SYSTEM_PRIVATE_H */
