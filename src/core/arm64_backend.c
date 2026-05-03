/*
 * AArch64 backend for the cupid-ps1 CPU recompiler.  Mirrors x64_backend.c.
 * Whole TU is gated on __aarch64__ so x86_64 hosts compile an empty TU.
 *
 * Current scope (foundation):
 *   - Dispatcher prelude (g_enter_recompiler / g_dispatcher /
 *     g_compile_or_revalidate / g_discard_and_recompile / g_interpret_block /
 *     g_run_events_and_dispatch).
 *   - Lifecycle hooks (reset / begin_block / end_compile / end_block /
 *     end_block_with_exception) with cycle accounting.
 *   - Host-reg primitives (load constant / load from ptr / store to ptr /
 *     store constant / copy / flush).
 *   - compile_fallback uses the M6 end-block-and-interpret path so all
 *     unimplemented per-MIPS-op hooks route through the uncached-block
 *     interpreter.
 *
 * Out of scope (TODO, follow-up landings):
 *   - Per-MIPS-op compile_* hooks (ALU/shifts/branches/mul-div/lxx-sxx/
 *     COP0/COP2).  Until they land, every op falls back to interpreter
 *     so blocks compile to a tiny stub that hands off to g_interpret_block.
 *   - generate_block_protect_check / generate_icache_check_and_update.
 *   - Fastmem load/store sites.
 *   - PGXP CPU-mode call.
 */

#include "arm64_backend.h"

#if defined(__aarch64__)

#include "arm64_emit.h"
#include "bus.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler.h"
#include "cpu_types.h"
#include "settings.h"
#include "timing_event.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/memmap.h"

#include <stddef.h>
#include <string.h>

LOG_CHANNEL(Recompiler);

#define RWRET    ARM64_X0
#define RWARG1   ARM64_X0
#define RXARG1   ARM64_X0
#define RWARG2   ARM64_X1
#define RXARG2   ARM64_X1
#define RWARG3   ARM64_X2
#define RXARG3   ARM64_X2
#define RWARG4   ARM64_X3
#define RXARG4   ARM64_X3

#define RSTATE   ARM64_X19  /* &g_cpu_state, callee-saved */
#define RMEMBASE ARM64_X20  /* fastmem base, callee-saved */
#define RTMP1    ARM64_X16
#define RTMP2    ARM64_X17

#define STATE_DISP(field) ((s32)offsetof(cpu_state_t, field))

typedef struct arm64_backend_state {
  cpu_recompiler_t base;
  arm64_emit_t     near_emit;
  arm64_emit_t     far_emit;
  bool             is_far;
  u8*              block_near_start;
} arm64_backend_state_t;

static arm64_backend_state_t s_backend;

static inline arm64_backend_state_t* be_from(cpu_recompiler_t* r)
{
  return (arm64_backend_state_t*)r;
}

static inline arm64_emit_t* current_emit(arm64_backend_state_t* b)
{
  return b->is_far ? &b->far_emit : &b->near_emit;
}

static inline arm64_mem_t state_mem(s32 off)
{
  arm64_mem_t m = { .base = RSTATE, .offset = off };
  return m;
}

