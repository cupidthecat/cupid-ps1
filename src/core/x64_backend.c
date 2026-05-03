/*
 * x86_64 backend for the CPU recompiler.
 *
 * Provides: dispatcher prelude + lifecycle (Reset/BeginBlock/EndCompile/
 * EndBlock/EndBlockWithException/Flush) + host-reg primitives +
 * Compile_Fallback, ALU-imm hooks (ori/andi/xori), shifts, branches,
 * mul/div, lxx/sxx, cop0, cop2.
 *
 * RBP is the cpu_state pointer.  RBX is reserved for the fastmem base.
 * RDI/RSI/RDX/RCX are call args under the SysV ABI (Linux).
 */

#include "x64_backend.h"

#include "bus.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_diff_block.h"
#include "cpu_diff_runner.h"
#include "cpu_pc_trigger.h"
#include "cpu_recompiler.h"
#include "gte.h"
#include "settings.h"
#include "timing_event.h"
#include "x64_emit.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/types.h"

#include <stddef.h>
#include <string.h>

LOG_CHANNEL(Recompiler);

#define RWRET    X64_RAX
#define RWARG1   X64_RDI
#define RWARG2   X64_RSI
#define RWARG3   X64_RDX
#define RWARG4   X64_RCX
#define RXRET    X64_RAX
#define RXARG1   X64_RDI
#define RXARG2   X64_RSI
#define RXARG3   X64_RDX
#define RXARG4   X64_RCX
#define RSTATE   X64_RBP   /* &g_cpu_state */
#define RMEMBASE X64_RBX   /* fastmem base; reserved */

#define STATE_DISP(field)         ((s32)offsetof(cpu_state_t, field))
#define STATE_REG_DISP(reg_index) ((s32)(offsetof(cpu_state_t, regs) + (reg_index) * 4u))

typedef struct {
  cpu_recompiler_t base;     /* agnostic state */
  x64_emit_t       near_emit;
  x64_emit_t       far_emit;
  bool             is_far;   /* current emitter target */
  const u8*        block_near_start; /* block-host-code start (for self-link) */
} x64_backend_state_t;

static x64_backend_state_t s_backend;

static inline x64_backend_state_t* be_from(cpu_recompiler_t* r)
{
  return (x64_backend_state_t*)r;
}

static inline x64_emit_t* current_emit(x64_backend_state_t* b)
{
  return b->is_far ? &b->far_emit : &b->near_emit;
}

static inline x64_mem_t mips_ptr(cpu_reg_t reg)
{
  DebugAssert((u32)reg < CPU_REG_COUNT);
  return x64_mem(RSTATE, STATE_REG_DISP((u32)reg));
}

static inline x64_reg_t cf_get_reg_d(cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_d);
  return (x64_reg_t)cf.host_d;
}
static inline x64_reg_t cf_get_reg_s(cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_s);
  return (x64_reg_t)cf.host_s;
}
static inline x64_reg_t cf_get_reg_t(cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_t);
  return (x64_reg_t)cf.host_t;
}
static inline x64_reg_t cf_get_reg_lo(cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_lo);
  return (x64_reg_t)cf.host_lo;
}
static inline x64_reg_t cf_get_reg_hi(cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_hi);
  return (x64_reg_t)cf.host_hi;
}

/* Materialize cf.MipsS into `dst` (32-bit). */
static void move_s_to_reg(cpu_recompiler_t* r, x64_reg_t dst,
                          cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (cf.valid_host_s) {
    if ((u32)cf.host_s != (u32)dst)
      x64_mov_r_r(e, X64_SZ_32, dst, (x64_reg_t)cf.host_s);
  } else if (cf.const_s) {
    const u32 cv = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_s(cf));
    if (cv == 0u)
      x64_alu_r_r (e, X64_ALU_XOR, X64_SZ_32, dst, dst);
    else
      x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)cv);
  } else {
    x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(cpu_recomp_cf_mips_s(cf)));
  }
}

static void move_t_to_reg(cpu_recompiler_t* r, x64_reg_t dst,
                          cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (cf.valid_host_t) {
    if ((u32)cf.host_t != (u32)dst)
      x64_mov_r_r(e, X64_SZ_32, dst, (x64_reg_t)cf.host_t);
  } else if (cf.const_t) {
    const u32 cv = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf));
    if (cv == 0u)
      x64_alu_r_r (e, X64_ALU_XOR, X64_SZ_32, dst, dst);
    else
      x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)cv);
  } else {
    x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(cpu_recomp_cf_mips_t(cf)));
  }
}

/* Returns the rt host reg with cf.MipsS materialized into it. */
static x64_reg_t move_s_to_t(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_t);
  const x64_reg_t rt = cf_get_reg_t(cf);
  move_s_to_reg(r, rt, cf);
  return rt;
}

/* Reset: agnostic-state reset + (re-)allocate emitters from the code-cache
 * buffer, repopulate USABLE / CALLEE_SAVED bits on the host_regs array. */
static void hook_reset(cpu_recompiler_t* r, cpu_code_cache_block_t* block,
                       u8* code, u32 code_size, u8* far_code, u32 far_code_size)
{
  /* Agnostic reset: clear constants/load-delay/host-state/spec, preserve
   * USABLE / CALLEE_SAVED bits. */
  cpu_recompiler_reset_state(r, block, code, code_size, far_code, far_code_size);

  x64_backend_state_t* b = be_from(r);

  cpu_code_cache_align_code(CPU_RECOMP_FUNCTION_ALIGNMENT);

  u8* const near_buf  = cpu_code_cache_get_free_code_pointer();
  const u32 near_cap  = cpu_code_cache_get_free_code_space();
  u8* const far_buf   = cpu_code_cache_get_free_far_code_pointer();
  const u32 far_cap   = cpu_code_cache_get_free_far_code_space();

  x64_emit_init(&b->near_emit, near_buf, near_cap);
  x64_emit_init(&b->far_emit,  far_buf,  far_cap);
  b->is_far = false;
  b->block_near_start = near_buf;

  /* Repopulate USABLE / CALLEE_SAVED.  The reserved registers (RAX/RDI/RSI/
   * RDX call args, RSP, RBP=cpu_state, RBX=fastmem-or-reserved, RCX=shift
   * amount) must remain non-USABLE so the allocator never picks them. */
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    ra->flags &= (u8)CPU_RC_HR_IMMUTABLE_FLAGS;
    if (i == X64_RAX || i == X64_RDI || i == X64_RSI || i == X64_RDX ||
        i == X64_RSP || i == X64_RBP || i == X64_RBX || i == X64_RCX) {
      ra->flags &= (u8)~CPU_RC_HR_USABLE;
      continue;
    }
    /* Mark usable, set callee-saved bit per SysV calling convention.
     * SysV callee-saved: RBX (excluded), RBP (excluded), R12-R15.
     * Caller-saved: R8-R11, plus the call-arg regs we already excluded. */
    u8 fl = (u8)CPU_RC_HR_USABLE;
    if (i >= X64_R12 && i <= X64_R15)
      fl |= (u8)CPU_RC_HR_CALLEE_SAVED;
    ra->flags |= fl;
  }
}

 /*
 * register `reg` into host register `dst` (32-bit form).  Order:
 *   1. host-allocated copy if cached in a host reg,
 *   2. compile-time constant if known,
 *   3. fall back to load from g_state.regs. */
static void emit_mov_mips_reg_to_host(cpu_recompiler_t* r, x64_reg_t dst, cpu_reg_t reg)
{
  x64_emit_t* e = current_emit(be_from(r));
  DebugAssert((u32)reg < CPU_REG_COUNT);
  cpu_recomp_opt_u32_t hreg = cpu_recompiler_check_host_reg(r, 0u, CPU_RC_HRT_CPU_REG, reg);
  if (hreg.has) {
    if ((u32)hreg.val != (u32)dst)
      x64_mov_r_r(e, X64_SZ_32, dst, (x64_reg_t)hreg.val);
  } else if (cpu_recompiler_has_constant_reg(r, reg)) {
    const u32 v = cpu_recompiler_get_constant_reg_u32(r, reg);
    if (v == 0u)
      x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, dst, dst);
    else
      x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)v);
  } else {
    x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(reg));
  }
}

 /*
 * caller-saved hosts so g_state matches what the PGXP CPU-mode handler
 * sees, marshal up to two MIPS regs into RWARG2/3, instruction bits into
 * RWARG1 immediate, then call. */
static void hook_generate_pgxp_call_with_mips_regs(cpu_recompiler_t* r,
                                                   const void* func, u32 arg1val,
                                                   cpu_reg_t arg2, cpu_reg_t arg3)
{
  x64_emit_t* e = current_emit(be_from(r));
  DebugAssert(g_settings.gpu_pgxp_enable);
  cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
  if ((u32)arg2 < CPU_REG_COUNT)
    emit_mov_mips_reg_to_host(r, RWARG2, arg2);
  if ((u32)arg3 < CPU_REG_COUNT)
    emit_mov_mips_reg_to_host(r, RWARG3, arg3);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)arg1val);
  x64_call_abs64 (e, func);
}

 /*
 * Compares the current RAM bytes against the shadow snapshot taken at
 * compile time and jumps to g_discard_and_recompile_block on mismatch.
 * SSE-free fallback: 8-byte cmp loop + optional 4-byte tail.  PS1 blocks
 * are always 4-byte aligned (single MIPS instructions), so no byte-tail
 * needed. */
static void hook_generate_block_protect_check(cpu_recompiler_t* r,
                                              const u8* ram, const u8* shadow,
                                              u32 size)
{
  x64_emit_t* e = current_emit(be_from(r));
  /* Pre-load both pointers; offsets fit in disp8/disp32 from there so we
   * avoid re-issuing the 10-byte movabs per cmp. */
  x64_mov_r_imm64(e, RXARG1, (u64)(uintptr_t)ram);
  x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)shadow);

  u32 offset = 0u;
  while (size >= 8u) {
    x64_mov_r_m  (e, X64_SZ_64, RXARG3, x64_mem(RXARG1, (s32)offset));
    x64_alu_r_m  (e, X64_ALU_CMP, X64_SZ_64, RXARG3,
                  x64_mem(RXARG2, (s32)offset));
    u8* jne = x64_jcc_rel32(e, X64_CC_NE);
    x64_patch_rel32(jne, g_discard_and_recompile_block);
    offset += 8u;
    size   -= 8u;
  }
  if (size >= 4u) {
    x64_mov_r_m  (e, X64_SZ_32, RWARG3, x64_mem(RXARG1, (s32)offset));
    x64_alu_r_m  (e, X64_ALU_CMP, X64_SZ_32, RWARG3,
                  x64_mem(RXARG2, (s32)offset));
    u8* jne = x64_jcc_rel32(e, X64_CC_NE);
    x64_patch_rel32(jne, g_discard_and_recompile_block);
    offset += 4u;
    size   -= 4u;
  }
  DebugAssert(size == 0u);
}

 /*
 * Three branches:
 *   1. Block doesn't use the i-cache + has dynamic fetch ticks (pointer to
 *      memory access time changes at runtime e.g. EXP regions): emit
 *      `imul size,[ptr]; add [pending_ticks], result`.
 *   2. Block doesn't use the i-cache + static fetch ticks: emit `add
 *      [pending_ticks], uncached_fetch_ticks`.
 *   3. Block uses i-cache: per-line tag compare, bump pending_ticks by
 *      fill_ticks on each line miss, store new tag back. */
static void hook_generate_icache_check_and_update(cpu_recompiler_t* r)
{
  x64_emit_t* e = current_emit(be_from(r));
  const cpu_code_cache_block_t* b = r->block;

  if (!cpu_code_cache_block_has_flag(b, CPU_CODE_CACHE_BF_IS_USING_ICACHE)) {
    if (cpu_code_cache_block_has_flag(b, CPU_CODE_CACHE_BF_NEEDS_DYNAMIC_FETCH_TICKS)) {
      const tick_count_t* tp = cpu_recompiler_get_fetch_memory_access_time_ptr(r);
      /* RAX = block size; ECX = *tp; imul RAX, ECX; add [pending_ticks], EAX. */
      x64_mov_r_imm32(e, X64_SZ_32, X64_RAX, (s32)b->size);
      x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)tp);
      x64_mov_r_m   (e, X64_SZ_32, RWARG2, x64_mem(RXARG2, 0));
      x64_imul_r_r  (e, X64_SZ_32, X64_RAX, RWARG2);
      x64_alu_m_r   (e, X64_ALU_ADD, X64_SZ_32,
                     x64_mem(RSTATE, STATE_DISP(pending_ticks)), X64_RAX);
    } else {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)),
                    (s32)b->uncached_fetch_ticks);
    }
    return;
  }

  if (b->icache_line_count == 0u) return;

  /* Cached block: emit per-line tag check. */
  virtual_memory_address_t current_pc = b->pc & CPU_ICACHE_TAG_ADDRESS_MASK;
  const tick_count_t fill_ticks = cpu_get_icache_fill_ticks(current_pc);
  if (fill_ticks <= 0) return;

  /* RXARG1 = &g_state.icache_tags; RWARG2 = miss accumulator;
   * RWARG4 = fill_ticks per miss (constant across all lines). */
  x64_lea_r_m   (e, X64_SZ_64, RXARG1,
                 x64_mem(RSTATE, STATE_DISP(icache_tags)));
  x64_alu_r_r   (e, X64_ALU_XOR, X64_SZ_32, RWARG2, RWARG2);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG4, (s32)fill_ticks);

  for (u32 i = 0u; i < b->icache_line_count; i++,
       current_pc += (virtual_memory_address_t)CPU_ICACHE_LINE_SIZE) {
    const u32 tag    = cpu_get_icache_tag_for_address(current_pc);
    const u32 line   = cpu_get_icache_line(current_pc);
    const s32 offset = (s32)(line * sizeof(u32));

    x64_alu_r_r   (e, X64_ALU_XOR, X64_SZ_32, RWARG3, RWARG3);
    x64_alu_m_imm (e, X64_ALU_CMP, X64_SZ_32, x64_mem(RXARG1, offset), (s32)tag);
    x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RXARG1, offset), (s32)tag);
    x64_cmovcc_r_r(e, X64_CC_NE, X64_SZ_32, RWARG3, RWARG4);
    x64_alu_r_r   (e, X64_ALU_ADD, X64_SZ_32, RWARG2, RWARG3);
  }

  x64_alu_m_r(e, X64_ALU_ADD, X64_SZ_32,
              x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG2);
}

static void hook_begin_block(cpu_recompiler_t* r)
{
  /* Agnostic BeginBlock state-seeding happens in cpu_recompiler_compile_block
   * after this hook returns (sets r->inst, r->iinfo, r->compiler_pc, dirty
   * flags). */
  x64_emit_t* e = current_emit(be_from(r));
#ifdef CUPID_DEBUG_RECOMP_TRACE
  if (r->block != NULL) {
    extern void cpu_recomp_trace_block_entry(u32 pc);
    ERROR_LOG("begin_block pc=%08X size=%u", r->block->pc, r->block->size);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)r->block->pc);
    x64_call_abs64(e, (const void*)&cpu_recomp_trace_block_entry);
  }
