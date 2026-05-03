/*
 * AArch64 backend for the cupid-ps1 CPU recompiler.  Mirrors x64_backend.h.
 *
 * Whole TU is gated on __aarch64__; on x86_64 hosts the registration
 * symbols expand to no-ops so cpu_code_cache.c's startup path links
 * either backend independent of host arch.
 */

#ifndef CUPID_CORE_ARM64_BACKEND_H
#define CUPID_CORE_ARM64_BACKEND_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the arm64 backend.  Sets g_cpu_compiler + emit_jump +
 * emit_asm_functions callbacks on cpu_code_cache.  Idempotent. */
void arm64_backend_register(void);

/* Tears down the arm64 backend.  Symmetric with x64_backend_shutdown. */
void arm64_backend_shutdown(void);

/* Emit the dispatcher prelude into `code` (capacity `code_size`).  Returns
 * bytes emitted; populates the six g_* dispatcher globals as a side
 * effect.  Exported for byte-walk unit tests. */
u32 arm64_backend_emit_dispatcher_for_test(void* code, u32 code_size);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_ARM64_BACKEND_H */