static void hook_reset(cpu_recompiler_t* r, cpu_code_cache_block_t* block,
                       u8* code, u32 code_size, u8* far_code, u32 far_code_size)
{
  cpu_recompiler_reset_state(r, block, code, code_size, far_code, far_code_size);

  arm64_backend_state_t* b = be_from(r);

  cpu_code_cache_align_code(CPU_RECOMP_FUNCTION_ALIGNMENT);

  u8* const near_buf = cpu_code_cache_get_free_code_pointer();
  const u32 near_cap = cpu_code_cache_get_free_code_space();
  u8* const far_buf  = cpu_code_cache_get_free_far_code_pointer();
  const u32 far_cap  = cpu_code_cache_get_free_far_code_space();

  arm64_emit_init(&b->near_emit, near_buf, near_cap);
  arm64_emit_init(&b->far_emit,  far_buf,  far_cap);
  b->is_far = false;
  b->block_near_start = near_buf;

  /* AAPCS64 callee-saved: X19-X28, X29 (FP), X30 (LR), SP.
   * Reserved (non-USABLE for the allocator): X0-X3 (call args / return),
   * X16/X17 (IP0/IP1 = inline-call temps), X18 (platform reg, conservatively
   * skipped), X19 (RSTATE), X20 (RMEMBASE), X29 (FP), X30 (LR), SP (31). */
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    ra->flags &= (u8)CPU_RC_HR_IMMUTABLE_FLAGS;
    if (i <= ARM64_X3 || i == ARM64_X16 || i == ARM64_X17 || i == ARM64_X18 ||
        i == ARM64_X19 || i == ARM64_X20 || i == ARM64_X29 || i == ARM64_X30 ||
        i >= 31u) {
      ra->flags &= (u8)~CPU_RC_HR_USABLE;
      continue;
    }
    u8 fl = (u8)CPU_RC_HR_USABLE;
    /* X21..X28 are callee-saved per AAPCS64. */
    if (i >= ARM64_X21 && i <= ARM64_X28) fl |= (u8)CPU_RC_HR_CALLEE_SAVED;
    ra->flags |= fl;
  }
}

static void hook_begin_block(cpu_recompiler_t* r)
{
  arm64_emit_t* e = current_emit(be_from(r));
  if (cpu_code_cache_is_using_fastmem())
    arm64_ldr_r_m(e, ARM64_SZ_64, RMEMBASE,
                  state_mem(STATE_DISP(fastmem_base)));
}

static const void* hook_end_compile(cpu_recompiler_t* r,
                                    u32* out_code_size, u32* out_far_code_size)
{
  arm64_backend_state_t* b = be_from(r);
  Assert(!b->near_emit.overflow);
  Assert(!b->far_emit.overflow);

  const void* code     = b->block_near_start;
  const u32 near_size  = (u32)arm64_emit_size(&b->near_emit);
  const u32 far_size   = (u32)arm64_emit_size(&b->far_emit);

  cpu_code_cache_commit_code(near_size);
  cpu_code_cache_commit_far_code(far_size);

  *out_code_size     = near_size;
  *out_far_code_size = far_size;
  b->block_near_start = NULL;
  return code;
}

/* Globals populated by the dispatcher prelude. */
extern const void* g_compile_or_revalidate_block;
extern const void* g_discard_and_recompile_block;
extern const void* g_dispatcher;
extern const void* g_interpret_block;
extern const void* g_run_events_and_dispatch;

static void emit_end_and_link_block(cpu_recompiler_t* r,
                                    const u32* newpc, bool do_event_test)
{
  arm64_backend_state_t* b = be_from(r);
  arm64_emit_t* e = current_emit(b);

  DebugAssert(!r->dirty_pc && !r->block_ended);
  r->block_ended = true;

  const s32 cycles = r->cycles;
  r->cycles = 0;

  /* Add accumulated cycles to pending_ticks. */
  if (cycles > 0) {
    arm64_ldr_r_m(e, ARM64_SZ_32, RWARG1,
                  state_mem(STATE_DISP(pending_ticks)));
    arm64_alu_r_r_imm(e, ARM64_ALU_ADD, ARM64_SZ_32, RWARG1, RWARG1, (u32)cycles);
    arm64_str_r_m(e, ARM64_SZ_32, RWARG1,
                  state_mem(STATE_DISP(pending_ticks)));
  }

  if (newpc != NULL) {
    arm64_mov_r_imm32(e, RWARG1, *newpc);
    arm64_str_r_m(e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pc)));
  }

  if (do_event_test) {
    arm64_ldr_r_m(e, ARM64_SZ_32, RWARG1,
                  state_mem(STATE_DISP(pending_ticks)));
    arm64_ldr_r_m(e, ARM64_SZ_32, RWARG2,
                  state_mem(STATE_DISP(downcount)));
    arm64_cmp_r_r(e, ARM64_SZ_32, RWARG1, RWARG2);
    /* B.GE → run events; otherwise fall through to dispatcher. */
    u32* jge = arm64_b_cond_rel19(e, ARM64_CC_GE);
    arm64_patch_rel19(jge, g_run_events_and_dispatch);
  }

  /* Tail: jump to dispatcher.  Use abs64 so this works regardless of where
   * the block sits relative to the prelude.  TODO: optimize to direct
   * block-link patch like x64 once block-link infrastructure ports. */
  arm64_jmp_abs64(e, g_dispatcher, RTMP1);
}