#endif

  /* Per-block recomp/interp diff: emit a call to cpu_diff_block_enter at
   * the very top of the prologue, *before* any fastmem/icache/SMC work.
   * At this point the allocator is empty and EFLAGS is clean; the call
   * cannot perturb subsequent codegen.  When diff mode is off, the C
   * function returns after one mem-load + branch. */
  if (cpu_diff_block_is_active() && r->block != NULL) {
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)r->block->pc);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG2, (s32)r->block->size);
    x64_call_abs64 (e, (const void*)&cpu_diff_block_enter);
  }

  /* Per-block execution ring (CUPID_TRACE_EXEC=1).  Records every block
   * dispatch into a ring so the safety-net diag can show the actual
   * execution chain (not just the compile order). */
  if (cpu_code_cache_exec_trace_is_active() && r->block != NULL) {
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)r->block->pc);
    x64_call_abs64 (e, (const void*)&cpu_code_cache_dbg_record_exec);
  }

  /* PC-trigger GPR-dump hook (CUPID_PC_TRIGGER_PC=0x...).  Compile-time PC
   * compare against the latched target; only matching blocks emit a call.
   * Zero codegen impact in non-matching blocks.  Allocator empty + EFLAGS
   * clean here (same constraint as cpu_diff_block_enter above). */
  if (cpu_pc_trigger_is_active() && r->block != NULL &&
      r->block->pc == cpu_pc_trigger_target_pc()) {
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)r->block->pc);
    x64_call_abs64 (e, (const void*)&cpu_pc_trigger_on_block);
  }

  if (r->block != NULL &&
      r->block->protection == CPU_CODE_CACHE_PROT_MANUAL_CHECK &&
      r->v && r->v->generate_block_protect_check) {
    const u32 phys = cpu_virtual_to_physical(r->block->pc);
    const u8* ram_ptr = bus_get_ram_pointer() + phys;
    const u8* shadow_ptr = (const u8*)cpu_code_cache_block_instructions(r->block);
    r->v->generate_block_protect_check(r, ram_ptr, shadow_ptr,
                                       r->block->size * CPU_INSTRUCTION_SIZE);
  }

  if (r->block != NULL && r->v && r->v->generate_icache_check_and_update)
    r->v->generate_icache_check_and_update(r);

  if (cpu_code_cache_is_using_fastmem())
    x64_mov_r_m(e, X64_SZ_64, RMEMBASE,
                x64_mem(RSTATE, STATE_DISP(fastmem_base)));
}

static const void* hook_end_compile(cpu_recompiler_t* r,
                                    u32* out_code_size, u32* out_far_code_size)
{
  x64_backend_state_t* b = be_from(r);
  Assert(!b->near_emit.overflow);
  Assert(!b->far_emit.overflow);

  const void* code = b->block_near_start;
  const u32 near_size = (u32)x64_emit_size(&b->near_emit);
  const u32 far_size  = (u32)x64_emit_size(&b->far_emit);

  /* Commit consumed bytes to the code cache so subsequent blocks see the
   * advanced free pointer. */
  cpu_code_cache_commit_code(near_size);
  cpu_code_cache_commit_far_code(far_size);

  *out_code_size     = near_size;
  *out_far_code_size = far_size;
  b->block_near_start = NULL;
  return code;
}

/* Helper: emit cycle accounting + downcount check + jump to dispatcher or
 * next block.  Called from end_block + end_block_with_exception. */
static void emit_end_and_link_block(cpu_recompiler_t* r,
                                    const u32* newpc, bool do_event_test,
                                    bool force_run_events)
{
  x64_backend_state_t* b = be_from(r);
  x64_emit_t* e = current_emit(b);

  DebugAssert(!r->dirty_pc && !r->block_ended);
  r->block_ended = true;

  const s32 cycles = r->cycles;
  r->cycles = 0;
  const bool diff_active = cpu_diff_block_is_active();

  /* When the per-block diff harness is active we MUST emit the
   * cpu_diff_block_exit call AFTER pending_ticks has been advanced by the
   * block's accumulated cycles, otherwise the harness's pending_ticks
   * snapshot misses the per-block tail (only the prefix + mid-block
   * mid-flushes are captured, not the cycles still sitting in r->cycles
   * at end_block).  Without this, the harness reports a phantom
   * recomp/interp tick divergence on every block.  When diff is off, we
   * keep the original fast/slow paths byte-for-byte. */
  if (diff_active) {
    /* 1. Tick flush (mirrors slow-path's add). */
    if (cycles == 1) {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), 1);
    } else if (cycles > 0) {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), cycles);
    }
    /* 2. GTE completion-tick stash (mirrors slow-path branch). */
    if (r->gte_done_cycle > cycles) {
      x64_mov_r_m(e, X64_SZ_32, RWARG1,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)));
      const s32 add = r->gte_done_cycle - cycles;
      if (add == 1)
        x64_inc_r(e, X64_SZ_32, RWARG1);
      else
        x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, add);
      x64_mov_m_r(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(gte_completion_tick)), RWARG1);
    }
    /* 3. The diff hook itself.  Snapshot now sees the post-flush
     *    pending_ticks; comparison against the interp replay matches. */
    x64_call_abs64(e, (const void*)&cpu_diff_block_exit);
    /* 4. Event test (re-issue cmp; the C call clobbered EFLAGS). */
    if (force_run_events) {
      u8* j = x64_jmp_rel32(e);
      x64_patch_rel32(j, g_run_events_and_dispatch);
      return;
    }
    if (do_event_test) {
      x64_mov_r_m(e, X64_SZ_32, RWARG1,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)));
      x64_alu_r_m(e, X64_ALU_CMP, X64_SZ_32, RWARG1,
                  x64_mem(RSTATE, STATE_DISP(downcount)));
      u8* j = x64_jcc_rel32(e, X64_CC_GE);
      x64_patch_rel32(j, g_run_events_and_dispatch);
    }
  } else if (!do_event_test && r->gte_done_cycle <= cycles) {
    /* Fast path: only need to advance pending_ticks. */
    if (cycles == 1) {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), 1);
    } else if (cycles > 0) {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), cycles);
    }
    if (force_run_events) {
      u8* j = x64_jmp_rel32(e);
      x64_patch_rel32(j, g_run_events_and_dispatch);
      return;
    }
  } else {
    /* Slow path: pending_ticks += cycles ; if (pending_ticks >= downcount) → events. */
    if (do_event_test || cycles > 0 || r->gte_done_cycle > cycles) {
      x64_mov_r_m(e, X64_SZ_32, RWARG1,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)));
    }
    if (cycles > 0)
      x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, cycles);
    if (r->gte_done_cycle > cycles) {
      x64_mov_r_r(e, X64_SZ_32, RWARG2, RWARG1);
      const s32 add = r->gte_done_cycle - cycles;
      if (add == 1)
        x64_inc_r(e, X64_SZ_32, RWARG2);
      else
        x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG2, add);
      x64_mov_m_r(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(gte_completion_tick)), RWARG2);
    }
    if (do_event_test) {
      x64_alu_r_m(e, X64_ALU_CMP, X64_SZ_32, RWARG1,
                  x64_mem(RSTATE, STATE_DISP(downcount)));
    }
    if (cycles > 0) {
      x64_mov_m_r(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG1);
    }
    if (do_event_test) {
      u8* j = x64_jcc_rel32(e, X64_CC_GE);
      x64_patch_rel32(j, g_run_events_and_dispatch);
    }
  }

  /* Link to dispatcher or next block. */
  if (newpc == NULL) {
    u8* j = x64_jmp_rel32(e);
    x64_patch_rel32(j, g_dispatcher);
  } else {
    void* const site = x64_emit_cursor(e);
    const void* target =
      (*newpc == r->block->pc) ?
        cpu_code_cache_create_self_block_link(r->block, site, b->block_near_start) :
        cpu_code_cache_create_block_link     (r->block, site, *newpc);
    u8* j = x64_jmp_rel32(e);
    x64_patch_rel32(j, target);
  }
}

static void hook_end_block(cpu_recompiler_t* r, const u32* newpc, bool do_event_test)
{
  x64_emit_t* e = current_emit(be_from(r));
#ifdef CUPID_DEBUG_RECOMP_TRACE
  if (r->block != NULL)
    ERROR_LOG("end_block pc=%08X newpc=%s do_event=%d cycles=%d",
              r->block->pc, (newpc != NULL ? "set" : "NULL"),
              (int)do_event_test, (int)r->cycles);
#endif
  if (newpc != NULL) {
    /* Always write PC at block exit. The previous gate
     * `r->compiler_pc != *newpc` assumed the runtime pc field already
     * held compiler_pc, but the JIT never writes pc during block
     * execution; it only holds the dispatcher-set entry PC. Skipping
     * the write made not-taken branch fall-throughs jump to dispatcher
     * with stale entry PC, which re-entered the same block forever
     * (BIOS scratchpad-clear loop never exited). */
    x64_mov_m_imm32(e, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pc)), (s32)*newpc);
  }
  r->dirty_pc = false;

  /* Recomp doesn't maintain next_instruction_is_branch_delay_slot or
   * branch_was_taken at runtime (interp does, per-iter).  At block exit,
   * delay slot has been processed and any branch retired, so both should be
   * false.  cpu_dispatch_interrupt reads these for cause.BD/BT when an IRQ
   * fires between blocks; without the reset the flags retain stale values
   * (e.g. from the BIOS interp run before recomp took over), and amidog's
   * INTR test reports wrong cause flags. */
  x64_mov_m_imm32(e, X64_SZ_8,
                  x64_mem(RSTATE, STATE_DISP(next_instruction_is_branch_delay_slot)), 0);
  x64_mov_m_imm32(e, X64_SZ_8,
                  x64_mem(RSTATE, STATE_DISP(branch_was_taken)), 0);

  cpu_recompiler_flush(r, CPU_RC_FLUSH_END_BLOCK);

  /* Per-block diff: cpu_diff_block_exit is emitted from inside
   * emit_end_and_link_block, AFTER the per-block cycles flush, so the
   * harness's pending_ticks snapshot includes the end-of-block tick tail.
   * Calling it before the cycles flush would leak the tail into the next
   * block and produce spurious tick-divergence reports. */

  emit_end_and_link_block(r, newpc, do_event_test, false);
}

static void hook_end_block_with_exception(cpu_recompiler_t* r, cpu_exception_t excode)
{
  x64_emit_t* e = current_emit(be_from(r));

  cpu_recompiler_flush(r,
    CPU_RC_FLUSH_END_BLOCK | CPU_RC_FLUSH_FOR_EXCEPTION | CPU_RC_FLUSH_FOR_C_CALL);

  /* RWARG1 = cause_bits (computed at emit-time);
   * RWARG2 = m_current_instruction_pc;
   * if (BP) RWARG3 = inst->bits + call cpu_raise_break_exception
   * else                     call cpu_raise_exception_bits  */
  const u32 cause = cpu_cop0_cause_make(
    excode, r->current_instruction_branch_delay_slot,
    /*BT*/ r->current_instruction_branch_delay_slot && r->delay_slot_branch_was_taken,
    (u8)r->inst->cop.cop_n);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)cause);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG2, (s32)r->current_instruction_pc);

  if (excode != CPU_EXCEPTION_BP) {
    x64_call_abs64(e, (const void*)&cpu_raise_exception_bits);
  } else {
    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, (s32)r->inst->bits);
    x64_call_abs64(e, (const void*)&cpu_raise_break_exception);
  }

  /* Per-block diff: same insertion point as the no-exception path. */
  if (cpu_diff_block_is_active())
    x64_call_abs64(e, (const void*)&cpu_diff_block_exit);

  r->dirty_pc = false;
  emit_end_and_link_block(r, NULL, /*do_event_test=*/true, /*force=*/false);
}

static const void* hook_get_current_code_pointer(cpu_recompiler_t* r)
{
  return x64_emit_cursor(current_emit(be_from(r)));
}

static void hook_generate_call(cpu_recompiler_t* r, const void* func,
                               s32 a1reg, s32 a2reg, s32 a3reg)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (a1reg >= 0 && (x64_reg_t)a1reg != RXARG1)
    x64_mov_r_r(e, X64_SZ_64, RXARG1, (x64_reg_t)a1reg);
  if (a2reg >= 0 && (x64_reg_t)a2reg != RXARG2)
    x64_mov_r_r(e, X64_SZ_64, RXARG2, (x64_reg_t)a2reg);
  if (a3reg >= 0 && (x64_reg_t)a3reg != RXARG3)
    x64_mov_r_r(e, X64_SZ_64, RXARG3, (x64_reg_t)a3reg);
  x64_call_abs64(e, func);
}

static void hook_load_host_reg_with_constant(cpu_recompiler_t* r, u32 host_reg, u32 val)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (val == 0u)
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, (x64_reg_t)host_reg, (x64_reg_t)host_reg);
  else
    x64_mov_r_imm32(e, X64_SZ_32, (x64_reg_t)host_reg, (s32)val);
}

static void hook_load_host_reg_from_cpu_pointer(cpu_recompiler_t* r, u32 host_reg, const void* ptr)
{
  x64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  x64_mov_r_m(e, X64_SZ_32, (x64_reg_t)host_reg, x64_mem(RSTATE, disp));
}

static void hook_store_host_reg_to_cpu_pointer(cpu_recompiler_t* r, u32 host_reg, const void* ptr)
{
  x64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  x64_mov_m_r(e, X64_SZ_32, x64_mem(RSTATE, disp), (x64_reg_t)host_reg);
}

static void hook_store_constant_to_cpu_pointer(cpu_recompiler_t* r, u32 val, const void* ptr)
{
  x64_emit_t* e = current_emit(be_from(r));
  const s32 disp = (s32)((const u8*)ptr - (const u8*)&g_cpu_state);
  x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RSTATE, disp), (s32)val);
}

static void hook_copy_host_reg(cpu_recompiler_t* r, u32 dst, u32 src)
{
  if (src == dst) return;
  x64_emit_t* e = current_emit(be_from(r));
  x64_mov_r_r(e, X64_SZ_32, (x64_reg_t)dst, (x64_reg_t)src);
}

/* Backend-side flush.  Agnostic flush has already done the bookkeeping;
 * this emits the corresponding host stores.  Skips the
 * GenerateBlockProtectCheck-related bits. */
