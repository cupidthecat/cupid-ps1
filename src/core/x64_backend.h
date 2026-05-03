/*
 * x86_64 backend for the CPU recompiler.  Provides:
 *   - Dispatcher prelude (g_enter_recompiler, g_dispatcher, etc.) emitted
 *     into the code cache at startup.
 *   - Vtable hooks for the arch-agnostic recompiler base layer
 *     (cpu_recompiler.{c,h}).
 *   - emit_jump callback for cpu_code_cache's block-link backpatcher.
 */

#ifndef CUPID_CORE_X64_BACKEND_H
#define CUPID_CORE_X64_BACKEND_H

#include "common/types.h"
#include "core/x64_emit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the x64 backend.  Sets g_cpu_compiler, registers emit_jump and
 * emit_asm_functions callbacks on cpu_code_cache, and (via the next
 * cpu_code_cache_reset) emits the dispatcher prelude.  Idempotent. */
void x64_backend_register(void);

/* Tears down the x64 backend.  Clears g_cpu_compiler and the registered
 * callbacks.  Code-cache JIT memory is owned by cpu_code_cache, not us. */
void x64_backend_shutdown(void);

/* Emit the dispatcher prelude into `code` (capacity `code_size`).  Returns
 * bytes emitted; populates the six g_* dispatcher globals as a side
 * effect.  Normally invoked indirectly via cpu_code_cache_reset; exported
 * here for byte-walk unit tests. */
u32 x64_backend_emit_dispatcher_for_test(void* code, u32 code_size);

/* Backpatch tick adjustment.  When a fastmem load faults and the
 * page-fault handler patches the site to a slow-path stub, the stub's
 * call to the bus handler will add `BUS_RAM_READ_TICKS` at runtime --
 * but the recompiler already pre-charged the same amount into r->cycles
 * at compile time (see hook_compile_lxx fastmem branch).  To avoid the
 * double-count, the stub emits `sub [pending_ticks], BUS_RAM_READ_TICKS`
 * at its head when patching a load.  This helper writes that one
 * instruction; opaque to non-x64 callers (cpu_code_cache.c). */
void x64_backend_emit_sub_pending_ticks(x64_emit_t* e, s32 amount);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_X64_BACKEND_H */