static void hook_end_block(cpu_recompiler_t* r, const u32* newpc, bool do_event_test)
{
  emit_end_and_link_block(r, newpc, do_event_test);
}

static void hook_end_block_with_exception(cpu_recompiler_t* r, cpu_exception_t excode)
{
  arm64_emit_t* e = current_emit(be_from(r));

  cpu_recompiler_flush(r,
    CPU_RC_FLUSH_END_BLOCK | CPU_RC_FLUSH_FOR_EXCEPTION | CPU_RC_FLUSH_FOR_C_CALL);

  const u32 cause = cpu_cop0_cause_make(
    excode, r->current_instruction_branch_delay_slot,
    /*BT*/ false, (u8)r->inst->cop.cop_n);
  arm64_mov_r_imm32(e, RWARG1, cause);
  arm64_mov_r_imm32(e, RWARG2, r->current_instruction_pc);

  if (excode != CPU_EXCEPTION_BP) {
    arm64_call_abs64(e, (const void*)&cpu_raise_exception_bits, RTMP1);
  } else {
    arm64_mov_r_imm32(e, RWARG3, r->inst->bits);
    arm64_call_abs64(e, (const void*)&cpu_raise_break_exception, RTMP1);
  }

  r->dirty_pc = false;
  emit_end_and_link_block(r, NULL, /*do_event_test=*/true);
}

static const void* hook_get_current_code_pointer(cpu_recompiler_t* r)
{
  return arm64_emit_cursor(current_emit(be_from(r)));
}

static void hook_generate_call(cpu_recompiler_t* r, const void* func,
                               s32 a1reg, s32 a2reg, s32 a3reg)
{
  arm64_emit_t* e = current_emit(be_from(r));
  if (a1reg >= 0 && a1reg != (s32)RXARG1)
    arm64_mov_r_r(e, ARM64_SZ_64, RXARG1, (arm64_reg_t)a1reg);
  if (a2reg >= 0 && a2reg != (s32)RXARG2)
    arm64_mov_r_r(e, ARM64_SZ_64, RXARG2, (arm64_reg_t)a2reg);
  if (a3reg >= 0 && a3reg != (s32)RXARG3)
    arm64_mov_r_r(e, ARM64_SZ_64, RXARG3, (arm64_reg_t)a3reg);
  arm64_call_abs64(e, func, RTMP1);
}

static void hook_load_host_reg_with_constant(cpu_recompiler_t* r, u32 host_reg, u32 val)
{
  arm64_mov_r_imm32(current_emit(be_from(r)), (arm64_reg_t)host_reg, val);
}

static void hook_load_host_reg_from_cpu_pointer(cpu_recompiler_t* r, u32 host_reg, const void* ptr)
{
  arm64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  arm64_ldr_r_m(e, ARM64_SZ_32, (arm64_reg_t)host_reg, state_mem(disp));
}

static void hook_store_host_reg_to_cpu_pointer(cpu_recompiler_t* r, u32 host_reg, const void* ptr)
{
  arm64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  arm64_str_r_m(e, ARM64_SZ_32, (arm64_reg_t)host_reg, state_mem(disp));
}