static void hook_flush(cpu_recompiler_t* r, u32 flags)
{
  x64_emit_t* e = current_emit(be_from(r));

  if ((flags & CPU_RC_FLUSH_PC) && r->dirty_pc) {
    x64_mov_m_imm32(e, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pc)), (s32)r->compiler_pc);
    r->dirty_pc = false;
  }

  if (flags & CPU_RC_FLUSH_INSTRUCTION_BITS) {
    x64_mov_m_imm32(e, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(current_instruction)),
                    (s32)r->inst->bits);
    x64_mov_m_imm32(e, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(current_instruction_pc)),
                    (s32)r->current_instruction_pc);
    /* current_instruction_in_branch_delay_slot is a single byte. */
    x64_mov_m_imm32(e, X64_SZ_8,
                    x64_mem(RSTATE, STATE_DISP(current_instruction_in_branch_delay_slot)),
                    r->current_instruction_branch_delay_slot ? 1 : 0);
  }

  if ((flags & CPU_RC_FLUSH_LOAD_DELAY_FROM_STATE) && r->load_delay_dirty) {
    /* Read load_delay_reg into RWARG1 (zero-extend), load_delay_value into RWARG2,
     * write to regs.r[RWARG1] = RWARG2, then mark load_delay_reg = COUNT. */
    x64_movzx_r_m8(e, X64_SZ_32, RWARG1,
                   x64_mem(RSTATE, STATE_DISP(load_delay_reg)));
    x64_mov_r_m(e, X64_SZ_32, RWARG2,
                x64_mem(RSTATE, STATE_DISP(load_delay_value)));
    /* mov [rbp + offsetof(regs.r) + RWARG1*4], RWARG2 */
    x64_mov_msib_r(e, X64_SZ_32,
                   x64_msib(RSTATE, RWARG1, 4,
                            (s32)offsetof(cpu_state_t, regs)),
                   RWARG2);
    x64_mov_m_imm32(e, X64_SZ_8,
                    x64_mem(RSTATE, STATE_DISP(load_delay_reg)),
                    (s32)CPU_REG_COUNT);
    r->load_delay_dirty = false;
  }

  if ((flags & CPU_RC_FLUSH_LOAD_DELAY) && r->load_delay_register != (cpu_reg_t)CPU_REG_COUNT) {
    if (r->load_delay_value_register != CPU_RECOMP_NUM_HOST_REGS)
      cpu_recompiler_free_host_reg(r, r->load_delay_value_register);

    x64_mov_m_imm32(e, X64_SZ_8,
                    x64_mem(RSTATE, STATE_DISP(load_delay_reg)),
                    (s32)r->load_delay_register);
    r->load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
    r->load_delay_dirty = true;
  }

  if ((flags & CPU_RC_FLUSH_GTE_STALL_FROM_STATE) && r->dirty_gte_done_cycle) {
    x64_mov_r_m(e, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)));
    x64_mov_r_m(e, X64_SZ_32, RWARG2,
                x64_mem(RSTATE, STATE_DISP(gte_completion_tick)));
    if (r->cycles > 0) {
      if (r->cycles == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
      else                x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->cycles);
      r->cycles = 0;
    }
    x64_alu_r_r (e, X64_ALU_CMP, X64_SZ_32, RWARG2, RWARG1);
    x64_cmovcc_r_r(e, X64_CC_A, X64_SZ_32, RWARG1, RWARG2);
    x64_mov_m_r(e, X64_SZ_32,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG1);
    r->dirty_gte_done_cycle = false;
  }

  /* Mirrors FLUSH_GTE_STALL_FROM_STATE for the muldiv side: pending_ticks =
   * max(pending_ticks + r->cycles, muldiv_completion_tick).  Used by
   * MFHI/MFLO/MTHI/MTLO when r->dirty_muldiv_done_cycle (i.e. previous
   * MULT/MULTU non-const-rs producer or cross-block predecessor). */
  if ((flags & CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE) && r->dirty_muldiv_done_cycle) {
    x64_mov_r_m(e, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)));
    x64_mov_r_m(e, X64_SZ_32, RWARG2,
                x64_mem(RSTATE, STATE_DISP(muldiv_completion_tick)));
    if (r->cycles > 0) {
      if (r->cycles == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
      else                x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->cycles);
      r->cycles = 0;
    }
    x64_alu_r_r (e, X64_ALU_CMP, X64_SZ_32, RWARG2, RWARG1);
    x64_cmovcc_r_r(e, X64_CC_A, X64_SZ_32, RWARG1, RWARG2);
    x64_mov_m_r(e, X64_SZ_32,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG1);
    r->dirty_muldiv_done_cycle = false;
  }

  if ((flags & CPU_RC_FLUSH_GTE_DONE_CYCLE) && r->gte_done_cycle > r->cycles) {
    x64_mov_r_m(e, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)));
    if ((flags & CPU_RC_FLUSH_CYCLES) && r->cycles > 0) {
      if (r->cycles == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
      else                x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->cycles);
      x64_mov_m_r(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG1);
      r->gte_done_cycle -= r->cycles;
      r->cycles = 0;
    }
    if (r->gte_done_cycle == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
    else                        x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->gte_done_cycle);
    x64_mov_m_r(e, X64_SZ_32,
                x64_mem(RSTATE, STATE_DISP(gte_completion_tick)), RWARG1);
    r->gte_done_cycle = 0;
    r->dirty_gte_done_cycle = true;
  }

  /* Mirrors FLUSH_GTE_DONE_CYCLE for muldiv: writes the in-block-tracked
   * r->muldiv_done_cycle out to muldiv_completion_tick state, so a
   * subsequent block (whose dirty_muldiv_done_cycle starts true) can
   * stall against it.  Only fires for the clean (static-cycle) producer
   * path; dirty was already written to state by MULT/MULTU runtime emit. */
  if ((flags & CPU_RC_FLUSH_MULDIV_DONE_CYCLE) && !r->dirty_muldiv_done_cycle &&
      r->muldiv_done_cycle > r->cycles) {
    x64_mov_r_m(e, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)));
    if ((flags & CPU_RC_FLUSH_CYCLES) && r->cycles > 0) {
      if (r->cycles == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
      else                x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->cycles);
      x64_mov_m_r(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)), RWARG1);
      r->muldiv_done_cycle -= r->cycles;
      r->cycles = 0;
    }
    if (r->muldiv_done_cycle == 1) x64_inc_r  (e, X64_SZ_32, RWARG1);
    else                           x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->muldiv_done_cycle);
    x64_mov_m_r(e, X64_SZ_32,
                x64_mem(RSTATE, STATE_DISP(muldiv_completion_tick)), RWARG1);
    r->muldiv_done_cycle = 0;
    r->dirty_muldiv_done_cycle = true;
  }

  if ((flags & CPU_RC_FLUSH_CYCLES) && r->cycles > 0) {
    if (r->cycles == 1) {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), 1);
    } else {
      x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                    x64_mem(RSTATE, STATE_DISP(pending_ticks)), r->cycles);
    }
    /* gte_done_cycle = max(gte_done_cycle - cycles, 0) */
    s32 gd = r->gte_done_cycle - r->cycles;
    r->gte_done_cycle = (gd < 0) ? 0 : gd;
    /* muldiv_done_cycle similarly drains as cycles flush.  Only meaningful
     * when not dirty (clean static-cycle tracking); the dirty path already
     * wrote the absolute completion_tick to state. */
    if (!r->dirty_muldiv_done_cycle) {
      s32 md = r->muldiv_done_cycle - r->cycles;
      r->muldiv_done_cycle = (md < 0) ? 0 : md;
    }
    r->cycles = 0;
  }
}

static void hook_compile_fallback(cpu_recompiler_t* r)
{
  DEBUG_LOG("Compiling instruction fallback at PC=0x%08X, instruction=0x%08X",
            r->current_instruction_pc, r->inst->bits);

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

  x64_emit_t* e = current_emit(be_from(r));

  /* PC must point at the fallback op so g_interpret_block fetches it
   * first.  cpu_recompiler_flush(...PC) wrote r->compiler_pc, but for the
   * end-block-and-interpret path we want the *current* op address. */
  x64_mov_m_imm32(e, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pc)),
                  (s32)r->current_instruction_pc);

  /* Flush accumulated block cycles to pending_ticks (mirrors the prefix
   * of emit_end_and_link_block), since we bypass the normal end_block. */
  if (r->cycles == 1) {
    x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)), 1);
  } else if (r->cycles > 0) {
    x64_alu_m_imm(e, X64_ALU_ADD, X64_SZ_32,
                  x64_mem(RSTATE, STATE_DISP(pending_ticks)), r->cycles);
  }
  r->cycles = 0;

  /* Jump to g_interpret_block; never returns to JIT for this block. */
  u8* j = x64_jmp_rel32(e);
  x64_patch_rel32(j, g_interpret_block);

  cpu_recompiler_truncate_block(r);
  r->block_ended = true;  /* skip the agnostic-loop end_block emission */
}

static void hook_compile_andi(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 imm = cpu_instr_imm_zext32(*r->inst);
  if (imm != 0u) {
    const x64_reg_t rt = move_s_to_t(r, cf);
    x64_alu_r_imm(e, X64_ALU_AND, X64_SZ_32, rt, (s32)imm);
  } else {
    /* AND rt, $0 → rt = 0.  Avoid dependency on rs, just zero the dst. */
    const x64_reg_t rt = cf_get_reg_t(cf);
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, rt, rt);
  }
}

static void hook_compile_ori(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rt = move_s_to_t(r, cf);
  const u32 imm = cpu_instr_imm_zext32(*r->inst);
  if (imm != 0u)
    x64_alu_r_imm(e, X64_ALU_OR, X64_SZ_32, rt, (s32)imm);
}

static void hook_compile_xori(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rt = move_s_to_t(r, cf);
  const u32 imm = cpu_instr_imm_zext32(*r->inst);
  if (imm != 0u)
    x64_alu_r_imm(e, X64_ALU_XOR, X64_SZ_32, rt, (s32)imm);
}

/* Returns the rd host reg with cf.MipsT materialized into it. */
static x64_reg_t move_t_to_d(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(cf.valid_host_d);
  DebugAssert(!cf.valid_host_s || cf.host_s != cf.host_d);
  const x64_reg_t rd = cf_get_reg_d(cf);
  move_t_to_reg(r, rd, cf);
  return rd;
}

/* Switch current emitter to far code, optionally emitting a Jcc/JMP from
 * the near stream that targets the far cursor. */
static void switch_to_far_code(cpu_recompiler_t* r, bool emit_jump,
                               bool conditional, x64_cond_t cond)
{
  x64_backend_state_t* b = be_from(r);
  DebugAssert(!b->is_far);
  if (emit_jump) {
    void* const far_target = x64_emit_cursor(&b->far_emit);
    u8* site = conditional ? x64_jcc_rel32(&b->near_emit, cond)
                           : x64_jmp_rel32(&b->near_emit);
    x64_patch_rel32(site, far_target);
  }
  b->is_far = true;
}

static void switch_to_near_code(cpu_recompiler_t* r, bool emit_jump,
                                bool conditional, x64_cond_t cond)
{
  x64_backend_state_t* b = be_from(r);
  DebugAssert(b->is_far);
  if (emit_jump) {
    void* const near_target = x64_emit_cursor(&b->near_emit);
    u8* site = conditional ? x64_jcc_rel32(&b->far_emit, cond)
                           : x64_jmp_rel32(&b->far_emit);
    x64_patch_rel32(site, near_target);
  }
  b->is_far = false;
}

/* Emit a `jo` into far code that raises an Overflow exception, then
 * continue near code. */
static void test_overflow(cpu_recompiler_t* r, x64_reg_t result_reg)
{
  switch_to_far_code(r, /*emit_jump=*/true, /*conditional=*/true, X64_CC_O);
  cpu_recompiler_backup_host_state(r);
  cpu_recompiler_clear_host_reg(r, (u32)result_reg);
  hook_end_block_with_exception(r, CPU_EXCEPTION_OV);
  cpu_recompiler_restore_host_state(r);
  switch_to_near_code(r, /*emit_jump=*/false, /*conditional=*/false, X64_CC_O);
}

 /* Generic "(op) rd, rs, rt" for ALU register-register ops.  `op` selects
 * x64_alu_t variant. */
static void compile_dst_op(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                           x64_alu_t op, bool commutative, bool overflow)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (cf.valid_host_s && cf.valid_host_t) {
    if (cf.host_d == cf.host_s) {
      x64_alu_r_r(e, op, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_t(cf));
    } else if (cf.host_d == cf.host_t) {
      if (commutative) {
        x64_alu_r_r(e, op, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_s(cf));
      } else {
        x64_mov_r_r(e, X64_SZ_32, RWARG1, cf_get_reg_t(cf));
        x64_mov_r_r(e, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_s(cf));
        x64_alu_r_r(e, op, X64_SZ_32, cf_get_reg_d(cf), RWARG1);
      }
    } else {
      x64_mov_r_r(e, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_s(cf));
      x64_alu_r_r(e, op, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_t(cf));
    }
  } else if (commutative && (cf.const_s || cf.const_t)) {
    const x64_reg_t rd = cf_get_reg_d(cf);
    if (cf.const_s) move_t_to_reg(r, rd, cf);
    else            move_s_to_reg(r, rd, cf);
    const u32 cv = cpu_recompiler_get_constant_reg_u32(
      r, cf.const_s ? cpu_recomp_cf_mips_s(cf) : cpu_recomp_cf_mips_t(cf));
    if (cv != 0u) x64_alu_r_imm(e, op, X64_SZ_32, rd, (s32)cv);
    else          overflow = false;
  } else if (cf.const_s) {
    if (cf.valid_host_d && cf.valid_host_t && cf.host_d == cf.host_t) {
      x64_mov_r_r (e, X64_SZ_32, RWARG1, cf_get_reg_t(cf));
      move_s_to_reg(r, cf_get_reg_d(cf), cf);
      x64_alu_r_r (e, op, X64_SZ_32, cf_get_reg_d(cf), RWARG1);
    } else {
      move_s_to_reg(r, cf_get_reg_d(cf), cf);
      x64_alu_r_r (e, op, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_t(cf));
    }
  } else if (cf.const_t) {
    move_s_to_reg(r, cf_get_reg_d(cf), cf);
    const u32 cv = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf));
    if (cv != 0u) x64_alu_r_imm(e, op, X64_SZ_32, cf_get_reg_d(cf), (s32)cv);
    else          overflow = false;
  } else if (cf.valid_host_s) {
    if (cf.host_d != cf.host_s)
      x64_mov_r_r(e, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_s(cf));
    x64_alu_r_m(e, op, X64_SZ_32, cf_get_reg_d(cf), mips_ptr(cpu_recomp_cf_mips_t(cf)));
  } else if (cf.valid_host_t) {
    if (cf.host_d != cf.host_t)
      x64_mov_r_r(e, X64_SZ_32, cf_get_reg_d(cf), cf_get_reg_t(cf));
    x64_alu_r_m(e, op, X64_SZ_32, cf_get_reg_d(cf), mips_ptr(cpu_recomp_cf_mips_s(cf)));
  } else {
    x64_mov_r_m(e, X64_SZ_32, cf_get_reg_d(cf), mips_ptr(cpu_recomp_cf_mips_s(cf)));
    x64_alu_r_m(e, op, X64_SZ_32, cf_get_reg_d(cf), mips_ptr(cpu_recomp_cf_mips_t(cf)));
  }
  if (overflow) {
    DebugAssert(cf.valid_host_d);
    test_overflow(r, cf_get_reg_d(cf));
  }
}