static void hook_store_constant_to_cpu_pointer(cpu_recompiler_t* r, u32 val, const void* ptr)
{
  arm64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  arm64_mov_r_imm32(e, RTMP2, val);
  arm64_str_r_m(e, ARM64_SZ_32, RTMP2, state_mem(disp));
}

static void hook_copy_host_reg(cpu_recompiler_t* r, u32 dst, u32 src)
{
  arm64_mov_r_r(current_emit(be_from(r)), ARM64_SZ_32,
                (arm64_reg_t)dst, (arm64_reg_t)src);
}

static void hook_flush(cpu_recompiler_t* r, u32 flags)
{
  /* Agnostic flush calls back into the vtable's host-reg primitives to
   * write dirty regs to g_state.  Then the backend handles arch-specific
   * flushes (PC, instruction bits, load delay). */
  arm64_emit_t* e = current_emit(be_from(r));

  if ((flags & CPU_RC_FLUSH_PC) && r->dirty_pc) {
    arm64_mov_r_imm32(e, RTMP2, r->compiler_pc);
    arm64_str_r_m(e, ARM64_SZ_32, RTMP2, state_mem(STATE_DISP(pc)));
    r->dirty_pc = false;
  }

  if (flags & CPU_RC_FLUSH_INSTRUCTION_BITS) {
    arm64_mov_r_imm32(e, RTMP2, r->inst->bits);
    arm64_str_r_m(e, ARM64_SZ_32, RTMP2,
                  state_mem(STATE_DISP(current_instruction)));
    arm64_mov_r_imm32(e, RTMP2, r->current_instruction_pc);
    arm64_str_r_m(e, ARM64_SZ_32, RTMP2,
                  state_mem(STATE_DISP(current_instruction_pc)));
    arm64_mov_r_imm32(e, RTMP2, r->current_instruction_branch_delay_slot ? 1u : 0u);
    arm64_str_r_m(e, ARM64_SZ_32, RTMP2,
                  state_mem(STATE_DISP(current_instruction_in_branch_delay_slot)));
  }

}

static void hook_compile_fallback(cpu_recompiler_t* r)
{
  if (r->cycles > 0) r->cycles--;

  cpu_recompiler_flush(r,
    CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS |
    CPU_RC_FLUSH_INVALIDATE_MIPS_REGISTERS |
    CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS |
    CPU_RC_FLUSH_PC |
    CPU_RC_FLUSH_LOAD_DELAY_FROM_STATE |
    CPU_RC_FLUSH_LOAD_DELAY |
    CPU_RC_FLUSH_GTE_DONE_CYCLE | 
    CPU_RC_FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS);

  arm64_emit_t* e = current_emit(be_from(r));
  arm64_mov_r_imm32(e, RTMP2, r->current_instruction_pc);
  arm64_str_r_m(e, ARM64_SZ_32, RTMP2, state_mem(STATE_DISP(pc)));

  if (r->cycles > 0) {
    arm64_ldr_r_m(e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pending_ticks)));
    arm64_alu_r_r_imm(e, ARM64_ALU_ADD, ARM64_SZ_32, RWARG1, RWARG1, (u32)r->cycles);
    arm64_str_r_m(e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pending_ticks)));
  }
  r->cycles = 0;

  arm64_jmp_abs64(e, g_interpret_block, RTMP1);

  cpu_recompiler_truncate_block(r);
  r->block_ended = true;
}

static u32 emit_asm_functions(void* code, u32 code_size)
{
  arm64_emit_t e;
  arm64_emit_init(&e, code, code_size);

  void* const enter = arm64_emit_cursor(&e);
  memcpy(&g_enter_recompiler, &enter, sizeof(enter));

  /* Save FP/LR + all callee-saved we'll touch.  AAPCS64 requires SP
   * 16-byte aligned.  Stack layout (each slot = 16 bytes):
   *   [sp+0]  : x29 (FP), x30 (LR)
   *   [sp+16] : x19 (RSTATE), x20 (RMEMBASE)
   *   [sp+32] : x21, x22
   *   [sp+48] : x23, x24
   *   [sp+64] : x25, x26
   *   [sp+80] : x27, x28
   * Total: 96 bytes; carved via STP-pre with -96 offset. */
  arm64_stp_pre (&e, ARM64_SZ_64, ARM64_X29, ARM64_X30, ARM64_SP, -96);
  arm64_stp_pre (&e, ARM64_SZ_64, ARM64_X19, ARM64_X20, ARM64_SP, -16);
  /* TODO: add the X21..X28 saves once allocator actually uses them; the
   * agnostic allocator tracks CALLEE_SAVED and only allocates what blocks
   * need, so saving them all is ~always wasted.  Current foundation: the
   * compile_fallback path doesn't allocate callee-saved, so X19/X20 alone
   * is sufficient.  Native ALU chunks will revisit this. */

  arm64_mov_r_imm64(&e, RSTATE, (u64)(uintptr_t)&g_cpu_state);
  if (cpu_code_cache_is_using_fastmem())
    arm64_ldr_r_m(&e, ARM64_SZ_64, RMEMBASE, state_mem(STATE_DISP(fastmem_base)));

  /* Event check fall-through. */
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pending_ticks)));
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG2, state_mem(STATE_DISP(downcount)));
  arm64_cmp_r_r(&e, ARM64_SZ_32, RWARG1, RWARG2);
  u32* jl_to_dispatch = arm64_b_cond_rel19(&e, ARM64_CC_LT);

  g_run_events_and_dispatch = arm64_emit_cursor(&e);
  arm64_call_abs64(&e, (const void*)&timing_events_run_events, RTMP1);

  void* const dispatch_addr = arm64_emit_cursor(&e);
  g_dispatcher = (const void*)dispatch_addr;

  /* RWARG1 = pc; load g_code_lut[(pc>>16) & ...].slot[pc & ...].
   * Two-level LUT lookup mirroring the x64 dispatcher.  TODO once UBFX +
   * indexed [base + idx*scale] memops land in arm64_emit, this can fold to
   * the same shape as x64.  For now: a portable fallback that performs the
   * lookup via shifts + adds, accepting some extra instruction bytes. */
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pc)));
  /* Punt the LUT walk to a C helper so chunk 0 lands without needing every
   * arithmetic primitive.  cpu_code_cache_dispatch_lookup(pc) returns the
   * host_code address (or g_compile_or_revalidate_block / g_interpret_block
   * for invalid blocks). */
  arm64_call_abs64(&e, (const void*)&cpu_code_cache_dispatch_lookup, RTMP1);
  /* Result in X0 → branch to it. */
  arm64_br(&e, ARM64_X0);

  arm64_patch_rel19(jl_to_dispatch, dispatch_addr);

  g_compile_or_revalidate_block = arm64_emit_cursor(&e);
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pc)));
  arm64_call_abs64(&e, (const void*)&cpu_code_cache_compile_or_revalidate_block, RTMP1);
  { u32* j = arm64_b_rel26(&e); arm64_patch_rel26(j, dispatch_addr); }

  g_discard_and_recompile_block = arm64_emit_cursor(&e);
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pc)));
  arm64_call_abs64(&e, (const void*)&cpu_code_cache_discard_and_recompile_block, RTMP1);
  { u32* j = arm64_b_rel26(&e); arm64_patch_rel26(j, dispatch_addr); }

  g_interpret_block = arm64_emit_cursor(&e);
  arm64_call_abs64(&e, (const void*)&cpu_recompiler_run_uncached_block, RTMP1);
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG1, state_mem(STATE_DISP(pending_ticks)));
  arm64_ldr_r_m(&e, ARM64_SZ_32, RWARG2, state_mem(STATE_DISP(downcount)));
  arm64_cmp_r_r(&e, ARM64_SZ_32, RWARG1, RWARG2);
  {
    u32* jge = arm64_b_cond_rel19(&e, ARM64_CC_GE);
    arm64_patch_rel19(jge, g_run_events_and_dispatch);
    u32* j = arm64_b_rel26(&e);
    arm64_patch_rel26(j, dispatch_addr);
  }

  Assert(!e.overflow);
  return (u32)arm64_emit_size(&e);
}