/* slti / sltiu shared core. */
static void compile_slti_inner(cpu_recompiler_t* r,
                               cpu_recomp_compile_flags_t cf, bool sign)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rt = cf.valid_host_t ? cf_get_reg_t(cf) : RWARG1;

  if (!cf.valid_host_t || !cf.valid_host_s || cf.host_t != cf.host_s)
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, rt, rt);

  if (cf.valid_host_s)
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf),
                  (s32)cpu_instr_imm_sext32(*r->inst));
  else
    x64_alu_m_imm(e, X64_ALU_CMP, X64_SZ_32, mips_ptr(cpu_recomp_cf_mips_s(cf)),
                  (s32)cpu_instr_imm_sext32(*r->inst));

  if (cf.valid_host_t && cf.valid_host_s && cf.host_t == cf.host_s)
    x64_mov_r_imm32(e, X64_SZ_32, rt, 0);

  x64_setcc_r(e, sign ? X64_CC_L : X64_CC_B, rt);

  if (!cf.valid_host_t)
    x64_mov_m_r(e, X64_SZ_32, mips_ptr(cpu_recomp_cf_mips_t(cf)), rt);
}

/* slt / sltu shared core. */
static void compile_slt_inner(cpu_recompiler_t* r,
                              cpu_recomp_compile_flags_t cf, bool sign)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rd = cf_get_reg_d(cf);
  const x64_reg_t rs = cf.valid_host_s ? cf_get_reg_s(cf) : RWARG1;
  const x64_reg_t rt = cf.valid_host_t ? cf_get_reg_t(cf) : RWARG1;
  if (!cf.valid_host_s)
    move_s_to_reg(r, rs, cf);

  if (rd != rs && rd != rt)
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, rd, rd);

  if (cf.valid_host_t)
    x64_alu_r_r (e, X64_ALU_CMP, X64_SZ_32, rs, cf_get_reg_t(cf));
  else if (cf.const_t)
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, rs,
                  (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
  else
    x64_alu_r_m (e, X64_ALU_CMP, X64_SZ_32, rs, mips_ptr(cpu_recomp_cf_mips_t(cf)));

  if (rd == rs || rd == rt)
    x64_mov_r_imm32(e, X64_SZ_32, rd, 0);

  x64_setcc_r(e, sign ? X64_CC_L : X64_CC_B, rd);
}

static void hook_compile_addi(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rt = move_s_to_t(r, cf);
  const u32 imm = cpu_instr_imm_sext32(*r->inst);
  if (imm != 0u) {
    x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, rt, (s32)imm);
    if (g_settings.cpu_recompiler_memory_exceptions) {
      DebugAssert(cf.valid_host_t);
      test_overflow(r, rt);
    }
  }
}

static void hook_compile_addiu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rt = move_s_to_t(r, cf);
  const u32 imm = cpu_instr_imm_sext32(*r->inst);
  if (imm != 0u)
    x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, rt, (s32)imm);
}

static void hook_compile_slti (cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_slti_inner(r, cf, true ); }
static void hook_compile_sltiu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_slti_inner(r, cf, false); }

static void hook_compile_add (cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_dst_op(r, cf, X64_ALU_ADD, /*commutative=*/true,  g_settings.cpu_recompiler_memory_exceptions); }
static void hook_compile_addu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_dst_op(r, cf, X64_ALU_ADD, true,  false); }
static void hook_compile_sub (cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_dst_op(r, cf, X64_ALU_SUB, false, g_settings.cpu_recompiler_memory_exceptions); }
static void hook_compile_subu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_dst_op(r, cf, X64_ALU_SUB, false, false); }

/* and/or/xor have peephole shortcuts (and-with-zero, or/xor-with-self/zero). */

static void hook_compile_and(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t regd = cf_get_reg_d(cf);
  if (cpu_recomp_cf_mips_s(cf) == cpu_recomp_cf_mips_t(cf)) {
    /* and rs, rs → rs. */
    move_s_to_reg(r, regd, cf);
    return;
  }
  if (cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_s(cf), 0u) ||
      cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_t(cf), 0u)) {
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, regd, regd);
    return;
  }
  compile_dst_op(r, cf, X64_ALU_AND, /*commutative=*/true, /*overflow=*/false);
}

static void hook_compile_or(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  const x64_reg_t regd = cf_get_reg_d(cf);
  if (cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_s(cf), 0u) ||
      cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_t(cf), 0u) ||
      cpu_recomp_cf_mips_s(cf) == cpu_recomp_cf_mips_t(cf)) {
    if (cf.const_s) move_t_to_reg(r, regd, cf);
    else            move_s_to_reg(r, regd, cf);
    return;
  }
  compile_dst_op(r, cf, X64_ALU_OR, /*commutative=*/true, /*overflow=*/false);
}

static void hook_compile_xor(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t regd = cf_get_reg_d(cf);
  if (cpu_recomp_cf_mips_s(cf) == cpu_recomp_cf_mips_t(cf)) {
    /* xor rs, rs → 0. */
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, regd, regd);
    return;
  }
  if (cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_s(cf), 0u) ||
      cpu_recompiler_has_constant_reg_value(r, cpu_recomp_cf_mips_t(cf), 0u)) {
    if (cf.const_s) move_t_to_reg(r, regd, cf);
    else            move_s_to_reg(r, regd, cf);
    return;
  }
  compile_dst_op(r, cf, X64_ALU_XOR, /*commutative=*/true, /*overflow=*/false);
}

static void hook_compile_nor(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  hook_compile_or(r, cf);
  x64_not_r(e, X64_SZ_32, cf_get_reg_d(cf));
}

static void hook_compile_slt (cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_slt_inner(r, cf, true ); }
static void hook_compile_sltu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_slt_inner(r, cf, false); }

static void hook_compile_sll(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rd = move_t_to_d(r, cf);
  const u8 shamt = (u8)r->inst->r.shamt;
  if (shamt > 0u)
    x64_shift_r_imm(e, X64_SH_SHL, X64_SZ_32, rd, shamt);
}

static void hook_compile_srl(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rd = move_t_to_d(r, cf);
  const u8 shamt = (u8)r->inst->r.shamt;
  if (shamt > 0u)
    x64_shift_r_imm(e, X64_SH_SHR, X64_SZ_32, rd, shamt);
}

static void hook_compile_sra(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rd = move_t_to_d(r, cf);
  const u8 shamt = (u8)r->inst->r.shamt;
  if (shamt > 0u)
    x64_shift_r_imm(e, X64_SH_SAR, X64_SZ_32, rd, shamt);
}

 /* Variable-shift core.  shift amount comes from rs (low 5 bits used by x86
 * SHL/SHR/SAR semantics, matches MIPS). */
static void compile_variable_shift_inner(cpu_recompiler_t* r,
                                          cpu_recomp_compile_flags_t cf,
                                         x64_shift_t op) 
{
  x64_emit_t* e = current_emit(be_from(r));
  const x64_reg_t rd = cf_get_reg_d(cf);
  if (!cf.const_s) {
    move_s_to_reg(r, X64_RCX, cf);  /* RCX is reserved (not USABLE) */
    move_t_to_reg(r, rd, cf);
    x64_shift_r_cl(e, op, X64_SZ_32, rd);
  } else {
    move_t_to_reg(r, rd, cf);
    const u32 sh = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_s(cf)) & 0x1Fu;
    if (sh > 0u)
      x64_shift_r_imm(e, op, X64_SZ_32, rd, (u8)sh);
  }
}

static void hook_compile_sllv(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_variable_shift_inner(r, cf, X64_SH_SHL); }
static void hook_compile_srlv(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_variable_shift_inner(r, cf, X64_SH_SHR); }
static void hook_compile_srav(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_variable_shift_inner(r, cf, X64_SH_SAR); }

/* Validate a branch target's alignment.  Mirrors interp's `cpu_branch`
 * (cpu_core.c:275) which always raises AdEL on misaligned target - the
 * `cpu_recompiler_memory_exceptions` toggle was a perf knob that
 * left a recomp/interp behavioural gap: interp would deliver AdEL at JR
 * time (so the BIOS exception handler picks it up at the *branch* cycle,
 * matching real hardware), while recomp would silently set state.pc to
 * the bad value and only raise AdEL later, at dispatcher time, via the
 * safety net (`cpu_recompiler_synth_pc_exception_if_invalid`).  The
 * dispatcher-side raise is correct AdEL but slightly later in cycle
 * accounting, which manifests as a once-per-minute "1 safety-net event"
 * visible in CUPID_TRACE on Crash Bandicoot.
 *
 * Always emit the alignment check, regardless of the
 * `cpu_recompiler_memory_exceptions` setting.  Cost is one TEST + JCC per
 * JR/JALR (cheap; the not-taken path is statically predicted).  Matches
 * interp's per-instruction AdEL semantics. */
static void check_branch_target(cpu_recompiler_t* r, x64_reg_t pcreg)
{
  x64_emit_t* e = current_emit(be_from(r));
  x64_test_r_imm(e, X64_SZ_32, pcreg, 0x3);
  switch_to_far_code(r, /*emit_jump=*/true, /*conditional=*/true, X64_CC_NE);
  cpu_recompiler_backup_host_state(r);
  hook_end_block_with_exception(r, CPU_EXCEPTION_ADEL);
  cpu_recompiler_restore_host_state(r);
  switch_to_near_code(r, /*emit_jump=*/false, /*conditional=*/false, X64_CC_NE);
}

static void hook_compile_jr(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (!cf.valid_host_s)
    x64_mov_r_m(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_s(cf)));

  const x64_reg_t pcreg = cf.valid_host_s ? cf_get_reg_s(cf) : RWARG1;
  check_branch_target(r, pcreg);

  /* Write pc directly from the host register holding the target. */
  x64_mov_m_r(e, X64_SZ_32, x64_mem(RSTATE, STATE_DISP(pc)), pcreg);

  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, false);
  if (r->v && r->v->end_block)
    r->v->end_block(r, NULL, true);
  r->block_ended = true;
}

static void hook_compile_jalr(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (!cf.valid_host_s)
    x64_mov_r_m(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_s(cf)));

  const x64_reg_t pcreg = cf.valid_host_s ? cf_get_reg_s(cf) : RWARG1;

  if (cpu_recompiler_mips_d(r) != (cpu_reg_t)0)
    cpu_recompiler_set_constant_reg(r, cpu_recompiler_mips_d(r),
                                    cpu_recompiler_get_branch_return_address(r, cf));

  check_branch_target(r, pcreg);
  x64_mov_m_r(e, X64_SZ_32, x64_mem(RSTATE, STATE_DISP(pc)), pcreg);

  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, false);
  if (r->v && r->v->end_block)
    r->v->end_block(r, NULL, true);
  r->block_ended = true;
}

/* Conditional branch core.  Emits cmp/test + Jcc to the taken path, then
 * falls through to compile the not-taken delay slot + end_block(fall),
 * then resolves the taken label, restores host state, compiles the taken
 * delay slot + end_block(taken_pc). */
static void hook_compile_bxx(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                             cpu_recomp_branch_cond_t cond)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 taken_pc = cpu_recompiler_get_conditional_branch_target(r, cf);

  cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_BRANCH);

  DebugAssert(cf.valid_host_s);

  u8* taken_site = NULL;
  switch (cond) {
    case CPU_RC_BC_EQ:
    case CPU_RC_BC_NE:
      DebugAssert(cond == CPU_RC_BC_EQ || cond == CPU_RC_BC_NE
                  || cpu_recomp_cf_mips_t(cf) == (cpu_reg_t)0);
      if (cf.valid_host_t) {
        x64_alu_r_r(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf), cf_get_reg_t(cf));
      } else if (cf.const_t) {
        x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf),
                      (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
      } else {
        x64_alu_r_m(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf),
                    mips_ptr(cpu_recomp_cf_mips_t(cf)));
      }
      taken_site = x64_jcc_rel32(e, (cond == CPU_RC_BC_EQ) ? X64_CC_E : X64_CC_NE);
      break;

    case CPU_RC_BC_GT_ZERO:
      x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf), 0);
      taken_site = x64_jcc_rel32(e, X64_CC_G);
      break;

    case CPU_RC_BC_GE_ZERO:
      x64_test_r_r(e, X64_SZ_32, cf_get_reg_s(cf), cf_get_reg_s(cf));
      taken_site = x64_jcc_rel32(e, X64_CC_NS);
      break;

    case CPU_RC_BC_LT_ZERO:
      x64_test_r_r(e, X64_SZ_32, cf_get_reg_s(cf), cf_get_reg_s(cf));
      taken_site = x64_jcc_rel32(e, X64_CC_S);
      break;

    case CPU_RC_BC_LE_ZERO:
      x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, cf_get_reg_s(cf), 0);
      taken_site = x64_jcc_rel32(e, X64_CC_LE);
      break;

    default:
      Panic("Unhandled branch condition");
  }

  /* Not-taken path. */
  cpu_recompiler_backup_host_state(r);
  if (!cf.delay_slot_swapped) {
    /* compiler_pc here = delay_slot_pc; fall-through is one instruction past. */
    const u32 newpc = r->compiler_pc + CPU_INSTRUCTION_SIZE;
    if (r->v && r->v->store_constant_to_cpu_pointer)
      r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
    r->delay_slot_branch_was_taken = false;
    cpu_recompiler_compile_branch_delay_slot(r, false);
  }
  {
    const u32 newpc = r->compiler_pc;
    if (r->v && r->v->end_block)
      r->v->end_block(r, &newpc, true);
  }

  /* Taken path. */
  void* const taken_target = x64_emit_cursor(current_emit(be_from(r)));
  x64_patch_rel32(taken_site, taken_target);

  cpu_recompiler_restore_host_state(r);
  if (!cf.delay_slot_swapped) {
    if (r->v && r->v->store_constant_to_cpu_pointer)
      r->v->store_constant_to_cpu_pointer(r, taken_pc, &g_cpu_state.pc);
    r->delay_slot_branch_was_taken = true;
    cpu_recompiler_compile_branch_delay_slot(r, false);
  }
  if (r->v && r->v->end_block)
    r->v->end_block(r, &taken_pc, true);

  r->block_ended = true;
}

/* MULT/MULTU runtime cycle-count emission: rs (in RAX) → cycles in RCX
 * (5/8/12), based on cpu_get_mult_ticks_signed/unsigned bucketing
 * (cpu_core_private.h:165-175).  Asymmetric sign bucket boundaries
 * (rs=-2048 → 5, rs=+2048 → 8) require a branch on sign for the signed
 * case.  Preserves RAX (caller's rs).  Branchy codegen (~25 bytes), small
 * and predictable. */
static void emit_compute_mult_cycles_to_rcx(x64_emit_t* e, bool sign)
{
  x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 12);
  if (sign) {
    x64_test_r_r(e, X64_SZ_32, X64_RAX, X64_RAX);
    u8* j_neg = x64_jcc_rel32(e, X64_CC_S);

    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, 0x100000);
    u8* j_pos_default = x64_jcc_rel32(e, X64_CC_AE);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 8);
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, 0x800);
    u8* j_pos_eight = x64_jcc_rel32(e, X64_CC_AE);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 5);
    u8* j_pos_done = x64_jmp_rel32(e);

    void* neg_label = x64_emit_cursor(e);
    x64_patch_rel32(j_neg, neg_label);
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, -1048576);
    u8* j_neg_default = x64_jcc_rel32(e, X64_CC_L);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 8);
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, -2048);
    u8* j_neg_eight = x64_jcc_rel32(e, X64_CC_L);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 5);

    void* done_label = x64_emit_cursor(e);
    x64_patch_rel32(j_pos_default, done_label);
    x64_patch_rel32(j_pos_eight,   done_label);
    x64_patch_rel32(j_pos_done,    done_label);
    x64_patch_rel32(j_neg_default, done_label);
    x64_patch_rel32(j_neg_eight,   done_label);
  } else {
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, 0x100000);
    u8* j_default = x64_jcc_rel32(e, X64_CC_AE);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 8);
    x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, 0x800);
    u8* j_eight = x64_jcc_rel32(e, X64_CC_AE);
    x64_mov_r_imm32(e, X64_SZ_32, X64_RCX, 5);
    void* done_label = x64_emit_cursor(e);
    x64_patch_rel32(j_default, done_label);
    x64_patch_rel32(j_eight,   done_label);
  }
}

/* MULT/MULTU emit a runtime stash to muldiv_completion_tick: cycles depend
 * on rs's value, so we can't statically compile-time-track it like DIV.
 * After the multiply, sets r->dirty_muldiv_done_cycle = true so the
 * MFHI/MFLO/MTHI/MTLO consumer flushes from state via
 * CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE. */
static void compile_mult_inner(cpu_recompiler_t* r,
                               cpu_recomp_compile_flags_t cf, bool sign)
{
  x64_emit_t* e = current_emit(be_from(r));

  move_s_to_reg(r, X64_RAX, cf);

  /* While RAX still holds rs (clobbered by imul/mul below), compute the
   * cycle count into RCX and stash muldiv_completion_tick to state.
   *   muldiv_completion_tick = pending_ticks + r->cycles + cycles(rs)
   * Mirrors interp's cpu_add_muldiv_ticks(cpu_get_mult_ticks_*(rs)). */
  emit_compute_mult_cycles_to_rcx(e, sign);
  x64_mov_r_m(e, X64_SZ_32, RWARG1,
              x64_mem(RSTATE, STATE_DISP(pending_ticks)));
  if (r->cycles == 1)      x64_inc_r(e, X64_SZ_32, RWARG1);
  else if (r->cycles > 0)  x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, RWARG1, r->cycles);
  x64_alu_r_r(e, X64_ALU_ADD, X64_SZ_32, RWARG1, X64_RCX);
  x64_mov_m_r(e, X64_SZ_32,
              x64_mem(RSTATE, STATE_DISP(muldiv_completion_tick)), RWARG1);
  r->dirty_muldiv_done_cycle = true;

  if (cf.valid_host_t) {
    if (sign) x64_imul1_r(e, X64_SZ_32, cf_get_reg_t(cf));
    else      x64_mul1_r (e, X64_SZ_32, cf_get_reg_t(cf));
  } else if (cf.const_t) {
    x64_mov_r_imm32(e, X64_SZ_32, X64_RDX,
                    (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
    if (sign) x64_imul1_r(e, X64_SZ_32, X64_RDX);
    else      x64_mul1_r (e, X64_SZ_32, X64_RDX);
  } else {
    x64_mov_r_m(e, X64_SZ_32, X64_RDX, mips_ptr(cpu_recomp_cf_mips_t(cf)));
    if (sign) x64_imul1_r(e, X64_SZ_32, X64_RDX);
    else      x64_mul1_r (e, X64_SZ_32, X64_RDX);
  }

  if (cf.valid_host_lo)
    x64_mov_r_r(e, X64_SZ_32, cf_get_reg_lo(cf), X64_RAX);
  else
    x64_mov_m_r(e, X64_SZ_32, x64_mem(RSTATE, STATE_REG_DISP((u32)CPU_REG_LO)), X64_RAX);
  if (cf.valid_host_hi)
    x64_mov_r_r(e, X64_SZ_32, cf_get_reg_hi(cf), X64_RDX);
  else
    x64_mov_m_r(e, X64_SZ_32, x64_mem(RSTATE, STATE_REG_DISP((u32)CPU_REG_HI)), X64_RDX);
}

static void hook_compile_mult (cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_mult_inner(r, cf, true ); }
static void hook_compile_multu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ compile_mult_inner(r, cf, false); }

 /* Signed div: special-cases divide-by-zero (lo=±1, hi=num) and -2^31 / -1
 * (lo=-2^31, hi=0) to match MIPS R3000A semantics. */
static void hook_compile_div(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  DebugAssert(cf.valid_host_lo && cf.valid_host_hi);

  const x64_reg_t rt = cf.valid_host_t ? cf_get_reg_t(cf) : X64_RCX;
  if (!cf.valid_host_t)
    move_t_to_reg(r, rt, cf);

  const x64_reg_t rlo = cf_get_reg_lo(cf);
  const x64_reg_t rhi = cf_get_reg_hi(cf);

  move_s_to_reg(r, X64_RAX, cf);
  x64_cdq(e);

  /* if (rt == 0) { hi = num; lo = (s>=0) ? -1 : 1; goto done; } */
  x64_test_r_r(e, X64_SZ_32, rt, rt);
  u8* j_not_zero = x64_jcc_rel32(e, X64_CC_NE);

  x64_test_r_r  (e, X64_SZ_32, X64_RAX, X64_RAX);
  x64_mov_r_r   (e, X64_SZ_32, rhi, X64_RAX);   /* hi = num */
  x64_mov_r_imm32(e, X64_SZ_32, rlo, 1);
  x64_mov_r_imm32(e, X64_SZ_32, X64_RAX, -1);
  x64_cmovcc_r_r(e, X64_CC_NS, X64_SZ_32, rlo, X64_RAX);
  u8* j_done_a = x64_jmp_rel32(e);

  void* not_zero = x64_emit_cursor(e);
  x64_patch_rel32(j_not_zero, not_zero);

  /* if (eax == 0x80000000 && rt == -1) { lo = 0x80000000; hi = 0; goto done; } */
  x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, X64_RAX, (s32)0x80000000u);
  u8* j_not_unrep_a = x64_jcc_rel32(e, X64_CC_NE);
  x64_alu_r_imm(e, X64_ALU_CMP, X64_SZ_32, rt, -1);
  u8* j_not_unrep_b = x64_jcc_rel32(e, X64_CC_NE);

  x64_mov_r_imm32(e, X64_SZ_32, rlo, (s32)0x80000000u);
  x64_alu_r_r   (e, X64_ALU_XOR, X64_SZ_32, rhi, rhi);
  u8* j_done_b = x64_jmp_rel32(e);

  void* not_unrep = x64_emit_cursor(e);
  x64_patch_rel32(j_not_unrep_a, not_unrep);
  x64_patch_rel32(j_not_unrep_b, not_unrep);

  x64_idiv_r(e, X64_SZ_32, rt);
  x64_mov_r_r(e, X64_SZ_32, rlo, X64_RAX);
  x64_mov_r_r(e, X64_SZ_32, rhi, X64_RDX);

  void* done = x64_emit_cursor(e);
  x64_patch_rel32(j_done_a, done);
  x64_patch_rel32(j_done_b, done);

  /* DIV cycle count is constant 35; track in r->muldiv_done_cycle (no
   * runtime stash, in-block stall consumer uses r->cycles math). */
  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_div_ticks());
}

/* Unsigned div: divide-by-zero gives lo=-1, hi=num. */
static void hook_compile_divu(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  DebugAssert(cf.valid_host_lo && cf.valid_host_hi);

  const x64_reg_t rt = cf.valid_host_t ? cf_get_reg_t(cf) : X64_RCX;
  if (!cf.valid_host_t)
    move_t_to_reg(r, rt, cf);

  const x64_reg_t rlo = cf_get_reg_lo(cf);
  const x64_reg_t rhi = cf_get_reg_hi(cf);

  move_s_to_reg(r, X64_RAX, cf);
  x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, X64_RDX, X64_RDX);

  /* if (rt == 0) { lo = -1; hi = num; goto done; } */
  x64_test_r_r(e, X64_SZ_32, rt, rt);
  u8* j_not_zero = x64_jcc_rel32(e, X64_CC_NE);

  x64_mov_r_imm32(e, X64_SZ_32, rlo, -1);
  x64_mov_r_r   (e, X64_SZ_32, rhi, X64_RAX);
  u8* j_done = x64_jmp_rel32(e);

  void* not_zero = x64_emit_cursor(e);
  x64_patch_rel32(j_not_zero, not_zero);

  x64_div_r(e, X64_SZ_32, rt);
  x64_mov_r_r(e, X64_SZ_32, rlo, X64_RAX);
  x64_mov_r_r(e, X64_SZ_32, rhi, X64_RDX);

  void* done = x64_emit_cursor(e);
  x64_patch_rel32(j_done, done);

  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_div_ticks());
}

static x64_reg_t compute_loadstore_address(cpu_recompiler_t* r,
                                           cpu_recomp_compile_flags_t cf,
                                           const u32* known_addr)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 imm = cpu_instr_imm_sext32(*r->inst);

  if (cf.valid_host_s && imm == 0u && known_addr == NULL)
    return cf_get_reg_s(cf);

  const x64_reg_t dst = RWARG1;
  if (known_addr != NULL) {
    x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)*known_addr);
  } else {
    if (cf.valid_host_s) {
      const x64_reg_t src = cf_get_reg_s(cf);
      if (src != dst)
        x64_mov_r_r(e, X64_SZ_32, dst, src);
    } else if (cf.const_s) {
      x64_mov_r_imm32(e, X64_SZ_32, dst,
                      (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_s(cf)));
    } else {
      x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(cpu_recomp_cf_mips_s(cf)));
    }
    if (imm != 0u)
      x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, dst, (s32)imm);
  }
  return dst;
}

/* Resolve C thunk targets per access size + direction. */
static const void* read_memory_thunk(memory_access_size_t size)
{
  switch (size) {
    case MEMORY_ACCESS_SIZE_BYTE:     return (const void*)&cpu_recompiler_thunk_read_memory_byte;
    case MEMORY_ACCESS_SIZE_HALFWORD: return (const void*)&cpu_recompiler_thunk_read_memory_halfword;
    case MEMORY_ACCESS_SIZE_WORD:     return (const void*)&cpu_recompiler_thunk_read_memory_word;
    default:                          return NULL;
  }
}
static const void* write_memory_thunk(memory_access_size_t size)
{
  switch (size) {
    case MEMORY_ACCESS_SIZE_BYTE:     return (const void*)&cpu_recompiler_thunk_write_memory_byte;
    case MEMORY_ACCESS_SIZE_HALFWORD: return (const void*)&cpu_recompiler_thunk_write_memory_halfword;
    case MEMORY_ACCESS_SIZE_WORD:     return (const void*)&cpu_recompiler_thunk_write_memory_word;
    default:                          return NULL;
  }
}

/* Pad emitter cursor up to `target_size` bytes from `start_cursor` with
 * multi-byte NOPs.  Used by the fastmem path to guarantee the original
 * call site is at least 5 bytes (the size of a JMP rel32 backpatch). */
static u32 pad_to_min_size(x64_emit_t* e, void* start_cursor, u32 min_size)
{
  const u32 emitted = (u32)((u8*)x64_emit_cursor(e) - (u8*)start_cursor);
  if (emitted < min_size)
    x64_nop_n(e, min_size - emitted);
  return (u32)((u8*)x64_emit_cursor(e) - (u8*)start_cursor);
}

/* Load compile path.  PGXP deferred (M6).  Fastmem path
 * (M4c): emit `mov data, [RBX + addr]` (with size variants), pad to >=5
 * bytes for backpatch, register backpatch info.  Slow-path: existing
 * thunk call. */