u32 arm64_backend_emit_dispatcher_for_test(void* code, u32 code_size)
{
  return emit_asm_functions(code, code_size);
}

static const char* arm64_get_host_reg_name(u32 reg)
{
  static const char* const names[32] = {
    "x0","x1","x2","x3","x4","x5","x6","x7",
    "x8","x9","x10","x11","x12","x13","x14","x15",
    "x16","x17","x18","x19","x20","x21","x22","x23",
    "x24","x25","x26","x27","x28","x29","x30","sp", 
  };
  return (reg < 32u) ? names[reg] : "??";
}

static const struct cpu_recompiler_vtable s_vtable = {
  /* Lifecycle. */
   .reset                    = hook_reset,
  .begin_block              = hook_begin_block,
  .end_compile              = hook_end_compile,
  .end_block                = hook_end_block,
  .end_block_with_exception = hook_end_block_with_exception, 

  /* Codegen primitives. */
  .get_current_code_pointer        = hook_get_current_code_pointer,
  .generate_block_protect_check    = NULL, /* TODO arm64 chunk M2 */
  .generate_icache_check_and_update= NULL, /* TODO arm64 chunk M3 */
  .generate_call                   = hook_generate_call,
  .generate_pgxp_call_with_mips_regs = NULL, /* TODO arm64 chunk M5 */

  /* Host-reg primitives. */
   .load_host_reg_with_constant   = hook_load_host_reg_with_constant,
  .load_host_reg_from_cpu_pointer= hook_load_host_reg_from_cpu_pointer,
  .store_host_reg_to_cpu_pointer = hook_store_host_reg_to_cpu_pointer,
  .store_constant_to_cpu_pointer = hook_store_constant_to_cpu_pointer,
  .copy_host_reg                 = hook_copy_host_reg,
  .flush                         = hook_flush, 

  .compile_fallback = hook_compile_fallback,

  .get_host_reg_name = arm64_get_host_reg_name,
};

static void arm64_emit_jump_at_site(void* site, const void* dst, bool flush_icache)
{
  /* Patch a B (rel26) at `site` to target `dst`.  Block-link infrastructure
   * uses this to chain compiled blocks.  flush_icache is required on
   * AArch64 since I/D coherency is not automatic. */
  arm64_patch_rel26((u32*)site, dst);
  if (flush_icache)
    memmap_flush_instruction_cache(site, 4u);
}

void arm64_backend_register(void)
{
  memset(&s_backend, 0, sizeof(s_backend));
  s_backend.base.v = &s_vtable;
  g_cpu_compiler = &s_backend.base;

  cpu_code_cache_set_emit_jump(&arm64_emit_jump_at_site);
  cpu_code_cache_set_emit_asm_functions(&emit_asm_functions);

  INFO_LOG("arm64 backend registered (foundation: dispatcher + lifecycle + "
           "fallback-only; per-op codegen TODO)");
}

void arm64_backend_shutdown(void)
{
  cpu_code_cache_set_emit_jump(NULL);
  cpu_code_cache_set_emit_asm_functions(NULL);
  g_cpu_compiler = NULL;
  memset(&s_backend, 0, sizeof(s_backend));
}

#else /* !__aarch64__ */

#include "common/types.h"

void arm64_backend_register(void)  { /* no-op on non-aarch64 hosts */ }
void arm64_backend_shutdown(void)  { /* no-op */ }
u32  arm64_backend_emit_dispatcher_for_test(void* code, u32 code_size)
{ (void)code; (void)code_size; return 0u; }

#endif /* __aarch64__ */