static void hook_compile_lxx(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                             memory_access_size_t size, bool sign,
                             bool use_fastmem, const u32* known_addr)
{
  x64_emit_t* e = current_emit(be_from(r));

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/false, use_fastmem);

  const cpu_reg_t mips_t = cpu_recomp_cf_mips_t(cf);

  if (use_fastmem) {
    /* Pre-charge RAM read ticks the same way the slow-path bus
     * handler (`ram_read_*` in bus.c) charges them at runtime.  Without
     * this, fastmem loads cost 0 ticks while slow-path loads cost
     * BUS_RAM_READ_TICKS, and the per-block tick total drifts away from
     * the interpreter's (which always goes through the slow-path handler).
     * Stores are not pre-charged because RAM writes do not stall (see
     * ram_write_* in bus.c, which call no BUS_CYCLES). */
    r->cycles += (s32)BUS_RAM_READ_TICKS;

    const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);
    /* Allocate the destination host reg first so we can emit straight into it. */
    if (mips_t != (cpu_reg_t)0) {
      const u32 vreg = cpu_recompiler_allocate_host_reg(
        r, cpu_recompiler_get_flags_for_new_load_delayed_reg(r),
        CPU_RECOMP_EMULATE_LOAD_DELAYS ? CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG, 
        mips_t);
      const x64_reg_t dst = (x64_reg_t)vreg;
      const x64_mem_bis_t mem = x64_msib(X64_RBX, addr, 1, 0);
      void* site = x64_emit_cursor(e);
      switch (size) {
        case MEMORY_ACCESS_SIZE_BYTE:
          if (sign) x64_movsx_r_msib8 (e, X64_SZ_32, dst, mem);
          else      x64_movzx_r_msib8 (e, X64_SZ_32, dst, mem);
          break;
        case MEMORY_ACCESS_SIZE_HALFWORD:
          if (sign) x64_movsx_r_msib16(e, X64_SZ_32, dst, mem);
          else      x64_movzx_r_msib16(e, X64_SZ_32, dst, mem);
          break;
        case MEMORY_ACCESS_SIZE_WORD:
          x64_mov_r_msib(e, X64_SZ_32, dst, mem);
          break;
      }
      const u32 emitted = pad_to_min_size(e, site, 5u);
      cpu_code_cache_add_load_store_info(site, emitted,
                                         r->current_instruction_pc, r->block->pc,
                                         r->cycles, /*gpr_bitmask=*/0u,
                                         (u8)addr, (u8)dst, size, sign, /*is_load=*/true);
    } else {
      /* lxx with rt=$zero in fastmem: side-effect-only.  No host writeback,
       * but PSX still requires the load to fire (cache + MMIO).  Best to
       * emit a synthetic dst-into-RAX read so the slow path on fault still
       * has a place to put the value before discarding it. */
      const x64_mem_bis_t mem = x64_msib(X64_RBX, addr, 1, 0);
      void* site = x64_emit_cursor(e);
      switch (size) {
        case MEMORY_ACCESS_SIZE_BYTE:
          if (sign) x64_movsx_r_msib8 (e, X64_SZ_32, X64_RAX, mem);
          else      x64_movzx_r_msib8 (e, X64_SZ_32, X64_RAX, mem);
          break;
        case MEMORY_ACCESS_SIZE_HALFWORD:
          if (sign) x64_movsx_r_msib16(e, X64_SZ_32, X64_RAX, mem);
          else      x64_movzx_r_msib16(e, X64_SZ_32, X64_RAX, mem);
          break;
        case MEMORY_ACCESS_SIZE_WORD:
          x64_mov_r_msib(e, X64_SZ_32, X64_RAX, mem);
          break;
      }
      const u32 emitted = pad_to_min_size(e, site, 5u);
      cpu_code_cache_add_load_store_info(site, emitted,
                                         r->current_instruction_pc, r->block->pc,
                                         r->cycles, /*gpr_bitmask=*/0u,
                                         (u8)addr, (u8)X64_RAX, size, sign, /*is_load=*/true);
    }
    return;
  }

  const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);

  /* Inline alignment check.  Slow-path thunk's cpu_read_memory_halfword/word
   * also do this check, but a thunk-raised exception only mutates state -
   * the recomp block keeps running and end_block then writes
   * pc=block-fall-through, clobbering the exception vector npc set up by
   * raise_exception.  Catch misalignment here and bail via
   * end_block_with_exception (newpc=NULL → pc isn't overwritten).
   * Mirrors the JR/JALR check_branch_target pattern. */
  if (g_settings.cpu_recompiler_memory_exceptions && size != MEMORY_ACCESS_SIZE_BYTE) {
    const u32 mask = (size == MEMORY_ACCESS_SIZE_HALFWORD) ? 1u : 3u;
    x64_test_r_imm(e, X64_SZ_32, addr, (s32)mask);
    switch_to_far_code(r, /*emit_jump=*/true, /*conditional=*/true, X64_CC_NE);
    cpu_recompiler_backup_host_state(r);
    hook_end_block_with_exception(r, CPU_EXCEPTION_ADEL);
    cpu_recompiler_restore_host_state(r);
    switch_to_near_code(r, /*emit_jump=*/false, /*conditional=*/false, X64_CC_NE);
  }

  if (addr != RWARG1)
    x64_mov_r_r(e, X64_SZ_32, RWARG1, addr);

  x64_call_abs64(e, read_memory_thunk(size));

  if (mips_t == (cpu_reg_t)0)
    return;

  const u32 vreg = cpu_recompiler_allocate_host_reg(
    r, cpu_recompiler_get_flags_for_new_load_delayed_reg(r),
    CPU_RECOMP_EMULATE_LOAD_DELAYS ? CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG, 
    mips_t);

  switch (size) {
    case MEMORY_ACCESS_SIZE_BYTE:
      if (sign) x64_movsx_r_r8 (e, X64_SZ_32, (x64_reg_t)vreg, RWRET);
      else      x64_movzx_r_r8 (e, X64_SZ_32, (x64_reg_t)vreg, RWRET);
      break;
    case MEMORY_ACCESS_SIZE_HALFWORD:
      if (sign) x64_movsx_r_r16(e, X64_SZ_32, (x64_reg_t)vreg, RWRET);
      else      x64_movzx_r_r16(e, X64_SZ_32, (x64_reg_t)vreg, RWRET);
      break;
    case MEMORY_ACCESS_SIZE_WORD:
      x64_mov_r_r(e, X64_SZ_32, (x64_reg_t)vreg, RWRET);
      break;
  }
}

/* Store compile path.  Fastmem path emits `mov [RBX+addr],
 * data` and registers the backpatch entry; slow path goes through the
 * existing thunk call. */
static void hook_compile_sxx(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                             memory_access_size_t size, bool sign,
                             bool use_fastmem, const u32* known_addr)
{
  (void)sign;
  x64_emit_t* e = current_emit(be_from(r));

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/true, use_fastmem);

  if (use_fastmem) {
    const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);

    /* Materialize value into a host reg.  If T is already in a host reg,
     * use it directly.  Otherwise stage into RAX (caller-saved scratch).
     * RAX is safe because fastmem path doesn't call C functions. */
    x64_reg_t data_reg;
    if (cf.valid_host_t) {
      data_reg = cf_get_reg_t(cf);
    } else if (cf.const_t) {
      x64_mov_r_imm32(e, X64_SZ_32, X64_RAX,
                      (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
      data_reg = X64_RAX;
    } else {
      x64_mov_r_m(e, X64_SZ_32, X64_RAX, mips_ptr(cpu_recomp_cf_mips_t(cf)));
      data_reg = X64_RAX;
    }

    const x64_mem_bis_t mem = x64_msib(X64_RBX, addr, 1, 0);
    void* site = x64_emit_cursor(e);
    x64_mov_msib_r(e, (size == MEMORY_ACCESS_SIZE_WORD) ? X64_SZ_32 :
                       (size == MEMORY_ACCESS_SIZE_HALFWORD) ? X64_SZ_16 : X64_SZ_8,
                   mem, data_reg);
    const u32 emitted = pad_to_min_size(e, site, 5u);
    cpu_code_cache_add_load_store_info(site, emitted,
                                       r->current_instruction_pc, r->block->pc,
                                       r->cycles, /*gpr_bitmask=*/0u,
                                       (u8)addr, (u8)data_reg, size, /*sign=*/false, /*is_load=*/false);
    return;
  }

  if (cf.valid_host_t) {
    if (cf_get_reg_t(cf) != RWARG2)
      x64_mov_r_r(e, X64_SZ_32, RWARG2, cf_get_reg_t(cf));
  } else if (cf.const_t) {
    x64_mov_r_imm32(e, X64_SZ_32, RWARG2,
                    (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
  } else {
    x64_mov_r_m(e, X64_SZ_32, RWARG2, mips_ptr(cpu_recomp_cf_mips_t(cf)));
  }

  const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);

  if (g_settings.cpu_recompiler_memory_exceptions && size != MEMORY_ACCESS_SIZE_BYTE) {
    const u32 mask = (size == MEMORY_ACCESS_SIZE_HALFWORD) ? 1u : 3u;
    x64_test_r_imm(e, X64_SZ_32, addr, (s32)mask);
    switch_to_far_code(r, /*emit_jump=*/true, /*conditional=*/true, X64_CC_NE);
    cpu_recompiler_backup_host_state(r);
    hook_end_block_with_exception(r, CPU_EXCEPTION_ADES);
    cpu_recompiler_restore_host_state(r);
    switch_to_near_code(r, /*emit_jump=*/false, /*conditional=*/false, X64_CC_NE);
  }

  if (addr != RWARG1)
    x64_mov_r_r(e, X64_SZ_32, RWARG1, addr);

  x64_call_abs64(e, write_memory_thunk(size));
}

static void emit_load_mips_to_reg(cpu_recompiler_t* r, x64_reg_t dst, cpu_reg_t mips)
{
  x64_emit_t* e = current_emit(be_from(r));
  if (cpu_recompiler_has_constant_reg(r, mips)) {
    x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)cpu_recompiler_get_constant_reg_u32(r, mips));
    return;
  }
  cpu_recomp_opt_u32_t hreg = cpu_recompiler_check_host_reg(r, CPU_RC_HR_MODE_READ,
                                                            CPU_RC_HRT_CPU_REG, mips);
  if (hreg.has) {
    if ((x64_reg_t)hreg.val != dst)
      x64_mov_r_r(e, X64_SZ_32, dst, (x64_reg_t)hreg.val);
    return;
  }
  x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(mips));
}

/* Compile a load/store address into a callee-saved temp `addr`, identical
 * to compute_loadstore_address but with a caller-supplied destination so
 * the value survives the slow-path C thunk call. */
static void compute_loadstore_address_into(cpu_recompiler_t* r,
                                           cpu_recomp_compile_flags_t cf,
                                           const u32* known_addr,
                                           x64_reg_t dst)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 imm = cpu_instr_imm_sext32(*r->inst);

  if (known_addr != NULL) {
    x64_mov_r_imm32(e, X64_SZ_32, dst, (s32)*known_addr);
    return;
  }
  if (cf.valid_host_s) {
    const x64_reg_t src = cf_get_reg_s(cf);
    if (src != dst)
      x64_mov_r_r(e, X64_SZ_32, dst, src);
  } else if (cf.const_s) {
    x64_mov_r_imm32(e, X64_SZ_32, dst,
                    (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_s(cf)));
  } else {
    x64_mov_r_m(e, X64_SZ_32, dst, mips_ptr(cpu_recomp_cf_mips_s(cf)));
  }
  if (imm != 0u)
    x64_alu_r_imm(e, X64_ALU_ADD, X64_SZ_32, dst, (s32)imm);
}

static void hook_compile_lwx(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                             memory_access_size_t size, bool sign,
                             bool use_fastmem, const u32* known_addr)
{
  (void)size; (void)sign; (void)use_fastmem;
  x64_emit_t* e = current_emit(be_from(r));

  /* Need addr after the read_memory_thunk C call (for the *8 shift + the
   * existing-value merge), so park it in a callee-saved temp. */
  const u32 addr_idx = cpu_recompiler_allocate_temp_host_reg(r, CPU_RC_HR_CALLEE_SAVED);
  const x64_reg_t addr = (x64_reg_t)addr_idx;

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/false, /*use_fastmem=*/false);

  if (r->load_delay_dirty)
    cpu_recompiler_update_load_delay(r);

  compute_loadstore_address_into(r, cf, known_addr, addr);

  /* Word-aligned load into RWRET. */
  x64_mov_r_r  (e, X64_SZ_32, RWARG1, addr);
  x64_alu_r_imm(e, X64_ALU_AND, X64_SZ_32, RWARG1, (s32)~0x3u);
  x64_call_abs64(e, (const void*)&cpu_recompiler_thunk_read_memory_word);

  /* lwx with rt=$zero: still does the load (side-effect) but no merge. */
  const cpu_reg_t rt = (cpu_reg_t)r->inst->r.rt;
  if (rt == (cpu_reg_t)0) {
    cpu_recompiler_free_host_reg(r, addr_idx);
    return;
  }

  /* An LWL+LWR pair (or LWR+LWL) on the same rt within one block relies
   * on the second op merging with the first op's pending load-delay
   * result, NOT with the stale CPU_REG copy.  Mirrors the interp's
   *   existing = (rt == load_delay_reg) ? load_delay_value : reg[rt]
   * (cpu_core.c:799-801).  Crash Bandicoot's CDROM driver hits this at
   * 0x8002FB14 (lwl v0,0x5(s0); lwr v0,0x2(s0)); without the forward the
   * LBA->MSF input is corrupt, the CDROM rejects every Setloc with non-BCD
   * nibbles (40:BB:FC, 47:F7:9D), and the game freezes ~28 s in.
   *
   * After update_load_delay above, the pending value's host-reg type may
   * be LOAD_DELAY_VALUE (current cycle, just shifted) or NEXT_LOAD_DELAY_VALUE
   * (this cycle, newly set).  Check both. */
  cpu_recomp_opt_u32_t pending_idx = cpu_recompiler_check_host_reg(
    r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_LOAD_DELAY_VALUE, rt);
  if (!pending_idx.has) {
    pending_idx = cpu_recompiler_check_host_reg(
      r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE, rt);
  }
  /* If the pending value was flushed to cpu_state (no host-reg cache hit
   * but load_delay_register still tracks rt), read it from memory. */
  bool pending_in_state = !pending_idx.has &&
                          (r->load_delay_register == rt ||
                           r->next_load_delay_register == rt);

  const u32 value_idx = cpu_recompiler_allocate_host_reg(
    r, cpu_recompiler_get_flags_for_lwx_value_reg(r),
    CPU_RECOMP_EMULATE_LOAD_DELAYS ? CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG,
    rt);
  const x64_reg_t value = (x64_reg_t)value_idx;
  if (pending_idx.has && (x64_reg_t)pending_idx.val != value) {
    x64_mov_r_r(e, X64_SZ_32, value, (x64_reg_t)pending_idx.val);
  } else if (pending_idx.has) {
    /* allocator returned the same hreg the pending value was already in -
     * nothing to do, value already holds the in-flight load result. */
  } else if (pending_in_state) {
    /* Pending load was flushed; pick it back up from cpu_state. */
    x64_mov_r_m(e, X64_SZ_32, value,
                x64_mem(RSTATE,
                        (r->load_delay_register == rt)
                          ? STATE_DISP(load_delay_value)
                          : STATE_DISP(next_load_delay_value)));
  } else {
    emit_load_mips_to_reg(r, value, rt);
  }

  /* shift = (addr & 3) * 8;  RWARG2 = 24 - shift */
  x64_mov_r_r   (e, X64_SZ_32, X64_RCX, addr);
  x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, X64_RCX, 3);
  x64_shift_r_imm(e, X64_SH_SHL, X64_SZ_32, X64_RCX, 3);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG2, 24);
  x64_alu_r_r   (e, X64_ALU_SUB, X64_SZ_32, RWARG2, X64_RCX);

  if ((cpu_instruction_op_t)r->inst->any.op == CPU_OP_LWL) {
    /* mask = 0x00FFFFFF >> shift; value = (value & mask) | (RWRET << (24-shift)) */
    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, (s32)0x00FFFFFFu);
    x64_shift_r_cl (e, X64_SH_SHR, X64_SZ_32, RWARG3);
    x64_alu_r_r    (e, X64_ALU_AND, X64_SZ_32, value, RWARG3);
    x64_mov_r_r    (e, X64_SZ_32, X64_RCX, RWARG2);
    x64_shift_r_cl (e, X64_SH_SHL, X64_SZ_32, RWRET);
    x64_alu_r_r    (e, X64_ALU_OR,  X64_SZ_32, value, RWRET);
  } else { /* LWR */
    /* mask = 0xFFFFFF00 << (24-shift); value = (value & mask) | (RWRET >> shift) */
    x64_shift_r_cl (e, X64_SH_SHR, X64_SZ_32, RWRET);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, (s32)0xFFFFFF00u);
    x64_mov_r_r    (e, X64_SZ_32, X64_RCX, RWARG2);
    x64_shift_r_cl (e, X64_SH_SHL, X64_SZ_32, RWARG3);
    x64_alu_r_r    (e, X64_ALU_AND, X64_SZ_32, value, RWARG3);
    x64_alu_r_r    (e, X64_ALU_OR,  X64_SZ_32, value, RWRET);
  }

  cpu_recompiler_free_host_reg(r, addr_idx);
}

static void hook_compile_swx(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                             memory_access_size_t size, bool sign,
                             bool use_fastmem, const u32* known_addr)
{
  (void)size; (void)sign; (void)use_fastmem;
  x64_emit_t* e = current_emit(be_from(r));

  const u32 addr_idx = cpu_recompiler_allocate_temp_host_reg(r, CPU_RC_HR_CALLEE_SAVED);
  const x64_reg_t addr = (x64_reg_t)addr_idx;

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/true, /*use_fastmem=*/false);
  compute_loadstore_address_into(r, cf, known_addr, addr);

  /* Read existing word at aligned addr into RWRET. */
  x64_mov_r_r  (e, X64_SZ_32, RWARG1, addr);
  x64_alu_r_imm(e, X64_ALU_AND, X64_SZ_32, RWARG1, (s32)~0x3u);
  x64_call_abs64(e, (const void*)&cpu_recompiler_thunk_read_memory_word);

  /* CL = (addr & 3) * 8;  align addr in place for the writeback. */
  x64_mov_r_r   (e, X64_SZ_32, X64_RCX, addr);
  x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, X64_RCX, 3);
  x64_shift_r_imm(e, X64_SH_SHL, X64_SZ_32, X64_RCX, 3);
  x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, addr, (s32)~0x3u);

  /* Materialize rt into RWARG2 so the merge sequence can clobber CL. */
  emit_load_mips_to_reg(r, RWARG2, (cpu_reg_t)r->inst->r.rt);

  if ((cpu_instruction_op_t)r->inst->any.op == CPU_OP_SWL) {
    /* mem_mask = 0xFFFFFF00 << shift; new = (mem & mem_mask) | (rt >> (24-shift)) */
    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, (s32)0xFFFFFF00u);
    x64_shift_r_cl (e, X64_SH_SHL, X64_SZ_32, RWARG3);
    x64_alu_r_r    (e, X64_ALU_AND, X64_SZ_32, RWRET, RWARG3);

    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, 24);
    x64_alu_r_r    (e, X64_ALU_SUB, X64_SZ_32, RWARG3, X64_RCX);
    x64_mov_r_r    (e, X64_SZ_32, X64_RCX, RWARG3);
    x64_shift_r_cl (e, X64_SH_SHR, X64_SZ_32, RWARG2);
    x64_alu_r_r    (e, X64_ALU_OR,  X64_SZ_32, RWARG2, RWRET);
  } else { /* SWR */
    /* mem_mask = 0x00FFFFFF >> (24-shift); new = (mem & mem_mask) | (rt << shift) */
    x64_shift_r_cl (e, X64_SH_SHL, X64_SZ_32, RWARG2);

    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, 24);
    x64_alu_r_r    (e, X64_ALU_SUB, X64_SZ_32, RWARG3, X64_RCX);
    x64_mov_r_r    (e, X64_SZ_32, X64_RCX, RWARG3);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG3, (s32)0x00FFFFFFu);
    x64_shift_r_cl (e, X64_SH_SHR, X64_SZ_32, RWARG3);
    x64_alu_r_r    (e, X64_ALU_AND, X64_SZ_32, RWRET, RWARG3);
    x64_alu_r_r    (e, X64_ALU_OR,  X64_SZ_32, RWARG2, RWRET);
  }

  /* Slow-path word write of merged value. */
  x64_mov_r_r  (e, X64_SZ_32, RWARG1, addr);
  x64_call_abs64(e, (const void*)&cpu_recompiler_thunk_write_memory_word);

  cpu_recompiler_free_host_reg(r, addr_idx);
}

/* Inline `cpu_check_for_pending_interrupt`: if (sr.IEc && ((sr & cause) &
 * 0xFF00)) downcount = 0.  Forces the next end_block to fall through into
 * the event loop so a queued IRQ can dispatch promptly. */
static void emit_test_interrupts(cpu_recompiler_t* r)
{
  x64_emit_t* e = current_emit(be_from(r));
  x64_mov_r_m  (e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(cop0_regs.sr.bits)));
  x64_test_r_imm(e, X64_SZ_32, RWARG1, 1);
  u8* j_a = x64_jcc_rel32(e, X64_CC_E);
  x64_alu_r_m  (e, X64_ALU_AND, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(cop0_regs.cause.bits)));
  x64_test_r_imm(e, X64_SZ_32, RWARG1, 0xFF00);
  u8* j_b = x64_jcc_rel32(e, X64_CC_E);
  x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RSTATE, STATE_DISP(downcount)), 0);
  void* skip = x64_emit_cursor(e);
  x64_patch_rel32(j_a, skip);
  x64_patch_rel32(j_b, skip);
}

static void hook_compile_mtc0(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const cpu_cop0_reg_t reg = (cpu_cop0_reg_t)r->inst->r.rd;
  u32* const ptr = cpu_recompiler_get_cop0_reg_ptr(reg);
  const u32 mask = cpu_recompiler_get_cop0_reg_write_mask(reg);
  if (ptr == NULL) {
    if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
    return;
  }
  if (mask == 0u) {
    /* read-only register, ignore */
    return;
  }

  /* SR/CAUSE writes invoke C side-effects.  Flush caller-saved hosts now
   * (compile-time decision) so the C call sees consistent g_state.  Do
   * the flush BEFORE the read-modify-write so RWARG1/2/3 are free. */
  const bool needs_c_call = (reg == CPU_COP0_REG_SR);
  if (needs_c_call)
    cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);

  x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)ptr);

  if (cf.valid_host_t) {
    x64_mov_r_r   (e, X64_SZ_32, RWARG1, cf_get_reg_t(cf));
    x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, RWARG1, (s32)mask);
  } else if (cf.const_t) {
    const u32 cv = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf));
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)(cv & mask));
  } else {
    x64_mov_r_m  (e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_t(cf)));
    x64_alu_r_imm(e, X64_ALU_AND, X64_SZ_32, RWARG1, (s32)mask);
  }
  x64_mov_r_m  (e, X64_SZ_32, RWARG2, x64_mem(RXARG3, 0));

  /* For SR: stash old XOR new in RAX so we can test bit 16 (Isc) post-write
   * to decide whether to call cpu_update_memory_pointers.  RAX is a scratch
   * caller-saved that the allocator never picks. */
  if (reg == CPU_COP0_REG_SR) {
    x64_mov_r_r(e, X64_SZ_32, X64_RAX, RWARG2);
    x64_alu_r_r(e, X64_ALU_XOR, X64_SZ_32, X64_RAX, RWARG1);
  }

  x64_alu_r_imm(e, X64_ALU_AND, X64_SZ_32, RWARG2, (s32)~mask);
  x64_alu_r_r  (e, X64_ALU_OR,  X64_SZ_32, RWARG2, RWARG1);
  x64_mov_m_r  (e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG2);

  if (reg == CPU_COP0_REG_SR) {
    x64_test_r_imm(e, X64_SZ_32, X64_RAX, (s32)(1u << 16));
    u8* j_skip = x64_jcc_rel32(e, X64_CC_E);
    x64_call_abs64(e, (const void*)&cpu_update_memory_pointers);
    if (cpu_code_cache_is_using_fastmem())
      x64_mov_r_m(e, X64_SZ_64, RMEMBASE,
                  x64_mem(RSTATE, STATE_DISP(fastmem_base)));
    void* skip = x64_emit_cursor(e);
    x64_patch_rel32(j_skip, skip);
  }
  if (reg == CPU_COP0_REG_SR || reg == CPU_COP0_REG_CAUSE)
    emit_test_interrupts(r);
}

static void hook_compile_rfe(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  (void)cf;
  x64_emit_t* e = current_emit(be_from(r));
  /* Shift the SR mode bits right by two, preserving the upper bits. */
  static const u32 mode_bits_mask = 0xFu;
  x64_mov_r_m   (e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(cop0_regs.sr.bits)));
  x64_mov_r_r   (e, X64_SZ_32, RWARG2, RWARG1);
  x64_shift_r_imm(e, X64_SH_SHR, X64_SZ_32, RWARG2, 2);
  x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, RWARG1, (s32)~mode_bits_mask);
  x64_alu_r_imm (e, X64_ALU_AND, X64_SZ_32, RWARG2, (s32)mode_bits_mask);
  x64_alu_r_r   (e, X64_ALU_OR,  X64_SZ_32, RWARG1, RWARG2);
  x64_mov_m_r   (e, X64_SZ_32, x64_mem(RSTATE, STATE_DISP(cop0_regs.sr.bits)), RWARG1);
  emit_test_interrupts(r);
}

static void hook_compile_mfc2(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 index = cpu_cop2_index(*r->inst);
  const cpu_reg_t rt = (cpu_reg_t)r->inst->r.rt;

  cpu_recomp_gte_reg_lookup_t lk = cpu_recompiler_get_gte_register_pointer(index, /*writing=*/false);
  if (lk.action == CPU_RC_GTE_REG_IGNORE)
    return;

  u32 hreg;
  if (lk.action == CPU_RC_GTE_REG_DIRECT) {
    hreg = cpu_recompiler_allocate_host_reg(
      r, cpu_recompiler_get_flags_for_new_load_delayed_reg(r),
      CPU_RECOMP_EMULATE_LOAD_DELAYS ? CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG, rt);
    x64_mov_r_imm64(e, RXARG1, (u64)(uintptr_t)lk.ptr);
    x64_mov_r_m   (e, X64_SZ_32, (x64_reg_t)hreg, x64_mem(RXARG1, 0));
  } else if (lk.action == CPU_RC_GTE_REG_CALL_HANDLER) {
    cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)index);
    x64_call_abs64 (e, (const void*)&gte_read_register);
    hreg = cpu_recompiler_allocate_host_reg(
      r, cpu_recompiler_get_flags_for_new_load_delayed_reg(r),
      CPU_RECOMP_EMULATE_LOAD_DELAYS ? CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG, rt);
    x64_mov_r_r(e, X64_SZ_32, (x64_reg_t)hreg, RWRET);
  } else {
    Panic("Unhandled GTE read action");
  }
  (void)cf;
}

static void hook_compile_mtc2(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  x64_emit_t* e = current_emit(be_from(r));
  const u32 index = cpu_cop2_index(*r->inst);
  cpu_recomp_gte_reg_lookup_t lk = cpu_recompiler_get_gte_register_pointer(index, /*writing=*/true);
  if (lk.action == CPU_RC_GTE_REG_IGNORE)
    return;

  if (lk.action == CPU_RC_GTE_REG_DIRECT) {
    x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)lk.ptr);
    if (cf.const_t) {
      const u32 cv = cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf));
      x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RXARG3, 0), (s32)cv);
    } else if (cf.valid_host_t) {
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), cf_get_reg_t(cf));
    } else {
      x64_mov_r_m(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_t(cf)));
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
    }
  } else if (lk.action == CPU_RC_GTE_REG_SIGN_EXTEND_16 ||
             lk.action == CPU_RC_GTE_REG_ZERO_EXTEND_16) {
    const bool sign = (lk.action == CPU_RC_GTE_REG_SIGN_EXTEND_16);
    x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)lk.ptr);
    if (cf.const_t) {
      const u16 cv = (u16)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf));
      const u32 ext = sign ? (u32)(s32)(s16)cv : (u32)cv;
      x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RXARG3, 0), (s32)ext);
    } else if (cf.valid_host_t) {
      if (sign) x64_movsx_r_r16(e, X64_SZ_32, RWARG1, cf_get_reg_t(cf));
      else      x64_movzx_r_r16(e, X64_SZ_32, RWARG1, cf_get_reg_t(cf));
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
    } else {
      if (sign) x64_movsx_r_m16(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_t(cf)));
      else      x64_movzx_r_m16(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_t(cf)));
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
    }
  } else if (lk.action == CPU_RC_GTE_REG_CALL_HANDLER) {
    cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)index);
    /* RWARG2 = T value. */
    if (cf.const_t) {
      x64_mov_r_imm32(e, X64_SZ_32, RWARG2,
                      (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
    } else if (cf.valid_host_t) {
      if (cf_get_reg_t(cf) != RWARG2)
        x64_mov_r_r(e, X64_SZ_32, RWARG2, cf_get_reg_t(cf));
    } else {
      x64_mov_r_m(e, X64_SZ_32, RWARG2, mips_ptr(cpu_recomp_cf_mips_t(cf)));
    }
    x64_call_abs64(e, (const void*)&gte_write_register);
  } else if (lk.action == CPU_RC_GTE_REG_PUSH_FIFO) {
    cpu_recomp_gte_reg_lookup_t sxy1 = cpu_recompiler_get_gte_register_pointer(13u, true);
    cpu_recomp_gte_reg_lookup_t sxy2 = cpu_recompiler_get_gte_register_pointer(14u, true);
    cpu_recomp_gte_reg_lookup_t sxy0 = cpu_recompiler_get_gte_register_pointer(12u, true);
    x64_mov_r_imm64(e, RXARG1, (u64)(uintptr_t)sxy1.ptr);
    x64_mov_r_m   (e, X64_SZ_32, RWARG1, x64_mem(RXARG1, 0));
    x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)sxy2.ptr);
    x64_mov_r_m   (e, X64_SZ_32, RWARG2, x64_mem(RXARG2, 0));
    x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy0.ptr);
    x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
    x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy1.ptr);
    x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG2);
    x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy2.ptr);
    if (cf.const_t) {
      x64_mov_m_imm32(e, X64_SZ_32, x64_mem(RXARG3, 0),
                      (s32)cpu_recompiler_get_constant_reg_u32(r, cpu_recomp_cf_mips_t(cf)));
    } else if (cf.valid_host_t) {
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), cf_get_reg_t(cf));
    } else {
      x64_mov_r_m(e, X64_SZ_32, RWARG1, mips_ptr(cpu_recomp_cf_mips_t(cf)));
      x64_mov_m_r(e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
    }
  } else {
    Panic("Unhandled GTE write action");
  }
}

/* GTE op (cop2 with non-common encoding): stall+ticks already handled by
 * the template (CPU_RC_TF_GTE_STALL).  Just call gte_execute(inst.bits). */
static void hook_compile_cop2(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  (void)cf;
  x64_emit_t* e = current_emit(be_from(r));
  cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
  x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)r->inst->bits);
  x64_call_abs64 (e, (const void*)&gte_execute);
}

static void hook_compile_lwc2(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                              memory_access_size_t size, bool sign,
                              bool use_fastmem, const u32* known_addr)
{
  (void)size; (void)sign; (void)use_fastmem;
  x64_emit_t* e = current_emit(be_from(r));
  const u32 index = (u32)r->inst->r.rt;

  cpu_recomp_gte_reg_lookup_t lk = cpu_recompiler_get_gte_register_pointer(index, /*writing=*/true);

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/false, /*use_fastmem=*/false);

  const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);
  if (addr != RWARG1)
    x64_mov_r_r(e, X64_SZ_32, RWARG1, addr);

  x64_call_abs64(e, (const void*)&cpu_recompiler_thunk_read_memory_word);
  /* Loaded value now sits in RWRET. */

  switch (lk.action) {
    case CPU_RC_GTE_REG_IGNORE:
      break;

    case CPU_RC_GTE_REG_DIRECT:
      x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)lk.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWRET);
      break;

    case CPU_RC_GTE_REG_SIGN_EXTEND_16:
      x64_movsx_r_r16(e, X64_SZ_32, RWARG3, RWRET);
      x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)lk.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG2, 0), RWARG3);
      break;

    case CPU_RC_GTE_REG_ZERO_EXTEND_16:
      x64_movzx_r_r16(e, X64_SZ_32, RWARG3, RWRET);
      x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)lk.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG2, 0), RWARG3);
      break;

    case CPU_RC_GTE_REG_CALL_HANDLER:
      cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
      x64_mov_r_r    (e, X64_SZ_32, RWARG2, RWRET);
      x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)index);
      x64_call_abs64 (e, (const void*)&gte_write_register);
      break;

    case CPU_RC_GTE_REG_PUSH_FIFO: {
      /* SXY0 <- SXY1; SXY1 <- SXY2; SXY2 <- value (in RWRET). */
      cpu_recomp_gte_reg_lookup_t sxy0 = cpu_recompiler_get_gte_register_pointer(12u, true);
      cpu_recomp_gte_reg_lookup_t sxy1 = cpu_recompiler_get_gte_register_pointer(13u, true);
      cpu_recomp_gte_reg_lookup_t sxy2 = cpu_recompiler_get_gte_register_pointer(14u, true);
      x64_mov_r_imm64(e, RXARG1, (u64)(uintptr_t)sxy1.ptr);
      x64_mov_r_m   (e, X64_SZ_32, RWARG1, x64_mem(RXARG1, 0));
      x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)sxy2.ptr);
      x64_mov_r_m   (e, X64_SZ_32, RWARG2, x64_mem(RXARG2, 0));
      x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy0.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG1);
      x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy1.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWARG2);
      x64_mov_r_imm64(e, RXARG3, (u64)(uintptr_t)sxy2.ptr);
      x64_mov_m_r   (e, X64_SZ_32, x64_mem(RXARG3, 0), RWRET);
      break;
    }
  }
}

static void hook_compile_swc2(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                              memory_access_size_t size, bool sign,
                              bool use_fastmem, const u32* known_addr)
{
  (void)size; (void)sign; (void)use_fastmem;
  x64_emit_t* e = current_emit(be_from(r));
  const u32 index = (u32)r->inst->r.rt;

  cpu_recomp_gte_reg_lookup_t lk = cpu_recompiler_get_gte_register_pointer(index, /*writing=*/false);

  /* Fetch GTE reg value into RWARG2 (the eventual store-data argument).
   * Done BEFORE flush_for_load_store so the load sees coherent state. */
  if (lk.action == CPU_RC_GTE_REG_DIRECT) {
    x64_mov_r_imm64(e, RXARG2, (u64)(uintptr_t)lk.ptr);
    x64_mov_r_m   (e, X64_SZ_32, RWARG2, x64_mem(RXARG2, 0));
  } else if (lk.action == CPU_RC_GTE_REG_CALL_HANDLER) {
    cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL);
    x64_mov_r_imm32(e, X64_SZ_32, RWARG1, (s32)index);
    x64_call_abs64 (e, (const void*)&gte_read_register);
    x64_mov_r_r    (e, X64_SZ_32, RWARG2, RWRET);
  } else {
    if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
    return;
  }

  /* Save value across the addr-compute + thunk call by parking it in a
   * callee-saved temp.  Slow path call to thunk_write clobbers RWARG2. */
  const u32 data_idx = cpu_recompiler_allocate_temp_host_reg(r, CPU_RC_HR_CALLEE_SAVED);
  x64_mov_r_r(e, X64_SZ_32, (x64_reg_t)data_idx, RWARG2);

  cpu_recompiler_flush_for_load_store(r, known_addr, /*store=*/true, /*use_fastmem=*/false);

  const x64_reg_t addr = compute_loadstore_address(r, cf, known_addr);
  if (addr != RWARG1)
    x64_mov_r_r(e, X64_SZ_32, RWARG1, addr);
  x64_mov_r_r  (e, X64_SZ_32, RWARG2, (x64_reg_t)data_idx);
  x64_call_abs64(e, (const void*)&cpu_recompiler_thunk_write_memory_word);

  cpu_recompiler_free_host_reg(r, data_idx);
}

/* (Per-instruction emit_diff_trace_record removed; replaced by
 * per-block snapshot pairs in cpu_diff_block.{c,h}.  See hook_begin_block
 * and hook_end_block for the new dispatch points.) */

static u32 emit_asm_functions(void* code, u32 code_size)
{
  x64_emit_t e;
  x64_emit_init(&e, code, code_size);

  const s32 stack_size = 8;

  {
    void* const cur = x64_emit_cursor(&e);
    memcpy(&g_enter_recompiler, &cur, sizeof(cur));
  }

  x64_alu_r_imm  (&e, X64_ALU_SUB, X64_SZ_64, X64_RSP, stack_size);
  x64_mov_r_imm64(&e, RSTATE, (u64)(uintptr_t)&g_cpu_state);

  /* event_check (fall-through from prelude) */
  x64_mov_r_m(&e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(pending_ticks)));
  x64_alu_r_m(&e, X64_ALU_CMP, X64_SZ_32, RWARG1,
              x64_mem(RSTATE, STATE_DISP(downcount)));
  u8* const jl_to_dispatch = x64_jcc_rel32(&e, X64_CC_L);

  /* g_run_events_and_dispatch */
  g_run_events_and_dispatch = x64_emit_cursor(&e);
  x64_call_abs64(&e, (const void*)&timing_events_run_events);

  /* g_dispatcher */
  u8* const dispatch_addr = x64_emit_cursor(&e);
  g_dispatcher = (const void*)dispatch_addr;
  x64_mov_r_m    (&e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(pc)));
  x64_mov_r_imm64(&e, RXARG2, (u64)(uintptr_t)&g_code_lut[0]);
  x64_mov_r_r    (&e, X64_SZ_32, RWARG3, RWARG1);
  x64_shift_r_imm(&e, X64_SH_SHR, X64_SZ_32, RWARG3,
                  (u8)CPU_CODE_CACHE_LUT_TABLE_SHIFT);
  x64_mov_r_msib (&e, X64_SZ_64, RXARG2, x64_msib(RXARG2, RXARG3, 8, 0));
  x64_alu_r_imm  (&e, X64_ALU_AND, X64_SZ_32, RWARG1,
                  (s32)((CPU_CODE_CACHE_LUT_TABLE_SIZE - 1u) << 2u));
  x64_jmp_msib   (&e, x64_msib(RXARG2, RWARG1, 2, 0));

  x64_patch_rel32(jl_to_dispatch, dispatch_addr);

  /* g_compile_or_revalidate_block */
  g_compile_or_revalidate_block = x64_emit_cursor(&e);
  x64_mov_r_m   (&e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(pc)));
  x64_call_abs64(&e, (const void*)&cpu_code_cache_compile_or_revalidate_block);
  { u8* j = x64_jmp_rel32(&e); x64_patch_rel32(j, dispatch_addr); }

  /* g_discard_and_recompile_block */
  g_discard_and_recompile_block = x64_emit_cursor(&e);
  x64_mov_r_m   (&e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(pc)));
  x64_call_abs64(&e, (const void*)&cpu_code_cache_discard_and_recompile_block);
  { u8* j = x64_jmp_rel32(&e); x64_patch_rel32(j, dispatch_addr); }

  g_interpret_block = x64_emit_cursor(&e);
  x64_call_abs64(&e, (const void*)&cpu_recompiler_run_uncached_block);
  x64_mov_r_m  (&e, X64_SZ_32, RWARG1, x64_mem(RSTATE, STATE_DISP(pending_ticks)));
  x64_alu_r_m  (&e, X64_ALU_CMP, X64_SZ_32, RWARG1,
                x64_mem(RSTATE, STATE_DISP(downcount)));
  {
    u8* jge = x64_jcc_rel32(&e, X64_CC_GE);
    x64_patch_rel32(jge, g_run_events_and_dispatch);
    u8* j = x64_jmp_rel32(&e);
    x64_patch_rel32(j, dispatch_addr);
  }

  Assert(!e.overflow);
  return (u32)x64_emit_size(&e);
}

/* See x64_backend.h.  Emits a single `sub dword [RBP+pending_ticks], imm`
 * into `e`; used by the fastmem backpatch stub to undo the compile-time
 * RAM_READ_TICKS pre-charge before the runtime slow-path adds it again. */
void x64_backend_emit_sub_pending_ticks(x64_emit_t* e, s32 amount)
{
  if (amount == 0) return;
  x64_alu_m_imm(e, X64_ALU_SUB, X64_SZ_32,
                x64_mem(RSTATE, STATE_DISP(pending_ticks)), amount);
}

u32 x64_backend_emit_dispatcher_for_test(void* code, u32 code_size)
{
  return emit_asm_functions(code, code_size);
}

static const char* x64_get_host_reg_name(u32 reg)
{
  static const char* const names[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
  };
  return (reg < 16u) ? names[reg] : "??";
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
  .generate_block_protect_check    = hook_generate_block_protect_check,
  .generate_icache_check_and_update= hook_generate_icache_check_and_update,
  .generate_call                   = hook_generate_call,
  .generate_pgxp_call_with_mips_regs = hook_generate_pgxp_call_with_mips_regs, 

  /* Host-reg primitives. */
   .load_host_reg_with_constant   = hook_load_host_reg_with_constant,
  .load_host_reg_from_cpu_pointer= hook_load_host_reg_from_cpu_pointer,
  .store_host_reg_to_cpu_pointer = hook_store_host_reg_to_cpu_pointer,
  .store_constant_to_cpu_pointer = hook_store_constant_to_cpu_pointer,
  .copy_host_reg                 = hook_copy_host_reg,
  .flush                         = hook_flush, 

  .compile_fallback = hook_compile_fallback,

  /* Chunk 1: trivial ALU-imm. */
   .compile_andi = hook_compile_andi,
  .compile_ori  = hook_compile_ori,
  .compile_xori = hook_compile_xori, 

  /* Chunk 2: ALU + immediate. */
   .compile_addi  = hook_compile_addi,
  .compile_addiu = hook_compile_addiu,
  .compile_slti  = hook_compile_slti,
  .compile_sltiu = hook_compile_sltiu,
  .compile_add   = hook_compile_add,
  .compile_addu  = hook_compile_addu,
  .compile_sub   = hook_compile_sub,
  .compile_subu  = hook_compile_subu,
  .compile_and   = hook_compile_and,
  .compile_or    = hook_compile_or,
  .compile_xor   = hook_compile_xor,
  .compile_nor   = hook_compile_nor,
  .compile_slt   = hook_compile_slt,
  .compile_sltu  = hook_compile_sltu, 

  /* Chunk 3: shifts (LUI is base-layer). */
   .compile_sll   = hook_compile_sll,
  .compile_srl   = hook_compile_srl,
  .compile_sra   = hook_compile_sra,
  .compile_sllv  = hook_compile_sllv,
  .compile_srlv  = hook_compile_srlv,
  .compile_srav  = hook_compile_srav, 

  /* Chunk 5: branches + jumps (J/JAL handled by base layer). */
   .compile_jr    = hook_compile_jr,
  .compile_jalr  = hook_compile_jalr,
  .compile_bxx   = hook_compile_bxx, 

  /* Chunk 4: mul/div (HI/LO handled by base layer move-reg template). */
   .compile_mult  = hook_compile_mult,
  .compile_multu = hook_compile_multu,
  .compile_div   = hook_compile_div,
  .compile_divu  = hook_compile_divu, 

  /* Chunk 6: memory loads/stores (M1a closes lwx/swx; LWC2/SWC2 land in M1b). */
   .compile_lxx   = hook_compile_lxx,
  .compile_sxx   = hook_compile_sxx,
  .compile_lwx   = hook_compile_lwx,
  .compile_swx   = hook_compile_swx, 

  /* Chunk 7: COP0 (mfc0 handled by base layer).
   * Stale-Isc bug fixed: hook_compile_mtc0 reloads RMEMBASE post-
   * cpu_update_memory_pointers, so fastmem store sites emitted later in
   * the block see the right pointer.  Spec-layer cop0_sr remains
   * coherent because spec_exec_mtc0 / spec_exec_rfe update it. */
   .compile_mtc0  = hook_compile_mtc0,
  .compile_rfe   = hook_compile_rfe, 

  /* Chunk 8: COP2 / GTE.  M1b adds LWC2/SWC2 native codegen. */
   .compile_mfc2  = hook_compile_mfc2,
  .compile_mtc2  = hook_compile_mtc2,
  .compile_cop2  = hook_compile_cop2,
  .compile_lwc2  = hook_compile_lwc2,
  .compile_swc2  = hook_compile_swc2,

  /* Multi-cycle muldiv stall (partial: closes the steady-state
   * cached-block residual "un-modeled muldiv stall". Uncached
   * BIOS/KSEG1 blocks have a structural eager-prologue vs
   * lazy-per-inst-fetch mismatch with the interp replay that we don't
   * fix here, same as GTE-stall blocks today).  Producer/consumer logic
   * lives in cpu_recompiler.c (cpu_recompiler_stall_until_muldiv_complete
   * + set_muldiv_done_cycle_static).  Per-arch backends emit the FLUSH
   * codepaths via hook_flush below. */

  .get_host_reg_name = x64_get_host_reg_name,
};

void x64_backend_register(void)
{
  memset(&s_backend, 0, sizeof(s_backend));
  s_backend.base.v = &s_vtable;
  g_cpu_compiler = &s_backend.base;

  cpu_code_cache_set_emit_jump(&x64_emit_jump_at_site);
  cpu_code_cache_set_emit_asm_functions(&emit_asm_functions);

  INFO_LOG("x64 backend registered (lifecycle + ALU + shifts + branches + mul/div + lxx/sxx + cop0 + cop2)");
}

void x64_backend_shutdown(void)
{
  cpu_code_cache_set_emit_jump(NULL);
  cpu_code_cache_set_emit_asm_functions(NULL);
  g_cpu_compiler = NULL;
  memset(&s_backend, 0, sizeof(s_backend));
}
