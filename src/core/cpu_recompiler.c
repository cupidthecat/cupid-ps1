/*
 * Recompiler base - arch-agnostic helpers.  Provides:
 *
 *   ✓ reset_state                         (per-block init)
 *   ✓ has_constant_reg / set_constant_reg / clear / flush
 *   ✓ get_cop0_reg_ptr / write_mask
 *   ✓ mips_signed/unsigned_divide         (called from compiled blocks)
 *   ✓ Register allocator                  - get_free / allocate / check / swap /
 *                                           flush / free / clear / mark / rename /
 *                                           delete_mips_reg / try_rename / counters
 *   ✓ Composite Flush(u32 flags)
 *   ✓ Load-delay engine                   - has / cancel / update / finish /
 *                                           finish_to_reg / get_flags_for_new
 *   ✓ Host-state backup / restore         (for delay-slot rollback)
 *   ✓ Speculative-constant infrastructure - init / invalidate / read/write_reg /
 *                                           copy_reg / read/write_mem /
 *                                           invalidate_mem (open-addressed hashmap)
 *   ✓ spec_is_cache_isolated
 *
 * Allocator hooks (load_host_reg_with_constant, load_host_reg_from_cpu_pointer,
 * store_host_reg_to_cpu_pointer, store_constant_to_cpu_pointer, copy_host_reg,
 * flush) call into r->v->* - the backend vtable.  Until 6.D registers a backend,
 * these are NULL function pointers; cpu_code_cache_execute() guards against
 * dispatching to a NULL backend, so this code is unreachable until 6.D wires it.
 *
 * The per-MIPS-op compile_*_const helpers, compile_template,
 * compile_loadstore_template, and compile_instruction switch land in 6.C3.
 */

#include "cpu_recompiler.h"

#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_diff_block.h"
#include "cpu_diff_runner.h"
#include "cpu_pgxp.h"
#include "settings.h"

#include "common/assert.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Recompiler);

cpu_recompiler_t* g_cpu_compiler = NULL;

static void spec_mem_grow(cpu_recompiler_t* r);
static u32  fnv1a_u32(u32 x);

void cpu_recompiler_reset_state(cpu_recompiler_t* r,
                                cpu_code_cache_block_t* block,
                                u8* code_buffer, u32 code_buffer_space,
                                u8* far_code_buffer, u32 far_code_space)
{
  (void)code_buffer;
  (void)code_buffer_space;
  (void)far_code_buffer;
  (void)far_code_space;

  r->block = block;
  r->compiler_pc = (block != NULL) ? block->pc : 0u;
  r->cycles = 0;
  r->gte_done_cycle = 0;
  r->muldiv_done_cycle = 0;
  r->inst = NULL;
  r->iinfo = NULL;
  r->current_instruction_pc = 0;
  r->current_instruction_branch_delay_slot = false;
  r->branch_delay_slot_swapped = false;
  r->delay_slot_branch_was_taken = false;
  r->dirty_pc = false;
  r->dirty_instruction_bits = false;
  r->dirty_gte_done_cycle = true;
  /* dirty_muldiv_done_cycle starts true at block reset: a previous block
   * may have written muldiv_completion_tick to state, and the consumer in
   * this block needs to load from state to see it.  After the first
   * MULT/MULTU/DIV/DIVU in this block sets a fresh value, the producer
   * either keeps it dirty (state-only, MULT/MULTU non-const-rs) or marks
   * it clean (in-block-schedulable, DIV/DIVU). */
  r->dirty_muldiv_done_cycle = true;
  r->block_ended = false;

  r->constant_regs_valid = 0u;
  r->constant_regs_dirty = 0u;
  memset(r->constant_reg_values, 0, sizeof(r->constant_reg_values));
  r->constant_regs_valid = 1u; /* Reg::zero (idx 0) is constant 0 */
  r->constant_reg_values[0] = 0u;

  /* Wipe host reg allocations.  USABLE / CALLEE_SAVED bits stay because
   * they're per-arch immutable; backend Reset() repopulates them. */
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    ra->flags &= (u8)CPU_RC_HR_IMMUTABLE_FLAGS;
    ra->type = CPU_RC_HRT_TEMP;
    ra->reg = (cpu_reg_t)CPU_REG_COUNT;
    ra->counter = 0;
  }
  r->register_alloc_counter = 0;

  r->load_delay_dirty = (CPU_RECOMP_EMULATE_LOAD_DELAYS != 0);
  r->load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
  r->load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
  r->next_load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
  r->next_load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;

  r->host_state_backup_count = 0u;

  cpu_recompiler_init_speculative_regs(r);
}

bool cpu_recompiler_has_constant_reg(const cpu_recompiler_t* r, cpu_reg_t reg)
{
  DebugAssert((u32)reg < CPU_REG_COUNT);
  return ((r->constant_regs_valid >> (u32)reg) & 1u) != 0u;
}

bool cpu_recompiler_has_dirty_constant_reg(const cpu_recompiler_t* r, cpu_reg_t reg)
{
  DebugAssert((u32)reg < CPU_REG_COUNT);
  return ((r->constant_regs_dirty >> (u32)reg) & 1u) != 0u;
}

bool cpu_recompiler_has_constant_reg_value(const cpu_recompiler_t* r,
                                           cpu_reg_t reg, u32 v)
{
  return cpu_recompiler_has_constant_reg(r, reg) &&
         r->constant_reg_values[(u32)reg] == v;
}

u32 cpu_recompiler_get_constant_reg_u32(const cpu_recompiler_t* r, cpu_reg_t reg)
{
  DebugAssert((u32)reg < CPU_REG_COUNT);
  return r->constant_reg_values[(u32)reg];
}

s32 cpu_recompiler_get_constant_reg_s32(const cpu_recompiler_t* r, cpu_reg_t reg)
{
  return (s32)cpu_recompiler_get_constant_reg_u32(r, reg);
}

void cpu_recompiler_set_constant_reg(cpu_recompiler_t* r, cpu_reg_t reg, u32 v)
{
  DebugAssert((u32)reg < CPU_REG_COUNT && (u32)reg != 0);

  /* Cancel any incoming load delay to this reg first. */
  cpu_recompiler_cancel_load_delays_to_reg(r, reg);

  if (cpu_recompiler_has_constant_reg_value(r, reg, v)) {
    /* Shouldn't be any host regs caching it. */
    DebugAssert(!cpu_recompiler_check_host_reg(r, 0u, CPU_RC_HRT_CPU_REG, reg).has);
    return;
  }

  r->constant_reg_values[(u32)reg] = v;
  r->constant_regs_valid |= ((u64)1u << (u32)reg);
  r->constant_regs_dirty |= ((u64)1u << (u32)reg);

  /* Discard any host reg currently caching this guest reg. */
  cpu_recomp_opt_u32_t hr = cpu_recompiler_check_host_reg(r, 0u, CPU_RC_HRT_CPU_REG, reg);
  if (hr.has)
    cpu_recompiler_free_host_reg(r, hr.val);
}

void cpu_recompiler_clear_constant_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  DebugAssert((u32)reg < CPU_REG_COUNT && (u32)reg != 0);
  r->constant_regs_valid &= ~((u64)1u << (u32)reg);
  r->constant_regs_dirty &= ~((u64)1u << (u32)reg);
  r->constant_reg_values[(u32)reg] = 0u;
}

void cpu_recompiler_flush_constant_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  DebugAssert(cpu_recompiler_has_constant_reg(r, reg));
  if (r->v && r->v->store_constant_to_cpu_pointer) {
     r->v->store_constant_to_cpu_pointer(r,
                                        r->constant_reg_values[(u32)reg], 
                                        &g_cpu_state.regs.r[(u32)reg]);
  }
  r->constant_regs_dirty &= ~((u64)1u << (u32)reg);
}

void cpu_recompiler_flush_constant_regs(cpu_recompiler_t* r, bool invalidate)
{
  for (u32 i = 1; i < CPU_REG_COUNT; i++) {
    if (cpu_recompiler_has_dirty_constant_reg(r, (cpu_reg_t)i))
      cpu_recompiler_flush_constant_reg(r, (cpu_reg_t)i);
    if (invalidate)
      cpu_recompiler_clear_constant_reg(r, (cpu_reg_t)i);
  }
}

static u32* s_cop0_ptrs[16];
static const u32 s_cop0_masks[16] = {
  0u, 0u, 0u,
  0xFFFFFFFFu,                   /* 3 BPC */
  0u,
  0xFFFFFFFFu,                   /* 5 BDA */
  0u,                             /* 6 TAR (read-only on PSX) */
  CPU_COP0_DCIC_WRITE_MASK,      /* 7 DCIC */
  0u,                             /* 8 BadVaddr */
  0xFFFFFFFFu,                   /* 9 BDAM */
  0u,
  0xFFFFFFFFu,                   /* 11 BPCM */
  CPU_COP0_SR_WRITE_MASK,        /* 12 SR */
  CPU_COP0_CAUSE_WRITE_MASK,     /* 13 CAUSE */
  0u,                             /* 14 EPC (written by exception logic only) */
  0u,                             /* 15 PRID (read-only) */
};

static void init_cop0_table_once(void)
{
  static bool initialised = false;
  if (initialised) return;
  initialised = true;

  s_cop0_ptrs[3]  = &g_cpu_state.cop0_regs.BPC;
  s_cop0_ptrs[5]  = &g_cpu_state.cop0_regs.BDA;
  s_cop0_ptrs[6]  = &g_cpu_state.cop0_regs.TAR;
  s_cop0_ptrs[7]  = &g_cpu_state.cop0_regs.dcic.bits;
  s_cop0_ptrs[8]  = &g_cpu_state.cop0_regs.BadVaddr;
  s_cop0_ptrs[9]  = &g_cpu_state.cop0_regs.BDAM;
  s_cop0_ptrs[11] = &g_cpu_state.cop0_regs.BPCM;
  s_cop0_ptrs[12] = &g_cpu_state.cop0_regs.sr.bits;
  s_cop0_ptrs[13] = &g_cpu_state.cop0_regs.cause.bits;
  s_cop0_ptrs[14] = &g_cpu_state.cop0_regs.EPC;
  s_cop0_ptrs[15] = &g_cpu_state.cop0_regs.PRID;
}

u32* cpu_recompiler_get_cop0_reg_ptr(cpu_cop0_reg_t reg)
{
  init_cop0_table_once();
  const u32 idx = (u32)reg;
  return (idx < 16u) ? s_cop0_ptrs[idx] : NULL;
}

u32 cpu_recompiler_get_cop0_reg_write_mask(cpu_cop0_reg_t reg)
{
  const u32 idx = (u32)reg;
  return (idx < 16u) ? s_cop0_masks[idx] : 0u;
}

void cpu_recompiler_mips_signed_divide(s32 num, s32 denom, u32* lo, u32* hi)
{
  if (denom == 0) {
    *lo = (num >= 0) ? 0xFFFFFFFFu : 1u;
    *hi = (u32)num;
    return;
  }
  if (num == (s32)0x80000000 && denom == -1) {
    *lo = 0x80000000u;
    *hi = 0u;
    return;
  }
  *lo = (u32)(num / denom);
  *hi = (u32)(num % denom);
}

void cpu_recompiler_mips_unsigned_divide(u32 num, u32 denom, u32* lo, u32* hi)
{
  if (denom == 0u) {
    *lo = 0xFFFFFFFFu;
    *hi = num;
    return;
  }
  *lo = num / denom;
  *hi = num % denom;
}

bool cpu_recompiler_is_host_reg_allocated(const cpu_recompiler_t* r, u32 hr)
{
  return (r->host_regs[hr].flags & CPU_RC_HR_ALLOCATED) != 0u;
}

u32 cpu_recompiler_get_free_host_reg(cpu_recompiler_t* r, u32 flags)
{
  const u32 req_flags = CPU_RC_HR_USABLE | (flags & CPU_RC_HR_CALLEE_SAVED);

  u32 fallback = CPU_RECOMP_NUM_HOST_REGS;
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    if ((r->host_regs[i].flags & (req_flags | CPU_RC_HR_NEEDED | CPU_RC_HR_ALLOCATED)) == req_flags) {
      if (r->host_regs[i].flags & CPU_RC_HR_CALLEE_SAVED)
        return i;
      else if (fallback == CPU_RECOMP_NUM_HOST_REGS)
        fallback = i;
    }
  }
  if (fallback != CPU_RECOMP_NUM_HOST_REGS)
    return fallback;

  /* Find the allocated reg with the lowest counter (longest unused). */
  u32 lowest = CPU_RECOMP_NUM_HOST_REGS;
  u32 lowest_count = 0xFFFFFFFFu;
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    const cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if ((ra->flags & (req_flags | CPU_RC_HR_NEEDED)) != req_flags)
      continue;

    DebugAssert(ra->flags & CPU_RC_HR_ALLOCATED);
    if (ra->type == CPU_RC_HRT_TEMP)
      continue;            /* can't punt temps */

    if (ra->counter < lowest_count) {
      lowest = i;
      lowest_count = ra->counter;
    }
  }

  AssertMsg(lowest != CPU_RECOMP_NUM_HOST_REGS, "Register allocation failed.");

  const cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[lowest];
  if (ra->type == CPU_RC_HRT_CPU_REG &&
      r->iinfo != NULL &&
      cpu_code_cache_inst_used_test(r->iinfo, ra->reg) &&
      (flags & CPU_RC_HR_CALLEE_SAVED)) {
    /* Try to relocate the callee-saved register to a caller-saved slot. */
    u32 cs_lowest = CPU_RECOMP_NUM_HOST_REGS;
    u32 cs_lowest_count = 0xFFFFFFFFu;
    for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
      const u32 caller_req_flags = CPU_RC_HR_USABLE;
      const u32 caller_req_mask  = CPU_RC_HR_USABLE | CPU_RC_HR_NEEDED | CPU_RC_HR_CALLEE_SAVED;
      const cpu_recomp_host_reg_alloc_t* cra = &r->host_regs[i];
      if ((cra->flags & caller_req_mask) != caller_req_flags)
        continue;

      if (!(cra->flags & CPU_RC_HR_ALLOCATED)) {
        cs_lowest = i;
        cs_lowest_count = 0;
        break;
      }
      if (cra->type == CPU_RC_HRT_TEMP)
        continue;
      if (cra->counter < cs_lowest_count) {
        cs_lowest = i;
        cs_lowest_count = cra->counter;
      }
    }

    if (cs_lowest_count < lowest_count) {
      if (cpu_recompiler_is_host_reg_allocated(r, cs_lowest))
        cpu_recompiler_free_host_reg(r, cs_lowest);
      if (r->v && r->v->copy_host_reg)
        r->v->copy_host_reg(r, cs_lowest, lowest);
      cpu_recompiler_swap_host_reg_alloc(r, cs_lowest, lowest);
      DebugAssert(!cpu_recompiler_is_host_reg_allocated(r, lowest));
      return lowest;
    }
  }

  cpu_recompiler_free_host_reg(r, lowest);
  return lowest;
}

 u32 cpu_recompiler_allocate_host_reg(cpu_recompiler_t* r, u32 flags,
                                     cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg) 
{
  if ((flags & CPU_RC_HR_MODE_WRITE) &&
      (type == CPU_RC_HRT_CPU_REG || type == CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE)) {
    cpu_recompiler_cancel_load_delays_to_reg(r, reg);
  }

  if (type != CPU_RC_HRT_TEMP) {
    cpu_recomp_opt_u32_t check = cpu_recompiler_check_host_reg(r, flags, type, reg);
    DebugAssert((type != CPU_RC_HRT_LOAD_DELAY_VALUE && type != CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE) || !check.has);
    if (check.has)
      return check.val;
  }

  const u32 hreg = cpu_recompiler_get_free_host_reg(r, flags);
  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[hreg];
   ra->flags = (u8)((ra->flags & CPU_RC_HR_IMMUTABLE_FLAGS) |
                   (flags & CPU_RC_HR_ALLOWED_FLAGS) | 
                   CPU_RC_HR_ALLOCATED | CPU_RC_HR_NEEDED);
  ra->type = type;
  ra->reg = reg;
  ra->counter = r->register_alloc_counter++;

  switch (type) {
    case CPU_RC_HRT_CPU_REG: {
      DebugAssert(reg != (cpu_reg_t)0);
      if (flags & CPU_RC_HR_MODE_READ) {
        DebugAssert((u32)reg > 0u && (u32)reg < CPU_REG_COUNT);
        if (cpu_recompiler_has_constant_reg(r, reg)) {
          if (r->v && r->v->load_host_reg_with_constant)
            r->v->load_host_reg_with_constant(r, hreg,
                                              cpu_recompiler_get_constant_reg_u32(r, reg));
          r->constant_regs_dirty &= ~((u64)1u << (u32)reg);
          ra->flags |= CPU_RC_HR_MODE_WRITE;
        } else {
          if (r->v && r->v->load_host_reg_from_cpu_pointer)
            r->v->load_host_reg_from_cpu_pointer(r, hreg, &g_cpu_state.regs.r[(u32)reg]);
        }
      }
      if ((flags & CPU_RC_HR_MODE_WRITE) && cpu_recompiler_has_constant_reg(r, reg)) {
        DebugAssert(reg != (cpu_reg_t)0);
        cpu_recompiler_clear_constant_reg(r, reg);
      }
    } break;

    case CPU_RC_HRT_LOAD_DELAY_VALUE: {
      DebugAssert(!r->load_delay_dirty &&
                  (!cpu_recompiler_has_load_delay(r) || !(flags & CPU_RC_HR_MODE_WRITE)));
      r->load_delay_register = reg;
      r->load_delay_value_register = hreg;
      if ((flags & CPU_RC_HR_MODE_READ) && r->v && r->v->load_host_reg_from_cpu_pointer)
        r->v->load_host_reg_from_cpu_pointer(r, hreg, &g_cpu_state.load_delay_value);
    } break;

    case CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE: {
      r->next_load_delay_register = reg;
      r->next_load_delay_value_register = hreg;
      if ((flags & CPU_RC_HR_MODE_READ) && r->v && r->v->load_host_reg_from_cpu_pointer)
        r->v->load_host_reg_from_cpu_pointer(r, hreg, &g_cpu_state.next_load_delay_value);
    } break;

    case CPU_RC_HRT_TEMP: {
      DebugAssert(!(flags & (CPU_RC_HR_MODE_READ | CPU_RC_HR_MODE_WRITE)));
    } break;

    default:
      Panic("Unknown host-reg type");
      break;
  }

  return hreg;
}

cpu_recomp_opt_u32_t
cpu_recompiler_check_host_reg(cpu_recompiler_t* r, u32 flags,
                              cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg)
{
  cpu_recomp_opt_u32_t result = {.has = false, .val = 0u};

  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if (!(ra->flags & CPU_RC_HR_ALLOCATED) || ra->type != type || ra->reg != reg)
      continue;

    DebugAssert(ra->flags & CPU_RC_HR_MODE_READ);
    if (flags & CPU_RC_HR_MODE_WRITE) {
      DebugAssert(type == CPU_RC_HRT_CPU_REG);
      if (cpu_recompiler_has_constant_reg(r, reg)) {
        DebugAssert(reg != (cpu_reg_t)0);
        cpu_recompiler_clear_constant_reg(r, reg);
      }
    }

    ra->flags |= (u8)((flags & CPU_RC_HR_ALLOWED_FLAGS) | CPU_RC_HR_NEEDED);
    ra->counter = r->register_alloc_counter++;

    /* Promote to callee-saved by relocating to a callee-saved register. */
    if ((flags & CPU_RC_HR_CALLEE_SAVED) && !(ra->flags & CPU_RC_HR_CALLEE_SAVED)) {
      const u32 new_reg = cpu_recompiler_get_free_host_reg(r, CPU_RC_HR_CALLEE_SAVED);
      if (r->v && r->v->copy_host_reg)
        r->v->copy_host_reg(r, new_reg, i);
      cpu_recompiler_swap_host_reg_alloc(r, i, new_reg);
      DebugAssert(!cpu_recompiler_is_host_reg_allocated(r, i));
      result.has = true;
      result.val = new_reg;
      return result;
    }

    result.has = true;
    result.val = i;
    return result;
  }
  return result;
}

u32 cpu_recompiler_allocate_temp_host_reg(cpu_recompiler_t* r, u32 flags)
{
  return cpu_recompiler_allocate_host_reg(r, flags, CPU_RC_HRT_TEMP, (cpu_reg_t)CPU_REG_COUNT);
}

void cpu_recompiler_swap_host_reg_alloc(cpu_recompiler_t* r, u32 lhs, u32 rhs)
{
  cpu_recomp_host_reg_alloc_t* lra = &r->host_regs[lhs];
  cpu_recomp_host_reg_alloc_t* rra = &r->host_regs[rhs];

  const u8 lra_flags = lra->flags;
  lra->flags = (u8)((lra->flags & CPU_RC_HR_IMMUTABLE_FLAGS) |
                    (rra->flags & ~CPU_RC_HR_IMMUTABLE_FLAGS));
  rra->flags = (u8)((rra->flags & CPU_RC_HR_IMMUTABLE_FLAGS) |
                    (lra_flags  & ~CPU_RC_HR_IMMUTABLE_FLAGS));

  cpu_recomp_host_reg_alloc_type_t tmp_type = lra->type;
  lra->type = rra->type; rra->type = tmp_type;

  cpu_reg_t tmp_reg = lra->reg;
  lra->reg = rra->reg;   rra->reg = tmp_reg;

  u16 tmp_counter = lra->counter;
  lra->counter = rra->counter; rra->counter = tmp_counter;
}

void cpu_recompiler_flush_host_reg(cpu_recompiler_t* r, u32 reg)
{
  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[reg];
  if (ra->flags & CPU_RC_HR_MODE_WRITE) {
    switch (ra->type) {
      case CPU_RC_HRT_CPU_REG: {
        DebugAssert((u32)ra->reg > 0u && (u32)ra->reg < CPU_REG_COUNT);
        if (r->v && r->v->store_host_reg_to_cpu_pointer)
          r->v->store_host_reg_to_cpu_pointer(r, reg, &g_cpu_state.regs.r[(u32)ra->reg]);
      } break;

      case CPU_RC_HRT_LOAD_DELAY_VALUE: {
        DebugAssert(r->load_delay_value_register == reg);
        if (r->v && r->v->store_host_reg_to_cpu_pointer)
          r->v->store_host_reg_to_cpu_pointer(r, reg, &g_cpu_state.load_delay_value);
        r->load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
      } break;

      case CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE: {
        DebugAssert(r->next_load_delay_value_register == reg);
        WARNING_LOG("Flushing NEXT load delayed register to state");
        if (r->v && r->v->store_host_reg_to_cpu_pointer)
          r->v->store_host_reg_to_cpu_pointer(r, reg, &g_cpu_state.next_load_delay_value);
        r->next_load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
      } break;

      default:
        break;
    }
    ra->flags = (u8)((ra->flags & ~CPU_RC_HR_MODE_WRITE) | CPU_RC_HR_MODE_READ);
  }
}

void cpu_recompiler_free_host_reg(cpu_recompiler_t* r, u32 reg)
{
  DebugAssert(cpu_recompiler_is_host_reg_allocated(r, reg));
  cpu_recompiler_flush_host_reg(r, reg);
  cpu_recompiler_clear_host_reg(r, reg);
}

void cpu_recompiler_clear_host_reg(cpu_recompiler_t* r, u32 reg)
{
  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[reg];
  ra->flags &= (u8)CPU_RC_HR_IMMUTABLE_FLAGS;
  ra->type = CPU_RC_HRT_TEMP;
  ra->counter = 0;
  ra->reg = (cpu_reg_t)CPU_REG_COUNT;
}

void cpu_recompiler_mark_regs_needed(cpu_recompiler_t* r,
                                     cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg)
{
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if ((ra->flags & CPU_RC_HR_ALLOCATED) && ra->type == type && ra->reg == reg)
      ra->flags |= CPU_RC_HR_NEEDED;
  }
}

void cpu_recompiler_rename_host_reg(cpu_recompiler_t* r, u32 reg, u32 new_flags,
                                    cpu_recomp_host_reg_alloc_type_t new_type, cpu_reg_t new_reg)
{
  /* only supported for cpu regs / temps / next-load-delay slot */
  DebugAssert(new_type == CPU_RC_HRT_TEMP || new_type == CPU_RC_HRT_CPU_REG ||
              new_type == CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE);

  cpu_recomp_opt_u32_t old_reg = cpu_recompiler_check_host_reg(r, 0u, new_type, new_reg);
  if (old_reg.has)
    cpu_recompiler_clear_host_reg(r, old_reg.val);   /* don't writeback */

  if (new_type == CPU_RC_HRT_CPU_REG || new_type == CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE)
    cpu_recompiler_cancel_load_delays_to_reg(r, new_reg);

  if (new_type == CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE) {
    DebugAssert(r->next_load_delay_register == (cpu_reg_t)CPU_REG_COUNT &&
                r->next_load_delay_value_register == CPU_RECOMP_NUM_HOST_REGS);
    r->next_load_delay_register = new_reg;
    r->next_load_delay_value_register = reg;
  }

  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[reg];
   ra->flags = (u8)((ra->flags & CPU_RC_HR_IMMUTABLE_FLAGS) |
                   CPU_RC_HR_NEEDED | CPU_RC_HR_ALLOCATED | 
                   (new_flags & CPU_RC_HR_ALLOWED_FLAGS));
  ra->counter = r->register_alloc_counter++;
  ra->type = new_type;
  ra->reg = new_reg;
}

void cpu_recompiler_clear_host_reg_needed(cpu_recompiler_t* r, u32 reg)
{
  DebugAssert(reg < CPU_RECOMP_NUM_HOST_REGS && cpu_recompiler_is_host_reg_allocated(r, reg));
  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[reg];
  if (ra->flags & CPU_RC_HR_MODE_WRITE)
    ra->flags |= CPU_RC_HR_MODE_READ;
  ra->flags &= (u8)~CPU_RC_HR_NEEDED;
}

void cpu_recompiler_clear_host_regs_needed(cpu_recompiler_t* r)
{
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if (!(ra->flags & CPU_RC_HR_ALLOCATED))
      continue;
    DebugAssert(ra->type != CPU_RC_HRT_TEMP);
    if (ra->flags & CPU_RC_HR_MODE_WRITE)
      ra->flags |= CPU_RC_HR_MODE_READ;
    ra->flags &= (u8)~CPU_RC_HR_NEEDED;
  }
}

void cpu_recompiler_delete_mips_reg(cpu_recompiler_t* r, cpu_reg_t reg, bool flush)
{
  DebugAssert(reg != (cpu_reg_t)0);

  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if ((ra->flags & CPU_RC_HR_ALLOCATED) && ra->type == CPU_RC_HRT_CPU_REG && ra->reg == reg) {
      if (flush)
        cpu_recompiler_flush_host_reg(r, i);
      cpu_recompiler_clear_host_reg(r, i);
      cpu_recompiler_clear_constant_reg(r, reg);
      return;
    }
  }

  if (flush && cpu_recompiler_has_dirty_constant_reg(r, reg))
    cpu_recompiler_flush_constant_reg(r, reg);
  cpu_recompiler_clear_constant_reg(r, reg);
}

bool cpu_recompiler_try_rename_mips_reg(cpu_recompiler_t* r, cpu_reg_t to, cpu_reg_t from,
                                        u32 fromhost, cpu_reg_t other)
{
  /* can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt */
  if (to == from || to == other ||
      r->iinfo == NULL || !cpu_code_cache_inst_rename_test(r->iinfo, from))
    return false;

  if (cpu_code_cache_inst_live_test(r->iinfo, from))
    cpu_recompiler_flush_host_reg(r, fromhost);

  /* remove all references to renamed-to register */
  cpu_recompiler_delete_mips_reg(r, to, false);
  cpu_recompiler_cancel_load_delays_to_reg(r, to);

  r->host_regs[fromhost].reg = to;
  r->host_regs[fromhost].flags |= (CPU_RC_HR_MODE_READ | CPU_RC_HR_MODE_WRITE);
  return true;
}

void cpu_recompiler_update_host_reg_counters(cpu_recompiler_t* r)
{
  if (r->block == NULL || r->iinfo == NULL)
    return;

  const cpu_code_cache_inst_info_t* info_end =
      cpu_code_cache_block_instructions_info(r->block) + r->block->size;

  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
    if ((ra->flags & (CPU_RC_HR_ALLOCATED | CPU_RC_HR_NEEDED)) != CPU_RC_HR_ALLOCATED)
      continue;

    /* Try not to punt out load delays. */
    if (ra->type != CPU_RC_HRT_CPU_REG) {
      ra->counter = 0xFFFFu;
      continue;
    }

    DebugAssert(cpu_recompiler_is_host_reg_allocated(r, i));
    const cpu_code_cache_inst_info_t* cur = r->iinfo;
    const cpu_reg_t reg = ra->reg;

    if (!(cur->reg_flags[(u32)reg] & CPU_CODE_CACHE_RI_USED)) {
      ra->counter = 0;
      continue;
    }

    /* count instructions until next read of this reg */
    u16 counter_val = 0xFFFFu;
    for (; cur != info_end; cur++, counter_val--) {
      if (cpu_code_cache_inst_reads_reg(cur, reg))
        break;
    }
    ra->counter = counter_val;
  }
}

void cpu_recompiler_flush(cpu_recompiler_t* r, u32 flags)
{
  if (flags & (CPU_RC_FLUSH_FREE_UNNEEDED_CALLER_SAVED_REGS |
               CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS |
               CPU_RC_FLUSH_FREE_ALL_REGISTERS)) {
    const u32 req_mask = (flags & CPU_RC_FLUSH_FREE_ALL_REGISTERS) ?
                           CPU_RC_HR_ALLOCATED :
                           ((flags & CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS) ?
                              (CPU_RC_HR_ALLOCATED | CPU_RC_HR_CALLEE_SAVED) : 
                              (CPU_RC_HR_ALLOCATED | CPU_RC_HR_CALLEE_SAVED | CPU_RC_HR_NEEDED));
    const u32 req_flags = CPU_RC_HR_ALLOCATED;

    for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
      cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
      if (((u32)ra->flags & req_mask) == req_flags)
        cpu_recompiler_free_host_reg(r, i);
    }
  }

  if (flags & CPU_RC_FLUSH_INVALIDATE_MIPS_REGISTERS) {
    for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
      cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
      if ((ra->flags & CPU_RC_HR_ALLOCATED) && ra->type == CPU_RC_HRT_CPU_REG)
        cpu_recompiler_free_host_reg(r, i);
    }
    cpu_recompiler_flush_constant_regs(r, true);
  } else if (flags & CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS) {
    for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
      cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
      if (((u32)ra->flags & (CPU_RC_HR_ALLOCATED | CPU_RC_HR_MODE_WRITE)) ==
              (CPU_RC_HR_ALLOCATED | CPU_RC_HR_MODE_WRITE) &&
          ra->type == CPU_RC_HRT_CPU_REG) 
        cpu_recompiler_flush_host_reg(r, i);
    }
    cpu_recompiler_flush_constant_regs(r, false);
  }

  if (flags & CPU_RC_FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS)
    cpu_recompiler_invalidate_speculative_values(r);

  if (r->v && r->v->flush)
    r->v->flush(r, flags);
}

bool cpu_recompiler_has_load_delay(const cpu_recompiler_t* r)
{
  return r->load_delay_register != (cpu_reg_t)CPU_REG_COUNT;
}

void cpu_recompiler_cancel_load_delays_to_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  if (r->load_delay_register != reg)
    return;
  r->load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
  if (r->load_delay_value_register != CPU_RECOMP_NUM_HOST_REGS) {
    cpu_recompiler_clear_host_reg(r, r->load_delay_value_register);
    r->load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
  }
}

void cpu_recompiler_update_load_delay(cpu_recompiler_t* r)
{
  if (r->load_delay_dirty) {
    DebugAssert(!cpu_recompiler_has_load_delay(r));

    /* Free non-dirty cached cpu regs (they'll be reloaded with the new
     * load-delay value if needed). */
    const u32 req = (CPU_RC_HR_ALLOCATED | CPU_RC_HR_MODE_WRITE);
    for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++) {
      cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[i];
      if (ra->type != CPU_RC_HRT_CPU_REG ||
          !cpu_recompiler_is_host_reg_allocated(r, i) ||
          ((ra->flags & req) == req)) 
        continue;
      DebugAssert(!(ra->flags & CPU_RC_HR_MODE_WRITE));
      cpu_recompiler_clear_host_reg(r, i);
    }

    /* Drop non-dirty constants. */
    for (u32 i = 1; i < CPU_REG_COUNT; i++) {
      if (!cpu_recompiler_has_constant_reg(r, (cpu_reg_t)i) ||
          cpu_recompiler_has_dirty_constant_reg(r, (cpu_reg_t)i))
        continue;
      cpu_recompiler_clear_constant_reg(r, (cpu_reg_t)i);
    }

    cpu_recompiler_flush(r, CPU_RC_FLUSH_LOAD_DELAY_FROM_STATE);
  }

  cpu_recompiler_finish_load_delay(r);

  /* Move next-load-delay forward into current. */
  if (r->next_load_delay_register != (cpu_reg_t)CPU_REG_COUNT) {
    if (r->next_load_delay_value_register == CPU_RECOMP_NUM_HOST_REGS) {
      cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ,
                                       CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE,
                                       r->next_load_delay_register);
      DebugAssert(r->next_load_delay_value_register != CPU_RECOMP_NUM_HOST_REGS);
    }

    cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[r->next_load_delay_value_register];
    ra->flags |= CPU_RC_HR_MODE_WRITE;
    ra->type = CPU_RC_HRT_LOAD_DELAY_VALUE;

    r->load_delay_register = r->next_load_delay_register;
    r->load_delay_value_register = r->next_load_delay_value_register;
    r->next_load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
    r->next_load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
  }
}

void cpu_recompiler_finish_load_delay(cpu_recompiler_t* r)
{
  DebugAssert(!r->load_delay_dirty);
  if (!cpu_recompiler_has_load_delay(r))
    return;

  if (r->load_delay_value_register == CPU_RECOMP_NUM_HOST_REGS) {
    cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ,
                                     CPU_RC_HRT_LOAD_DELAY_VALUE, r->load_delay_register);
    DebugAssert(r->load_delay_value_register != CPU_RECOMP_NUM_HOST_REGS);
  }

  /* Kill any (old) cached value for this register. */
  cpu_recompiler_delete_mips_reg(r, r->load_delay_register, false);

  cpu_recomp_host_reg_alloc_t* ra = &r->host_regs[r->load_delay_value_register];
  DebugAssert(ra->reg == r->load_delay_register);
  ra->flags = (u8)((ra->flags & CPU_RC_HR_IMMUTABLE_FLAGS) |
                   CPU_RC_HR_ALLOCATED | CPU_RC_HR_MODE_READ | CPU_RC_HR_MODE_WRITE);
  ra->counter = r->register_alloc_counter++;
  ra->type = CPU_RC_HRT_CPU_REG;

  cpu_recompiler_clear_constant_reg(r, r->load_delay_register);

  r->load_delay_register = (cpu_reg_t)CPU_REG_COUNT;
  r->load_delay_value_register = CPU_RECOMP_NUM_HOST_REGS;
}

void cpu_recompiler_finish_load_delay_to_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  if (r->load_delay_dirty) {
    cpu_recompiler_update_load_delay(r);
    return;
  }
  if (r->load_delay_register != reg)
    return;
  cpu_recompiler_finish_load_delay(r);
}

u32 cpu_recompiler_get_flags_for_new_load_delayed_reg(const cpu_recompiler_t* r)
{
  (void)r;
  return g_settings.gpu_pgxp_enable
           ? (CPU_RC_HR_MODE_WRITE | CPU_RC_HR_CALLEE_SAVED)
           : (CPU_RC_HR_MODE_WRITE);
}

/* LWL/LWR codegen hardcodes RCX/RAX/RDX as merge scratch (shift count, aligned
 * word, mask).  If the value reg is allocated as one of these caller-saved
 * scratches, the merge clobbers the existing/in-flight value before the AND/OR.
 * Force callee-saved here.  SWL/SWR materialize into a fixed RWARG2 instead and
 * don't hit this. */
u32 cpu_recompiler_get_flags_for_lwx_value_reg(const cpu_recompiler_t* r)
{
  return cpu_recompiler_get_flags_for_new_load_delayed_reg(r) | CPU_RC_HR_CALLEE_SAVED;
}

void cpu_recompiler_backup_host_state(cpu_recompiler_t* r)
{
  DebugAssert(r->host_state_backup_count <
              (sizeof(r->host_state_backup) / sizeof(r->host_state_backup[0])));

  cpu_recomp_state_backup_t* bu = &r->host_state_backup[r->host_state_backup_count];
  bu->cycles = r->cycles;
  bu->gte_done_cycle = r->gte_done_cycle;
  bu->muldiv_done_cycle = r->muldiv_done_cycle;
  bu->compiler_pc = r->compiler_pc;
  bu->dirty_pc = r->dirty_pc;
  bu->dirty_instruction_bits = r->dirty_instruction_bits;
  bu->dirty_gte_done_cycle = r->dirty_gte_done_cycle;
  bu->dirty_muldiv_done_cycle = r->dirty_muldiv_done_cycle;
  bu->block_ended = r->block_ended;
  bu->inst = r->inst;
  bu->iinfo = r->iinfo;
  bu->current_instruction_pc = r->current_instruction_pc;
  bu->current_instruction_delay_slot = r->current_instruction_branch_delay_slot;
  bu->const_regs_valid = r->constant_regs_valid;
  bu->const_regs_dirty = r->constant_regs_dirty;
  memcpy(bu->const_regs_values, r->constant_reg_values, sizeof(bu->const_regs_values));
  memcpy(bu->host_regs, r->host_regs, sizeof(bu->host_regs));
  bu->register_alloc_counter = r->register_alloc_counter;
  bu->load_delay_dirty = r->load_delay_dirty;
  bu->load_delay_register = r->load_delay_register;
  bu->load_delay_value_register = r->load_delay_value_register;
  bu->next_load_delay_register = r->next_load_delay_register;
  bu->next_load_delay_value_register = r->next_load_delay_value_register;
  r->host_state_backup_count++;
}

void cpu_recompiler_restore_host_state(cpu_recompiler_t* r)
{
  DebugAssert(r->host_state_backup_count > 0);
  r->host_state_backup_count--;

  cpu_recomp_state_backup_t* bu = &r->host_state_backup[r->host_state_backup_count];
  memcpy(r->host_regs, bu->host_regs, sizeof(r->host_regs));
  memcpy(r->constant_reg_values, bu->const_regs_values, sizeof(r->constant_reg_values));
  r->constant_regs_dirty = bu->const_regs_dirty;
  r->constant_regs_valid = bu->const_regs_valid;
  r->current_instruction_branch_delay_slot = bu->current_instruction_delay_slot;
  r->current_instruction_pc = bu->current_instruction_pc;
  r->inst = bu->inst;
  r->iinfo = bu->iinfo;
  r->block_ended = bu->block_ended;
  r->dirty_gte_done_cycle = bu->dirty_gte_done_cycle;
  r->dirty_muldiv_done_cycle = bu->dirty_muldiv_done_cycle;
  r->dirty_instruction_bits = bu->dirty_instruction_bits;
  r->dirty_pc = bu->dirty_pc;
  r->compiler_pc = bu->compiler_pc;
  r->register_alloc_counter = bu->register_alloc_counter;
  r->load_delay_dirty = bu->load_delay_dirty;
  r->load_delay_register = bu->load_delay_register;
  r->load_delay_value_register = bu->load_delay_value_register;
  r->next_load_delay_register = bu->next_load_delay_register;
  r->next_load_delay_value_register = bu->next_load_delay_value_register;
  r->gte_done_cycle = bu->gte_done_cycle;
  r->muldiv_done_cycle = bu->muldiv_done_cycle;
  r->cycles = bu->cycles;
}

static u32 fnv1a_u32(u32 x)
{
  u32 h = 0x811C9DC5u;
  for (u32 i = 0; i < 4u; i++) {
    h ^= (x & 0xFFu);
    h *= 0x01000193u;
    x >>= 8;
  }
  return h;
}

static void spec_mem_grow(cpu_recompiler_t* r)
{
  const u32 old_cap = r->spec_mem_cap;
  cpu_recomp_spec_mem_entry_t* old = r->spec_mem_entries;
  const u32 new_cap = (old_cap == 0u) ? 32u : (old_cap * 2u);

  cpu_recomp_spec_mem_entry_t* fresh = (cpu_recomp_spec_mem_entry_t*)
      malloc(sizeof(cpu_recomp_spec_mem_entry_t) * new_cap);
  AssertMsg(fresh != NULL, "spec_mem allocation failed");
  for (u32 i = 0; i < new_cap; i++) {
    fresh[i].addr = CPU_RECOMP_SPEC_MEM_EMPTY_KEY;
    fresh[i].value.has = false;
    fresh[i].value.val = 0u;
  }

  /* Re-insert old entries. */
  if (old != NULL) {
    for (u32 i = 0; i < old_cap; i++) {
      if (old[i].addr == CPU_RECOMP_SPEC_MEM_EMPTY_KEY)
        continue;
      u32 mask = new_cap - 1u;
      u32 slot = fnv1a_u32(old[i].addr) & mask;
      while (fresh[slot].addr != CPU_RECOMP_SPEC_MEM_EMPTY_KEY)
        slot = (slot + 1u) & mask;
      fresh[slot] = old[i];
    }
    free(old);
  }

  r->spec_mem_entries = fresh;
  r->spec_mem_cap = new_cap;
}

void cpu_recompiler_init_speculative_regs(cpu_recompiler_t* r)
{
  for (u32 i = 0; i < CPU_REG_COUNT; i++) {
    r->spec_regs[i].has = (i == 0u);
    r->spec_regs[i].val = 0u;
  }
  /* Seed from any constant we already know about (mainly $zero). */
  for (u32 i = 0; i < CPU_REG_COUNT; i++) {
    if (cpu_recompiler_has_constant_reg(r, (cpu_reg_t)i)) {
      r->spec_regs[i].has = true;
      r->spec_regs[i].val = r->constant_reg_values[i];
    }
  }

  /* Wipe spec memory map. */
  if (r->spec_mem_entries != NULL) {
    for (u32 i = 0; i < r->spec_mem_cap; i++) {
      r->spec_mem_entries[i].addr = CPU_RECOMP_SPEC_MEM_EMPTY_KEY;
      r->spec_mem_entries[i].value.has = false;
      r->spec_mem_entries[i].value.val = 0u;
    }
  }
  r->spec_mem_count = 0u;

  r->spec_cop0_sr.has = true;
  r->spec_cop0_sr.val = g_cpu_state.cop0_regs.sr.bits;
}

void cpu_recompiler_invalidate_speculative_values(cpu_recompiler_t* r)
{
  for (u32 i = 1; i < CPU_REG_COUNT; i++) {
    r->spec_regs[i].has = false;
    r->spec_regs[i].val = 0u;
  }
  if (r->spec_mem_entries != NULL) {
    for (u32 i = 0; i < r->spec_mem_cap; i++)
      r->spec_mem_entries[i].addr = CPU_RECOMP_SPEC_MEM_EMPTY_KEY;
  }
  r->spec_mem_count = 0u;
  r->spec_cop0_sr.has = false;
  r->spec_cop0_sr.val = 0u;
}

cpu_recomp_spec_value_t cpu_recompiler_spec_read_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  return r->spec_regs[(u32)reg];
}

void cpu_recompiler_spec_write_reg(cpu_recompiler_t* r, cpu_reg_t reg, cpu_recomp_spec_value_t v)
{
  if ((u32)reg == 0u)
    return;
  r->spec_regs[(u32)reg] = v;
}

void cpu_recompiler_spec_invalidate_reg(cpu_recompiler_t* r, cpu_reg_t reg)
{
  if ((u32)reg == 0u)
    return;
  r->spec_regs[(u32)reg].has = false;
  r->spec_regs[(u32)reg].val = 0u;
}

void cpu_recompiler_spec_copy_reg(cpu_recompiler_t* r, cpu_reg_t dst, cpu_reg_t src)
{
  if ((u32)dst == 0u)
    return;
  r->spec_regs[(u32)dst] = r->spec_regs[(u32)src];
}

cpu_recomp_spec_value_t cpu_recompiler_spec_read_mem(cpu_recompiler_t* r, u32 addr)
{
  cpu_recomp_spec_value_t miss = {.has = false, .val = 0u};
  if (r->spec_mem_entries == NULL || r->spec_mem_cap == 0u)
    return miss;

  const u32 mask = r->spec_mem_cap - 1u;
  u32 slot = fnv1a_u32(addr) & mask;
  for (u32 probes = 0; probes < r->spec_mem_cap; probes++) {
    if (r->spec_mem_entries[slot].addr == CPU_RECOMP_SPEC_MEM_EMPTY_KEY)
      return miss;
    if (r->spec_mem_entries[slot].addr == addr)
      return r->spec_mem_entries[slot].value;
    slot = (slot + 1u) & mask;
  }
  return miss;
}

void cpu_recompiler_spec_write_mem(cpu_recompiler_t* r, virtual_memory_address_t addr,
                                   cpu_recomp_spec_value_t v)
{
  /* Grow if load factor would exceed ~70%. */
  if (r->spec_mem_cap == 0u || (r->spec_mem_count + 1u) * 10u > r->spec_mem_cap * 7u)
    spec_mem_grow(r);

  const u32 mask = r->spec_mem_cap - 1u;
  u32 slot = fnv1a_u32(addr) & mask;
  for (u32 probes = 0; probes < r->spec_mem_cap; probes++) {
    if (r->spec_mem_entries[slot].addr == CPU_RECOMP_SPEC_MEM_EMPTY_KEY) {
      r->spec_mem_entries[slot].addr = addr;
      r->spec_mem_entries[slot].value = v;
      r->spec_mem_count++;
      return;
    }
    if (r->spec_mem_entries[slot].addr == addr) {
      r->spec_mem_entries[slot].value = v;
      return;
    }
    slot = (slot + 1u) & mask;
  }
  Panic("spec_mem map full");
}

void cpu_recompiler_spec_invalidate_mem(cpu_recompiler_t* r, virtual_memory_address_t addr)
{
  if (r->spec_mem_entries == NULL || r->spec_mem_cap == 0u)
    return;
  const u32 mask = r->spec_mem_cap - 1u;
  u32 slot = fnv1a_u32(addr) & mask;
  for (u32 probes = 0; probes < r->spec_mem_cap; probes++) {
    if (r->spec_mem_entries[slot].addr == CPU_RECOMP_SPEC_MEM_EMPTY_KEY)
      return;
    if (r->spec_mem_entries[slot].addr == addr) {
      r->spec_mem_entries[slot].value.has = false;
      r->spec_mem_entries[slot].value.val = 0u;
      return;
    }
    slot = (slot + 1u) & mask;
  }
}

bool cpu_recompiler_spec_is_cache_isolated(cpu_recompiler_t* r)
{
  /* SR.Isc bit determines whether stores hit memory or
   * the cache.  When SR is unknown speculatively, assume not isolated. */
  if (!r->spec_cop0_sr.has)
    return false;
  cpu_cop0_sr_t sr;
  sr.bits = r->spec_cop0_sr.val;
  return sr.Isc != 0u;
}

#include "bus.h"
#include "cpu_code_cache.h"
#include "cpu_core_private.h"

__attribute__((weak)) bool cpu_code_cache_has_previously_faulted_on_pc(u32 pc) { (void)pc; return false; }
__attribute__((weak)) void cpu_code_cache_add_load_store_info(void* code_address, u32 code_size,
                                                               u32 guest_pc, u32 guest_block,
                                                              tick_count_t cycles, u32 gpr_bitmask, 
                                                              u8 address_register, u8 data_register,
                                                              memory_access_size_t size,
                                                              bool is_signed, bool is_load)
{
  (void)code_address; (void)code_size; (void)guest_pc; (void)guest_block;
  (void)cycles; (void)gpr_bitmask; (void)address_register; (void)data_register;
  (void)size; (void)is_signed; (void)is_load;
}

cpu_reg_t cpu_recompiler_mips_d(const cpu_recompiler_t* r) { return (cpu_reg_t)r->inst->r.rd; }

void cpu_recompiler_set_compiler_pc(cpu_recompiler_t* r, u32 newpc)
{
  r->compiler_pc = newpc;
  r->dirty_pc = true;
}

 u32 cpu_recompiler_get_conditional_branch_target(const cpu_recompiler_t* r,
                                                 cpu_recomp_compile_flags_t cf) 
{
  /* compiler pc has already been advanced when swapping branch delay slots */
  const u32 cur = r->compiler_pc - (cf.delay_slot_swapped ? CPU_INSTRUCTION_SIZE : 0u);
  return cur + (cpu_instr_imm_sext32(*r->inst) << 2);
}

 u32 cpu_recompiler_get_branch_return_address(const cpu_recompiler_t* r,
                                             cpu_recomp_compile_flags_t cf) 
{
  return r->compiler_pc + (cf.delay_slot_swapped ? 0u : CPU_INSTRUCTION_SIZE);
}

void cpu_recompiler_truncate_block(cpu_recompiler_t* r)
{
  r->block->size = ((r->current_instruction_pc - r->block->pc) / CPU_INSTRUCTION_SIZE) + 1u;
  r->iinfo->is_last_instruction = 1u;
}

const tick_count_t* cpu_recompiler_get_fetch_memory_access_time_ptr(const cpu_recompiler_t* r)
{
  const tick_count_t* p = bus_get_memory_access_time_ptr(cpu_virtual_to_physical(r->block->pc),
                                                         MEMORY_ACCESS_SIZE_WORD);
  AssertMsg(p != NULL, "Address has dynamic fetch ticks");
  return p;
}

void cpu_recompiler_flush_for_load_store(cpu_recompiler_t* r, const u32* addr, bool store, bool use_fastmem)
{
  (void)addr; (void)store;
  if (use_fastmem) return;
  cpu_recompiler_flush(r, CPU_RC_FLUSH_FOR_C_CALL | CPU_RC_FLUSH_FOR_LOADSTORE);
}

void cpu_recompiler_add_gte_ticks(cpu_recompiler_t* r, tick_count_t ticks)
{
  r->gte_done_cycle = r->cycles + ticks;
}

void cpu_recompiler_stall_until_gte_complete(cpu_recompiler_t* r)
{
  /* Subtract one for the current-instruction tick before stall, add it back. */
  DebugAssert(r->cycles > 0);
  r->cycles--;

  if (!r->dirty_gte_done_cycle) {
    if (r->gte_done_cycle > r->cycles)
      r->cycles += (r->gte_done_cycle - r->cycles);
  } else {
    cpu_recompiler_flush(r, CPU_RC_FLUSH_GTE_STALL_FROM_STATE);
  }
  r->cycles++;
}

/* MULT/MULTU/DIV/DIVU update muldiv_completion_tick (or r->muldiv_done_cycle
 * when the cycle count is known statically); MFHI/MFLO/MTHI/MTLO wait for
 * it (see interp cpu_stall_until_muldiv_complete()).  Two cases:
 *
 * 1. Clean (in-block scheduling): the producer was DIV/DIVU (35 cycles
 *    static) or MULT/MULTU with constant rs (cycle count statically
 *    known); r->muldiv_done_cycle holds the cycle offset and we just
 *    bump r->cycles up to it without touching pending_ticks state.
 *
 * 2. Dirty (state-resident): the producer was non-const-rs MULT/MULTU
 *    (or it crossed a block boundary).  muldiv_completion_tick is in
 *    state; flush via the backend's CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE
 *    handler (which performs `pending_ticks = max(pending_ticks + cycles,
 *    muldiv_completion_tick)` and zeroes r->cycles).
 *
 * Unlike GTE's stall (which subtracts/re-adds the current inst tick to
 * mirror cpu_add_gte_ticks's `+ 1`), muldiv's interp helper
 * cpu_add_muldiv_ticks uses NO `+ 1` (cpu_core_private.h:152), so the
 * consumer just compares against the post-incremented r->cycles directly. */
void cpu_recompiler_stall_until_muldiv_complete(cpu_recompiler_t* r)
{
  if (!r->dirty_muldiv_done_cycle) {
    if (r->muldiv_done_cycle > r->cycles)
      r->cycles = r->muldiv_done_cycle;
  } else {
    cpu_recompiler_flush(r, CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE);
  }
}

/* Producer side: stash the muldiv completion offset.  Two flavours:
 *  - cpu_recompiler_set_muldiv_done_cycle_static: cycles is known at
 *    compile time (DIV/DIVU = 35; MULT/MULTU with const rs).  Sets
 *    r->muldiv_done_cycle = r->cycles + cycles, dirty=false.  In-block
 *    consumers (MFHI/MFLO/MTHI/MTLO) see it via r->cycles math.  At
 *    end-of-block, FLUSH_MULDIV_DONE_CYCLE writes the absolute completion
 *    tick to state for cross-block visibility.
 *  - cpu_recompiler_set_muldiv_done_cycle_dynamic: emit producer code
 *    that writes muldiv_completion_tick to state directly, sets
 *    dirty=true.  Used by MULT/MULTU when rs is a runtime value (cycle
 *    count requires runtime branching on rs).  In-block consumers then
 *    flush from state. */
void cpu_recompiler_set_muldiv_done_cycle_static(cpu_recompiler_t* r, s32 cycles)
{
  r->muldiv_done_cycle = r->cycles + cycles;
  r->dirty_muldiv_done_cycle = false;
}

bool cpu_recompiler_try_swap_delay_slot(cpu_recompiler_t* r, cpu_reg_t rs, cpu_reg_t rt, cpu_reg_t rd)
{
  if (!CPU_RECOMP_SWAP_BRANCH_DELAY_SLOTS)
    return false;

  const cpu_instruction_t* next = r->inst + 1;
  DebugAssert(next < (cpu_code_cache_block_instructions(r->block) + r->block->size));

  const cpu_reg_t opcode_rs = (cpu_reg_t)next->r.rs;
  const cpu_reg_t opcode_rt = (cpu_reg_t)next->r.rt;
  const cpu_reg_t opcode_rd = (cpu_reg_t)next->r.rd;

  const cpu_instruction_t* backup_inst = r->inst;
  const u32 backup_pc = r->current_instruction_pc;
  const bool backup_ds = r->current_instruction_branch_delay_slot;
  bool safe = false;

  if (next->bits == 0u) { safe = true; goto done_check; }

  if ((CPU_RECOMP_EMULATE_LOAD_DELAYS && r->block->pc == r->current_instruction_pc) ||
       r->load_delay_dirty ||
      (cpu_recompiler_has_load_delay(r) && 
       (r->load_delay_register == rs || r->load_delay_register == rt || r->load_delay_register == rd))) {
    return false;
  }

  switch ((cpu_instruction_op_t)next->any.op) {
    case CPU_OP_ADDI: case CPU_OP_ADDIU: case CPU_OP_SLTI: case CPU_OP_SLTIU:
    case CPU_OP_ANDI: case CPU_OP_ORI:   case CPU_OP_XORI: case CPU_OP_LUI:
    case CPU_OP_LB:   case CPU_OP_LH:    case CPU_OP_LWL:  case CPU_OP_LW:
    case CPU_OP_LBU:  case CPU_OP_LHU:   case CPU_OP_LWR: {
      if ((rs != (cpu_reg_t)0 && rs == opcode_rt) ||
          (rt != (cpu_reg_t)0 && rt == opcode_rt) ||
          (rd != (cpu_reg_t)0 && (rd == opcode_rs || rd == opcode_rt))) {
        return false;
      }
      safe = true;
    } break;

    case CPU_OP_SB: case CPU_OP_SH: case CPU_OP_SWL: case CPU_OP_SW: case CPU_OP_SWR:
    case CPU_OP_LWC2: case CPU_OP_SWC2:
      safe = true;
      break;

    case CPU_OP_FUNCT: {
      switch ((cpu_instruction_funct_t)next->r.funct) {
        case CPU_FUNCT_SLL: case CPU_FUNCT_SRL: case CPU_FUNCT_SRA:
        case CPU_FUNCT_SLLV: case CPU_FUNCT_SRLV: case CPU_FUNCT_SRAV:
        case CPU_FUNCT_ADD: case CPU_FUNCT_ADDU: case CPU_FUNCT_SUB: case CPU_FUNCT_SUBU:
        case CPU_FUNCT_AND: case CPU_FUNCT_OR:   case CPU_FUNCT_XOR: case CPU_FUNCT_NOR:
        case CPU_FUNCT_SLT: case CPU_FUNCT_SLTU: {
          if ((rs != (cpu_reg_t)0 && rs == opcode_rd) ||
              (rt != (cpu_reg_t)0 && rt == opcode_rd) ||
              (rd != (cpu_reg_t)0 && (rd == opcode_rs || rd == opcode_rt))) {
            return false;
          }
          safe = true;
        } break;

        case CPU_FUNCT_MULT: case CPU_FUNCT_MULTU: case CPU_FUNCT_DIV: case CPU_FUNCT_DIVU:
          safe = true;
          break;

        default:
          return false;
      }
    } break;

    case CPU_OP_COP0: case CPU_OP_COP1: case CPU_OP_COP2: case CPU_OP_COP3: {
      if (cpu_cop_is_common_instruction(*next)) {
        switch (cpu_cop_common_op(*next)) {
          case CPU_COP_COMMON_MFCN: case CPU_COP_COMMON_CFCN: {
            if ((rs != (cpu_reg_t)0 && rs == opcode_rt) ||
                (rt != (cpu_reg_t)0 && rt == opcode_rt) ||
                (rd != (cpu_reg_t)0 && rd == opcode_rt)) {
              return false;
            }
            safe = true;
          } break;
          case CPU_COP_COMMON_MTCN: case CPU_COP_COMMON_CTCN:
            safe = true;
            break;
          default:
            return false;
        }
      } else {
        if ((cpu_instruction_op_t)next->any.op != CPU_OP_COP2)
          return false;
        safe = true;
      }
    } break;

    default:
      return false;
  }

done_check:
  if (!safe) return false;

  cpu_recompiler_compile_branch_delay_slot(r, true);
  r->inst = backup_inst;
  r->current_instruction_pc = backup_pc;
  r->current_instruction_branch_delay_slot = backup_ds;
  return true;
}

void cpu_recompiler_compile_move_reg_template(cpu_recompiler_t* r, cpu_reg_t dst, cpu_reg_t src,
                                              bool pgxp_move)
{
  if (dst == src || dst == (cpu_reg_t)0)
    return;

  if (cpu_recompiler_has_constant_reg(r, src)) {
    cpu_recompiler_delete_mips_reg(r, dst, false);
    cpu_recompiler_set_constant_reg(r, dst, cpu_recompiler_get_constant_reg_u32(r, src));
  } else {
    const u32 srcreg = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_CPU_REG, src);
    if (!cpu_recompiler_try_rename_mips_reg(r, dst, src, srcreg, (cpu_reg_t)CPU_REG_COUNT)) {
      const u32 dstreg = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_WRITE, CPU_RC_HRT_CPU_REG, dst);
      if (r->v && r->v->copy_host_reg)
        r->v->copy_host_reg(r, dstreg, srcreg);
      cpu_recompiler_clear_host_reg_needed(r, dstreg);
    }
  }

  /* PGXP MOVE wiring lands in 6.F. */
  (void)pgxp_move;
}

#define MD(R) (cpu_recompiler_mips_d(R))
#define MS(CF) (cpu_recomp_cf_mips_s(CF))
#define MT(CF) (cpu_recomp_cf_mips_t(CF))
#define KU32(R, REG) cpu_recompiler_get_constant_reg_u32(R, REG)
#define KS32(R, REG) cpu_recompiler_get_constant_reg_s32(R, REG)
#define HASK(R, REG) cpu_recompiler_has_constant_reg(R, REG)
#define HASKV(R, REG, V) cpu_recompiler_has_constant_reg_value(R, REG, V)
#define SETK(R, REG, V) cpu_recompiler_set_constant_reg(R, REG, V)

void cpu_recompiler_compile_sll_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MT(cf)) << r->inst->r.shamt);
}
void cpu_recompiler_compile_srl_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MT(cf)) >> r->inst->r.shamt);
}
void cpu_recompiler_compile_sra_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MT(cf)));
  SETK(r, MD(r), (u32)(KS32(r, MT(cf)) >> r->inst->r.shamt));
}
void cpu_recompiler_compile_sllv_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MT(cf)) << (KU32(r, MS(cf)) & 0x1Fu));
}
void cpu_recompiler_compile_srlv_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MT(cf)) >> (KU32(r, MS(cf)) & 0x1Fu));
}
void cpu_recompiler_compile_srav_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), (u32)(KS32(r, MT(cf)) >> (KU32(r, MS(cf)) & 0x1Fu)));
}

void cpu_recompiler_compile_and_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MS(cf)) & KU32(r, MT(cf)));
}
void cpu_recompiler_compile_or_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MS(cf)) | KU32(r, MT(cf)));
}
void cpu_recompiler_compile_xor_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MS(cf)) ^ KU32(r, MT(cf)));
}
void cpu_recompiler_compile_nor_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), ~(KU32(r, MS(cf)) | KU32(r, MT(cf))));
}

void cpu_recompiler_compile_slt_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), (KS32(r, MS(cf)) < KS32(r, MT(cf))) ? 1u : 0u);
}
void cpu_recompiler_compile_sltu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), (KU32(r, MS(cf)) < KU32(r, MT(cf))) ? 1u : 0u);
}

void cpu_recompiler_compile_mult_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  const s32 rs_s = KS32(r, MS(cf));
  const u64 res = (u64)((s64)rs_s * (s64)KS32(r, MT(cf)));
  SETK(r, (cpu_reg_t)CPU_REG_HI, (u32)(res >> 32));
  SETK(r, (cpu_reg_t)CPU_REG_LO, (u32)res);
  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_mult_ticks_signed(rs_s));
}
void cpu_recompiler_compile_multu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  const u32 rs_u = KU32(r, MS(cf));
  const u64 res = (u64)rs_u * (u64)KU32(r, MT(cf));
  SETK(r, (cpu_reg_t)CPU_REG_HI, (u32)(res >> 32));
  SETK(r, (cpu_reg_t)CPU_REG_LO, (u32)res);
  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_mult_ticks_unsigned(rs_u));
}

void cpu_recompiler_compile_div_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  u32 lo, hi;
  cpu_recompiler_mips_signed_divide(KS32(r, MS(cf)), KS32(r, MT(cf)), &lo, &hi);
  SETK(r, (cpu_reg_t)CPU_REG_HI, hi);
  SETK(r, (cpu_reg_t)CPU_REG_LO, lo);
  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_div_ticks());
}
void cpu_recompiler_compile_divu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  u32 lo, hi;
  cpu_recompiler_mips_unsigned_divide(KU32(r, MS(cf)), KU32(r, MT(cf)), &lo, &hi);
  SETK(r, (cpu_reg_t)CPU_REG_HI, hi);
  SETK(r, (cpu_reg_t)CPU_REG_LO, lo);
  cpu_recompiler_set_muldiv_done_cycle_static(r, (s32)cpu_get_div_ticks());
}

void cpu_recompiler_compile_add_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  if (MD(r) != (cpu_reg_t)0)
    SETK(r, MD(r), KU32(r, MS(cf)) + KU32(r, MT(cf)));
}
void cpu_recompiler_compile_addu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MS(cf)) + KU32(r, MT(cf)));
}
void cpu_recompiler_compile_sub_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  if (MD(r) != (cpu_reg_t)0)
    SETK(r, MD(r), KU32(r, MS(cf)) - KU32(r, MT(cf)));
}
void cpu_recompiler_compile_subu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  SETK(r, MD(r), KU32(r, MS(cf)) - KU32(r, MT(cf)));
}

void cpu_recompiler_compile_addi_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  if (MT(cf) != (cpu_reg_t)0)
    SETK(r, MT(cf), KU32(r, MS(cf)) + cpu_instr_imm_sext32(*r->inst));
}
void cpu_recompiler_compile_addiu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), KU32(r, MS(cf)) + cpu_instr_imm_sext32(*r->inst));
}
void cpu_recompiler_compile_slti_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), (KS32(r, MS(cf)) < (s32)cpu_instr_imm_sext32(*r->inst)) ? 1u : 0u);
}
void cpu_recompiler_compile_sltiu_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), (KU32(r, MS(cf)) < cpu_instr_imm_sext32(*r->inst)) ? 1u : 0u);
}
void cpu_recompiler_compile_andi_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), KU32(r, MS(cf)) & cpu_instr_imm_zext32(*r->inst));
}
void cpu_recompiler_compile_ori_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), KU32(r, MS(cf)) | cpu_instr_imm_zext32(*r->inst));
}
void cpu_recompiler_compile_xori_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  SETK(r, MT(cf), KU32(r, MS(cf)) ^ cpu_instr_imm_zext32(*r->inst));
}

void cpu_recompiler_compile_lui(cpu_recompiler_t* r)
{
  if (r->inst->i.rt == 0u)
    return;
  cpu_recompiler_set_constant_reg(r, (cpu_reg_t)r->inst->i.rt,
                                  cpu_instr_imm_zext32(*r->inst) << 16);
  /* PGXP CPU_LUI hook lands in 6.F. */
}

void cpu_recompiler_compile_mfc0(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  const cpu_cop0_reg_t reg = (cpu_cop0_reg_t)MD(r);
  const u32* ptr = cpu_recompiler_get_cop0_reg_ptr(reg);
  if (ptr == NULL) {
    ERROR_LOG("Read from unknown cop0 reg %u", (unsigned)reg);
    if (r->v && r->v->compile_fallback)
      r->v->compile_fallback(r);
    return;
  }
  DebugAssert(cf.valid_host_t);
  if (r->v && r->v->load_host_reg_from_cpu_pointer)
    r->v->load_host_reg_from_cpu_pointer(r, cf.host_t, ptr);
}

void cpu_recompiler_compile_j(cpu_recompiler_t* r)
{
  const u32 newpc = (r->compiler_pc & 0xF0000000u) | (r->inst->j.target << 2);
  /* Write pc=branch_target at runtime now so a delay-slot exception sees the
   * correct value for TAR (cpu_raise_exception_with_vector reads it when
   * cause.BD is set).  Mark BT for the same reason. */
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_jr_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  const u32 newpc = KU32(r, MS(cf));
  if ((newpc & 3u) && g_settings.cpu_recompiler_memory_exceptions) {
    if (r->v && r->v->end_block_with_exception)
      r->v->end_block_with_exception(r, CPU_EXCEPTION_ADEL);
    r->block_ended = true;
    return;
  }
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_jal(cpu_recompiler_t* r)
{
  const u32 newpc = (r->compiler_pc & 0xF0000000u) | (r->inst->j.target << 2);
  cpu_recomp_compile_flags_t empty = {.bits = 0u};
  cpu_recompiler_set_constant_reg(r, (cpu_reg_t)CPU_REG_RA,
                                  cpu_recompiler_get_branch_return_address(r, empty));
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_jalr_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  const u32 newpc = KU32(r, MS(cf));
  if (MD(r) != (cpu_reg_t)0) {
    cpu_recomp_compile_flags_t empty = {.bits = 0u};
    cpu_recompiler_set_constant_reg(r, MD(r), cpu_recompiler_get_branch_return_address(r, empty));
  }
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = true;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_syscall(cpu_recompiler_t* r)
{
  if (r->v && r->v->end_block_with_exception)
    r->v->end_block_with_exception(r, CPU_EXCEPTION_SYSCALL);
  r->block_ended = true;
}

void cpu_recompiler_compile_break(cpu_recompiler_t* r)
{
  if (r->v && r->v->end_block_with_exception)
    r->v->end_block_with_exception(r, CPU_EXCEPTION_BP);
  r->block_ended = true;
}

void cpu_recompiler_compile_b_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  DebugAssert(HASK(r, MS(cf)));
  const u8 irt = (u8)r->inst->i.rt;
  const bool bgez = (irt & 1u) != 0u;
  const bool link = ((irt & 0x1Eu) == 0x10u);
  const s32 rs = KS32(r, MS(cf));
  const bool taken = bgez ? (rs >= 0) : (rs < 0);
  const u32 taken_pc = cpu_recompiler_get_conditional_branch_target(r, cf);

  if (link)
    cpu_recompiler_set_constant_reg(r, (cpu_reg_t)CPU_REG_RA,
                                    cpu_recompiler_get_branch_return_address(r, cf));

  /* compiler_pc here = delay_slot_pc (branch_pc+4); after compile_branch_delay_slot
   * it advances to branch_pc+8 (post-delay-slot fall-through). */
  const u32 newpc = taken ? taken_pc : (r->compiler_pc + CPU_INSTRUCTION_SIZE);
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = taken;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_b(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{
  const u8 irt = (u8)r->inst->i.rt;
  const bool bgez = (irt & 1u) != 0u;
  const bool link = ((irt & 0x1Eu) == 0x10u);

  if (link)
    cpu_recompiler_set_constant_reg(r, (cpu_reg_t)CPU_REG_RA,
                                    cpu_recompiler_get_branch_return_address(r, cf));
  if (r->v && r->v->compile_bxx)
    r->v->compile_bxx(r, cf, bgez ? CPU_RC_BC_GE_ZERO : CPU_RC_BC_LT_ZERO);
}

void cpu_recompiler_compile_blez(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ if (r->v && r->v->compile_bxx) r->v->compile_bxx(r, cf, CPU_RC_BC_LE_ZERO); }
void cpu_recompiler_compile_blez_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ cpu_recompiler_compile_bxx_const(r, cf, CPU_RC_BC_LE_ZERO); }
void cpu_recompiler_compile_bgtz(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ if (r->v && r->v->compile_bxx) r->v->compile_bxx(r, cf, CPU_RC_BC_GT_ZERO); }
void cpu_recompiler_compile_bgtz_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ cpu_recompiler_compile_bxx_const(r, cf, CPU_RC_BC_GT_ZERO); }
void cpu_recompiler_compile_beq(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ if (r->v && r->v->compile_bxx) r->v->compile_bxx(r, cf, CPU_RC_BC_EQ); }
void cpu_recompiler_compile_beq_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ cpu_recompiler_compile_bxx_const(r, cf, CPU_RC_BC_EQ); }
void cpu_recompiler_compile_bne(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ if (r->v && r->v->compile_bxx) r->v->compile_bxx(r, cf, CPU_RC_BC_NE); }
void cpu_recompiler_compile_bne_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf)
{ cpu_recompiler_compile_bxx_const(r, cf, CPU_RC_BC_NE); }

void cpu_recompiler_compile_bxx_const(cpu_recompiler_t* r, cpu_recomp_compile_flags_t cf,
                                      cpu_recomp_branch_cond_t cond)
{
  DebugAssert(HASK(r, MS(cf)) && HASK(r, MT(cf)));
  bool taken = false;
  switch (cond) {
    case CPU_RC_BC_EQ:      taken = KU32(r, MS(cf)) == KU32(r, MT(cf)); break;
    case CPU_RC_BC_NE:      taken = KU32(r, MS(cf)) != KU32(r, MT(cf)); break;
    case CPU_RC_BC_GT_ZERO: taken = KS32(r, MS(cf)) >  0; break;
    case CPU_RC_BC_GE_ZERO: taken = KS32(r, MS(cf)) >= 0; break;
    case CPU_RC_BC_LT_ZERO: taken = KS32(r, MS(cf)) <  0; break;
    case CPU_RC_BC_LE_ZERO: taken = KS32(r, MS(cf)) <= 0; break;
    default: Panic("Unhandled branch condition");
  }
  const u32 taken_pc = cpu_recompiler_get_conditional_branch_target(r, cf);
  const u32 newpc = taken ? taken_pc : (r->compiler_pc + CPU_INSTRUCTION_SIZE);
  if (r->v && r->v->store_constant_to_cpu_pointer)
    r->v->store_constant_to_cpu_pointer(r, newpc, &g_cpu_state.pc);
  r->delay_slot_branch_was_taken = taken;
  cpu_recompiler_compile_branch_delay_slot(r, true);
  if (r->v && r->v->end_block)
    r->v->end_block(r, &newpc, true);
  r->block_ended = true;
}

void cpu_recompiler_compile_template(cpu_recompiler_t* r,
                                      cpu_recomp_compile_const_fn_t const_func,
                                     cpu_recomp_compile_fn_t func, 
                                     const void* pgxp_cpu_func,
                                     u32 tflags)
{
  bool allow_constant = (const_func != NULL);
  cpu_reg_t rs = (cpu_reg_t)r->inst->r.rs;
  cpu_reg_t rt = (cpu_reg_t)r->inst->r.rt;
  cpu_reg_t rd = (cpu_reg_t)r->inst->r.rd;

  if (tflags & CPU_RC_TF_GTE_STALL)
    cpu_recompiler_stall_until_gte_complete(r);

  /* throw away instructions writing to $zero */
  if (!(tflags & CPU_RC_TF_NO_NOP) &&
      (!g_settings.cpu_recompiler_memory_exceptions || !(tflags & CPU_RC_TF_CAN_OVERFLOW)) &&
      (((tflags & CPU_RC_TF_WRITES_T) && rt == (cpu_reg_t)0) || 
       ((tflags & CPU_RC_TF_WRITES_D) && rd == (cpu_reg_t)0))) {
    return;
  }

  /* rename ops with zero source */
  if ((tflags & CPU_RC_TF_RENAME_WITH_ZERO_T) && HASKV(r, rt, 0u)) {
    DebugAssert((tflags & (CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T))
                == (CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T));
    cpu_recompiler_compile_move_reg_template(r, rd, rs, true);
    return;
  } else if ((tflags & (CPU_RC_TF_RENAME_WITH_ZERO_T | CPU_RC_TF_COMMUTATIVE))
              == (CPU_RC_TF_RENAME_WITH_ZERO_T | CPU_RC_TF_COMMUTATIVE) && HASKV(r, rs, 0u)) {
    DebugAssert((tflags & (CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T))
                == (CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T));
    cpu_recompiler_compile_move_reg_template(r, rd, rt, true);
    return;
  } else if ((tflags & CPU_RC_TF_RENAME_WITH_ZERO_IMM) && r->inst->i.imm == 0u) {
    cpu_recompiler_compile_move_reg_template(r, rt, rs, true);
    return;
  }

  /* PGXP CPU-mode call.  Per-op pgxp_cpu_func wired via CT/CTNB; pre-emit
   * the call before the op codegen so PGXP sees pre-update operand values. */
  if (pgxp_cpu_func != NULL && g_settings.gpu_pgxp_enable &&
      ((tflags & CPU_RC_TF_PGXP_WITHOUT_CPU) || settings_using_pgxp_cpu_mode(&g_settings))) {
    cpu_reg_t reg_args[2] = {(cpu_reg_t)CPU_REG_COUNT, (cpu_reg_t)CPU_REG_COUNT};
    u32 num_reg_args = 0;
    if (tflags & CPU_RC_TF_READS_S)  reg_args[num_reg_args++] = rs;
    if (tflags & CPU_RC_TF_READS_T)  reg_args[num_reg_args++] = rt;
    if (tflags & CPU_RC_TF_READS_LO) reg_args[num_reg_args++] = (cpu_reg_t)CPU_REG_LO;
    if (tflags & CPU_RC_TF_READS_HI) reg_args[num_reg_args++] = (cpu_reg_t)CPU_REG_HI;
    DebugAssert(num_reg_args <= 2);
    if (r->v && r->v->generate_pgxp_call_with_mips_regs)
      r->v->generate_pgxp_call_with_mips_regs(r, pgxp_cpu_func, r->inst->bits,
                                              reg_args[0], reg_args[1]);
  }

  /* commutative swap when one operand is constant. */
  if ((tflags & CPU_RC_TF_COMMUTATIVE) && !(tflags & CPU_RC_TF_WRITES_T) &&
      ((HASK(r, rs) && !HASK(r, rt)) || ((tflags & CPU_RC_TF_WRITES_D) && rd == rt))) {
    cpu_reg_t tmp = rs; rs = rt; rt = tmp;
  }

  cpu_recomp_compile_flags_t cf = {.bits = 0u};

  if (tflags & CPU_RC_TF_READS_S) {
    cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rs);
    if (HASK(r, rs)) cf.const_s = 1u; else allow_constant = false;
  }
  if (tflags & CPU_RC_TF_READS_T) {
    cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rt);
    if (HASK(r, rt)) cf.const_t = 1u; else allow_constant = false;
  }
  if (tflags & CPU_RC_TF_READS_LO) {
    cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_LO);
    if (HASK(r, (cpu_reg_t)CPU_REG_LO)) cf.const_lo = 1u; else allow_constant = false;
  }
  if (tflags & CPU_RC_TF_READS_HI) {
    cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_HI);
    if (HASK(r, (cpu_reg_t)CPU_REG_HI)) cf.const_hi = 1u; else allow_constant = false;
  }

  if (tflags & CPU_RC_TF_READS_S) cf.mips_s = (u32)rs;
  if (tflags & (CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_T)) cf.mips_t = (u32)rt;

  if (allow_constant) {
    const_func(r, cf);
    return;
  }

  cpu_recompiler_update_host_reg_counters(r);

  if ((tflags & CPU_RC_TF_CAN_SWAP_DELAY_SLOT) &&
      cpu_recompiler_try_swap_delay_slot(r, cpu_recomp_cf_mips_s(cf), cpu_recomp_cf_mips_t(cf),
                                         (cpu_reg_t)CPU_REG_COUNT)) {
    cf.delay_slot_swapped = 1u;
    if (tflags & CPU_RC_TF_READS_S)  cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rs);
    if (tflags & CPU_RC_TF_READS_T)  cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rt);
    if (tflags & CPU_RC_TF_READS_LO) cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_LO);
    if (tflags & CPU_RC_TF_READS_HI) cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_HI);
  }

  if ((tflags & CPU_RC_TF_READS_S) &&
      ((tflags & CPU_RC_TF_NEEDS_REG_S) || !cf.const_s ||
       ((tflags & CPU_RC_TF_WRITES_D) && rd != (cpu_reg_t)0 && rd == rs))) {
    cf.host_s = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_CPU_REG, rs);
    cf.const_s = 0u;
    cf.valid_host_s = 1u;
  }

  if ((tflags & CPU_RC_TF_READS_T) &&
      ((tflags & (CPU_RC_TF_NEEDS_REG_T | CPU_RC_TF_WRITES_T)) || !cf.const_t ||
       ((tflags & CPU_RC_TF_WRITES_D) && rd != (cpu_reg_t)0 && rd == rt))) {
    cf.host_t = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_CPU_REG, rt);
    cf.const_t = 0u;
    cf.valid_host_t = 1u;
  }

  if (tflags & (CPU_RC_TF_READS_LO | CPU_RC_TF_WRITES_LO)) {
    const u32 mode = ((tflags & CPU_RC_TF_READS_LO)  ? CPU_RC_HR_MODE_READ  : 0u) |
                     ((tflags & CPU_RC_TF_WRITES_LO) ? CPU_RC_HR_MODE_WRITE : 0u);
    cf.host_lo = cpu_recompiler_allocate_host_reg(r, mode, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_LO);
    cf.const_lo = 0u; cf.valid_host_lo = 1u;
  }
  if (tflags & (CPU_RC_TF_READS_HI | CPU_RC_TF_WRITES_HI)) {
    const u32 mode = ((tflags & CPU_RC_TF_READS_HI)  ? CPU_RC_HR_MODE_READ  : 0u) |
                     ((tflags & CPU_RC_TF_WRITES_HI) ? CPU_RC_HR_MODE_WRITE : 0u);
    cf.host_hi = cpu_recompiler_allocate_host_reg(r, mode, CPU_RC_HRT_CPU_REG, (cpu_reg_t)CPU_REG_HI);
    cf.const_hi = 0u; cf.valid_host_hi = 1u;
  }

  const cpu_recomp_host_reg_alloc_type_t write_type =
    ((tflags & CPU_RC_TF_LOAD_DELAY) && CPU_RECOMP_EMULATE_LOAD_DELAYS) ?
      CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE : CPU_RC_HRT_CPU_REG;

  /* Backend hooks beyond the trivials may be NULL.  Below the per-MIPS
   * dispatch call sites guard `func` against NULL and route to
   * compile_fallback. */

  if ((tflags & CPU_RC_TF_CAN_OVERFLOW) && g_settings.cpu_recompiler_memory_exceptions) {
    const u32 tempreg = cpu_recompiler_allocate_temp_host_reg(r, 0u);
    if (tflags & CPU_RC_TF_WRITES_D) {
      cf.host_d = tempreg; cf.valid_host_d = 1u;
    } else if (tflags & CPU_RC_TF_WRITES_T) {
      cf.host_t = tempreg; cf.valid_host_t = 1u;
    }
    if (func != NULL) {
      func(r, cf);
    } else {
      cpu_recompiler_free_host_reg(r, tempreg);
      if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
      return;
    }
    if ((tflags & CPU_RC_TF_WRITES_D) && rd != (cpu_reg_t)0) {
      cpu_recompiler_delete_mips_reg(r, rd, false);
      cpu_recompiler_rename_host_reg(r, tempreg, CPU_RC_HR_MODE_WRITE, write_type, rd);
    } else if ((tflags & CPU_RC_TF_WRITES_T) && rt != (cpu_reg_t)0) {
      cpu_recompiler_delete_mips_reg(r, rt, false);
      cpu_recompiler_rename_host_reg(r, tempreg, CPU_RC_HR_MODE_WRITE, write_type, rt);
    } else {
      cpu_recompiler_free_host_reg(r, tempreg);
    }
  } else {
    if ((tflags & CPU_RC_TF_WRITES_D) && rd != (cpu_reg_t)0) {
      if ((tflags & CPU_RC_TF_READS_S) && cf.valid_host_s &&
          cpu_recompiler_try_rename_mips_reg(r, rd, rs, cf.host_s, (cpu_reg_t)CPU_REG_COUNT)) {
        cf.host_d = cf.host_s;
      } else {
        cf.host_d = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_WRITE, write_type, rd);
      }
      cf.valid_host_d = 1u;
    }
    if ((tflags & CPU_RC_TF_WRITES_T) && rt != (cpu_reg_t)0) {
      if ((tflags & CPU_RC_TF_READS_S) && cf.valid_host_s &&
          cpu_recompiler_try_rename_mips_reg(r, rt, rs, cf.host_s, (cpu_reg_t)CPU_REG_COUNT)) {
        cf.host_t = cf.host_s;
      } else {
        cf.host_t = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_WRITE, write_type, rt);
      }
      cf.valid_host_t = 1u;
    }
    if (func != NULL) {
      func(r, cf);
    } else if (r->v && r->v->compile_fallback) {
      r->v->compile_fallback(r);
    }
  }
}

void cpu_recompiler_compile_loadstore_template(cpu_recompiler_t* r,
                                                cpu_recomp_compile_loadstore_fn_t func,
                                               memory_access_size_t size, bool store, bool sign,
                                               u32 tflags) 
{
  const cpu_reg_t rs = (cpu_reg_t)r->inst->i.rs;
  const cpu_reg_t rt = (cpu_reg_t)r->inst->i.rt;

  if (tflags & CPU_RC_TF_GTE_STALL)
    cpu_recompiler_stall_until_gte_complete(r);

  cpu_recomp_compile_flags_t cf = {.bits = 0u};

  if (tflags & CPU_RC_TF_READS_S) {
    cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rs);
    cf.mips_s = (u32)rs;
  }
  if (tflags & (CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_T)) {
    if (tflags & CPU_RC_TF_READS_T)
      cpu_recompiler_mark_regs_needed(r, CPU_RC_HRT_CPU_REG, rt);
    cf.mips_t = (u32)rt;
  }

  cpu_recompiler_update_host_reg_counters(r);

  /* Constant-address fast path. */
  u32 known_addr_storage;
  const u32* known_addr = NULL;
  cpu_recomp_spec_value_t spec_addr = {.has = false, .val = 0u};
  bool use_fastmem = cpu_code_cache_is_using_fastmem() &&
                     !g_settings.cpu_recompiler_memory_exceptions &&
                     !cpu_recompiler_spec_is_cache_isolated(r) && 
                     !cpu_code_cache_has_previously_faulted_on_pc(r->current_instruction_pc);

  if (cpu_recompiler_has_constant_reg(r, rs)) {
    known_addr_storage = cpu_recompiler_get_constant_reg_u32(r, rs) + cpu_instr_imm_sext32(*r->inst);
    known_addr = &known_addr_storage;
    spec_addr.has = true;
    spec_addr.val = known_addr_storage;
    cf.const_s = 1u;
    if (!bus_can_use_fastmem_for_address(known_addr_storage))
      use_fastmem = false;
  } else {
    spec_addr = cpu_recompiler_spec_exec_loadstore_addr(r);
    if (use_fastmem && spec_addr.has && !bus_can_use_fastmem_for_address(spec_addr.val))
      use_fastmem = false;

    if (CPU_RECOMP_HAS_MEMORY_OPERANDS) {
      cpu_recomp_opt_u32_t hreg = cpu_recompiler_check_host_reg(r, CPU_RC_HR_MODE_READ,
                                                                CPU_RC_HRT_CPU_REG, rs);
      if (hreg.has) { cf.valid_host_s = 1u; cf.host_s = hreg.val; }
    } else {
      cf.host_s = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_CPU_REG, rs);
      cf.valid_host_s = 1u;
    }
  }

  if (tflags & CPU_RC_TF_READS_T) {
    if (cpu_recompiler_has_constant_reg(r, rt)) {
      cf.const_t = 1u;
    } else {
      if (CPU_RECOMP_HAS_MEMORY_OPERANDS) {
        cpu_recomp_opt_u32_t hreg = cpu_recompiler_check_host_reg(r, CPU_RC_HR_MODE_READ,
                                                                  CPU_RC_HRT_CPU_REG, rt);
        if (hreg.has) { cf.valid_host_t = 1u; cf.host_t = hreg.val; }
      } else {
        cf.host_t = cpu_recompiler_allocate_host_reg(r, CPU_RC_HR_MODE_READ, CPU_RC_HRT_CPU_REG, rt);
        cf.valid_host_t = 1u;
      }
    }
  }

  if (!use_fastmem && !store)
    cpu_recompiler_flush(r, CPU_RC_FLUSH_GTE_DONE_CYCLE);

  if (func) {
    func(r, cf, size, sign, use_fastmem, known_addr);
  } else if (r->v && r->v->compile_fallback) {
    /* When a backend's load/store hook is missing, route through the
     * interpreter so the access actually happens (otherwise blocks compile
     * but loads return stale state). */
    r->v->compile_fallback(r);
  }

  /* Self-modifying-code detection: store inside this block's range → truncate. */
  if (store && !r->block_ended && !r->current_instruction_branch_delay_slot && spec_addr.has &&
      cpu_get_segment_for_address(spec_addr.val) != CPU_SEGMENT_KSEG2) {
    const u32 phys = cpu_virtual_to_physical(spec_addr.val);
    if (phys >= cpu_virtual_to_physical(r->compiler_pc) &&
        phys < cpu_virtual_to_physical(r->block->pc + (r->block->size * CPU_INSTRUCTION_SIZE))) {
      WARNING_LOG("Truncating block due to speculative SMC at %08X", phys);
      cpu_recompiler_truncate_block(r);
    }
  }
}

void cpu_recompiler_compile_branch_delay_slot(cpu_recompiler_t* r, bool dirty_pc)
{
  cpu_recompiler_update_load_delay(r);
  cpu_recompiler_clear_host_regs_needed(r);

  r->inst++;
  r->iinfo++;
  r->current_instruction_pc += CPU_INSTRUCTION_SIZE;
  r->current_instruction_branch_delay_slot = true;
  r->compiler_pc += CPU_INSTRUCTION_SIZE;
  r->dirty_pc = dirty_pc;
  r->dirty_instruction_bits = true;

  cpu_recompiler_compile_instruction(r);
  r->current_instruction_branch_delay_slot = false;
  r->delay_slot_branch_was_taken = false;
}

#define CT(CONST_FN, FN, PGXP, TF) \
  cpu_recompiler_compile_template(r, &cpu_recompiler_compile_##CONST_FN, \
                                  r->v ? r->v->compile_##FN : NULL, PGXP, (TF))
#define CTNB(FN, PGXP, TF) \
  cpu_recompiler_compile_template(r, NULL, r->v ? r->v->compile_##FN : NULL, PGXP, (TF))
#define LST(FN, SIZE, STORE, SIGN, TF) \
  cpu_recompiler_compile_loadstore_template(r, r->v ? r->v->compile_##FN : NULL, \
                                            (SIZE), (STORE), (SIGN), (TF))

void cpu_recompiler_compile_instruction(cpu_recompiler_t* r)
{
  r->cycles++;

  if (cpu_is_nop_instruction(*r->inst)) {
    cpu_recompiler_update_load_delay(r);
    return;
  }

  switch ((cpu_instruction_op_t)r->inst->any.op) {
    case CPU_OP_FUNCT: {
      switch ((cpu_instruction_funct_t)r->inst->r.funct) {
        case CPU_FUNCT_SLL:  CT(sll_const, sll, &pgxp_cpu_sll, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sll(r); break;
        case CPU_FUNCT_SRL:  CT(srl_const, srl, &pgxp_cpu_srl, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_srl(r); break;
        case CPU_FUNCT_SRA:  CT(sra_const, sra, &pgxp_cpu_sra, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sra(r); break;
        case CPU_FUNCT_SLLV: CT(sllv_const, sllv, &pgxp_cpu_sllv, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sllv(r); break;
        case CPU_FUNCT_SRLV: CT(srlv_const, srlv, &pgxp_cpu_srlv, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_srlv(r); break;
        case CPU_FUNCT_SRAV: CT(srav_const, srav, &pgxp_cpu_srav, CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_srav(r); break;
        case CPU_FUNCT_JR:   CT(jr_const, jr, NULL, CPU_RC_TF_READS_S); break;
        case CPU_FUNCT_JALR: CT(jalr_const, jalr, NULL, CPU_RC_TF_READS_S | CPU_RC_TF_NO_NOP); cpu_recompiler_spec_exec_jalr(r); break;
        case CPU_FUNCT_SYSCALL: cpu_recompiler_compile_syscall(r); break;
        case CPU_FUNCT_BREAK:   cpu_recompiler_compile_break(r); break;
        case CPU_FUNCT_MFHI: cpu_recompiler_stall_until_muldiv_complete(r);
                             cpu_recompiler_spec_copy_reg(r, (cpu_reg_t)r->inst->r.rd, (cpu_reg_t)CPU_REG_HI);
                             cpu_recompiler_compile_move_reg_template(r, (cpu_reg_t)r->inst->r.rd, (cpu_reg_t)CPU_REG_HI, g_settings.gpu_pgxp_cpu); break;
        case CPU_FUNCT_MTHI: cpu_recompiler_stall_until_muldiv_complete(r);
                             cpu_recompiler_spec_copy_reg(r, (cpu_reg_t)CPU_REG_HI, (cpu_reg_t)r->inst->r.rs);
                             cpu_recompiler_compile_move_reg_template(r, (cpu_reg_t)CPU_REG_HI, (cpu_reg_t)r->inst->r.rs, g_settings.gpu_pgxp_cpu); break;
        case CPU_FUNCT_MFLO: cpu_recompiler_stall_until_muldiv_complete(r);
                             cpu_recompiler_spec_copy_reg(r, (cpu_reg_t)r->inst->r.rd, (cpu_reg_t)CPU_REG_LO);
                             cpu_recompiler_compile_move_reg_template(r, (cpu_reg_t)r->inst->r.rd, (cpu_reg_t)CPU_REG_LO, g_settings.gpu_pgxp_cpu); break;
        case CPU_FUNCT_MTLO: cpu_recompiler_stall_until_muldiv_complete(r);
                             cpu_recompiler_spec_copy_reg(r, (cpu_reg_t)CPU_REG_LO, (cpu_reg_t)r->inst->r.rs);
                             cpu_recompiler_compile_move_reg_template(r, (cpu_reg_t)CPU_REG_LO, (cpu_reg_t)r->inst->r.rs, g_settings.gpu_pgxp_cpu); break;
        case CPU_FUNCT_MULT:  CT(mult_const, mult, &pgxp_cpu_mult,
                                 CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_LO | CPU_RC_TF_WRITES_HI | CPU_RC_TF_COMMUTATIVE);
                              cpu_recompiler_spec_exec_mult(r); break;
        case CPU_FUNCT_MULTU: CT(multu_const, multu, &pgxp_cpu_multu,
                                 CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_LO | CPU_RC_TF_WRITES_HI | CPU_RC_TF_COMMUTATIVE);
                              cpu_recompiler_spec_exec_multu(r); break;
        case CPU_FUNCT_DIV:   CT(div_const, div, &pgxp_cpu_div,
                                 CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_LO | CPU_RC_TF_WRITES_HI);
                              cpu_recompiler_spec_exec_div(r); break;
        case CPU_FUNCT_DIVU:  CT(divu_const, divu, &pgxp_cpu_divu,
                                 CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_WRITES_LO | CPU_RC_TF_WRITES_HI);
                              cpu_recompiler_spec_exec_divu(r); break;
        case CPU_FUNCT_ADD:   CT(add_const, add, &pgxp_cpu_add,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_CAN_OVERFLOW | CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_add(r); break;
        case CPU_FUNCT_ADDU:  CT(addu_const, addu, &pgxp_cpu_add,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_addu(r); break;
        case CPU_FUNCT_SUB:   CT(sub_const, sub, &pgxp_cpu_sub,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_CAN_OVERFLOW | CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_sub(r); break;
        case CPU_FUNCT_SUBU:  CT(subu_const, subu, &pgxp_cpu_sub,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_subu(r); break;
        case CPU_FUNCT_AND:   CT(and_const, and, &pgxp_cpu_and,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_COMMUTATIVE);
                              cpu_recompiler_spec_exec_and(r); break;
        case CPU_FUNCT_OR:    CT(or_const, or, &pgxp_cpu_or,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_or(r); break;
        case CPU_FUNCT_XOR:   CT(xor_const, xor, &pgxp_cpu_xor,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T |
                                 CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_T);
                              cpu_recompiler_spec_exec_xor(r); break;
        case CPU_FUNCT_NOR:   CT(nor_const, nor, &pgxp_cpu_nor,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_COMMUTATIVE);
                              cpu_recompiler_spec_exec_nor(r); break;
        case CPU_FUNCT_SLT:   CT(slt_const, slt, &pgxp_cpu_slt,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_T | CPU_RC_TF_READS_S);
                              cpu_recompiler_spec_exec_slt(r); break;
        case CPU_FUNCT_SLTU:  CT(sltu_const, sltu, &pgxp_cpu_sltu,
                                 CPU_RC_TF_WRITES_D | CPU_RC_TF_READS_T | CPU_RC_TF_READS_S);
                              cpu_recompiler_spec_exec_sltu(r); break;
        default:
          if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
          cpu_recompiler_invalidate_speculative_values(r);
          cpu_recompiler_truncate_block(r);
          break;
      }
    } break;

    case CPU_OP_J:   cpu_recompiler_compile_j(r); break;
    case CPU_OP_JAL: cpu_recompiler_compile_jal(r); cpu_recompiler_spec_exec_jal(r); break;

    /* Branches: use base-layer wrappers (which dispatch into vtable compile_bxx with the right condition). */
    case CPU_OP_B:   cpu_recompiler_compile_template(r, &cpu_recompiler_compile_b_const,    &cpu_recompiler_compile_b,    NULL, CPU_RC_TF_READS_S | CPU_RC_TF_CAN_SWAP_DELAY_SLOT); cpu_recompiler_spec_exec_b(r); break;
    case CPU_OP_BLEZ: cpu_recompiler_compile_template(r, &cpu_recompiler_compile_blez_const, &cpu_recompiler_compile_blez, NULL, CPU_RC_TF_READS_S | CPU_RC_TF_CAN_SWAP_DELAY_SLOT); break;
    case CPU_OP_BGTZ: cpu_recompiler_compile_template(r, &cpu_recompiler_compile_bgtz_const, &cpu_recompiler_compile_bgtz, NULL, CPU_RC_TF_READS_S | CPU_RC_TF_CAN_SWAP_DELAY_SLOT); break;
    case CPU_OP_BEQ: cpu_recompiler_compile_template(r, &cpu_recompiler_compile_beq_const,  &cpu_recompiler_compile_beq,  NULL, CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_CAN_SWAP_DELAY_SLOT); break;
    case CPU_OP_BNE: cpu_recompiler_compile_template(r, &cpu_recompiler_compile_bne_const,  &cpu_recompiler_compile_bne,  NULL, CPU_RC_TF_READS_S | CPU_RC_TF_READS_T | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_CAN_SWAP_DELAY_SLOT); break;

    case CPU_OP_ADDI:  CT(addi_const, addi, &pgxp_cpu_addi,
                          CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_CAN_OVERFLOW | CPU_RC_TF_RENAME_WITH_ZERO_IMM);
                       cpu_recompiler_spec_exec_addi(r); break;
    case CPU_OP_ADDIU: CT(addiu_const, addiu, &pgxp_cpu_addi,
                          CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_IMM);
                       cpu_recompiler_spec_exec_addiu(r); break;
    case CPU_OP_SLTI:  CT(slti_const, slti, &pgxp_cpu_slti, CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S);
                       cpu_recompiler_spec_exec_slti(r); break;
    case CPU_OP_SLTIU: CT(sltiu_const, sltiu, &pgxp_cpu_sltiu, CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S);
                       cpu_recompiler_spec_exec_sltiu(r); break;
    case CPU_OP_ANDI:  CT(andi_const, andi, &pgxp_cpu_andi,
                          CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S | CPU_RC_TF_COMMUTATIVE);
                       cpu_recompiler_spec_exec_andi(r); break;
    case CPU_OP_ORI:   CT(ori_const, ori, &pgxp_cpu_ori,
                          CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_IMM);
                       cpu_recompiler_spec_exec_ori(r); break;
    case CPU_OP_XORI:  CT(xori_const, xori, &pgxp_cpu_xori,
                          CPU_RC_TF_WRITES_T | CPU_RC_TF_READS_S | CPU_RC_TF_COMMUTATIVE | CPU_RC_TF_RENAME_WITH_ZERO_IMM);
                       cpu_recompiler_spec_exec_xori(r); break;
    case CPU_OP_LUI:   cpu_recompiler_compile_lui(r); cpu_recompiler_spec_exec_lui(r); break;

    case CPU_OP_LB:  LST(lxx, MEMORY_ACCESS_SIZE_BYTE,     false, true,  CPU_RC_TF_READS_S | CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lxx(r, MEMORY_ACCESS_SIZE_BYTE, true); break;
    case CPU_OP_LBU: LST(lxx, MEMORY_ACCESS_SIZE_BYTE,     false, false, CPU_RC_TF_READS_S | CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lxx(r, MEMORY_ACCESS_SIZE_BYTE, false); break;
    case CPU_OP_LH:  LST(lxx, MEMORY_ACCESS_SIZE_HALFWORD, false, true,  CPU_RC_TF_READS_S | CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lxx(r, MEMORY_ACCESS_SIZE_HALFWORD, true); break;
    case CPU_OP_LHU: LST(lxx, MEMORY_ACCESS_SIZE_HALFWORD, false, false, CPU_RC_TF_READS_S | CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lxx(r, MEMORY_ACCESS_SIZE_HALFWORD, false); break;
    case CPU_OP_LW:  LST(lxx, MEMORY_ACCESS_SIZE_WORD,     false, false, CPU_RC_TF_READS_S | CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lxx(r, MEMORY_ACCESS_SIZE_WORD, false); break;
    case CPU_OP_LWL: LST(lwx, MEMORY_ACCESS_SIZE_WORD,     false, false, CPU_RC_TF_READS_S | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lwx(r, false); break;
    case CPU_OP_LWR: LST(lwx, MEMORY_ACCESS_SIZE_WORD,     false, false, CPU_RC_TF_READS_S | CPU_RC_TF_LOAD_DELAY); cpu_recompiler_spec_exec_lwx(r, true); break;
    case CPU_OP_SB:  LST(sxx, MEMORY_ACCESS_SIZE_BYTE,     true,  false, CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sxx(r, MEMORY_ACCESS_SIZE_BYTE); break;
    case CPU_OP_SH:  LST(sxx, MEMORY_ACCESS_SIZE_HALFWORD, true,  false, CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sxx(r, MEMORY_ACCESS_SIZE_HALFWORD); break;
    case CPU_OP_SW:  LST(sxx, MEMORY_ACCESS_SIZE_WORD,     true,  false, CPU_RC_TF_READS_S | CPU_RC_TF_READS_T); cpu_recompiler_spec_exec_sxx(r, MEMORY_ACCESS_SIZE_WORD); break;
    case CPU_OP_SWL: LST(swx, MEMORY_ACCESS_SIZE_WORD,     false, false, CPU_RC_TF_READS_S); cpu_recompiler_spec_exec_swx(r, false); break;
    case CPU_OP_SWR: LST(swx, MEMORY_ACCESS_SIZE_WORD,     false, false, CPU_RC_TF_READS_S); cpu_recompiler_spec_exec_swx(r, true); break;

    case CPU_OP_COP0: {
      if (cpu_cop_is_common_instruction(*r->inst)) {
        switch (cpu_cop_common_op(*r->inst)) {
          case CPU_COP_COMMON_MFCN:
            if (r->inst->r.rt != 0u) {
              /* Route through compile_template so the allocator populates
               * cf.host_t (mfc0 asserts on it). const_func=NULL because
               * cop0 reads have no compile-time constant tracking. The
               * backend func is the agnostic mfc0 emitter itself --
               * there's no per-backend hook needed since cop0 reads are
               * just load_host_reg_from_cpu_pointer(host_t, cop0_ptr). */
              cpu_recompiler_compile_template(r, NULL, cpu_recompiler_compile_mfc0,
                                              NULL,
                                              CPU_RC_TF_WRITES_T | CPU_RC_TF_LOAD_DELAY);
            }
            cpu_recompiler_spec_exec_mfc0(r);
            break;
          case CPU_COP_COMMON_MTCN:
            CTNB(mtc0, NULL, CPU_RC_TF_READS_T);
            cpu_recompiler_spec_exec_mtc0(r);
            break;
          default:
            if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
            break;
        }
      } else {
        if (cpu_cop0_op(*r->inst) == CPU_COP0_RFE) {
          CTNB(rfe, NULL, 0u);
          cpu_recompiler_spec_exec_rfe(r);
        } else {
          if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
        }
      }
    } break;

    case CPU_OP_COP2: {
      if (cpu_cop_is_common_instruction(*r->inst)) {
        switch (cpu_cop_common_op(*r->inst)) {
          case CPU_COP_COMMON_MFCN: case CPU_COP_COMMON_CFCN:
            if (r->inst->r.rt != 0u)
              CTNB(mfc2, NULL, CPU_RC_TF_GTE_STALL);
            break;
          case CPU_COP_COMMON_MTCN: case CPU_COP_COMMON_CTCN:
            CTNB(mtc2, &pgxp_cpu_mtc2, CPU_RC_TF_READS_T | CPU_RC_TF_PGXP_WITHOUT_CPU);
            break;
          default:
            if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
            break;
        }
      } else {
        CTNB(cop2, NULL, CPU_RC_TF_GTE_STALL);
      }
    } break;

    case CPU_OP_LWC2: LST(lwc2, MEMORY_ACCESS_SIZE_WORD, false, false, CPU_RC_TF_READS_S); break;
    case CPU_OP_SWC2: LST(swc2, MEMORY_ACCESS_SIZE_WORD, true,  false, CPU_RC_TF_GTE_STALL | CPU_RC_TF_READS_S);
                      cpu_recompiler_spec_exec_swc2(r); break;

    case CPU_OP_COP1: case CPU_OP_COP3:
    case CPU_OP_LWC0: case CPU_OP_LWC1: case CPU_OP_LWC3:
    case CPU_OP_SWC0: case CPU_OP_SWC1: case CPU_OP_SWC3:
      break;

    default:
      if (r->v && r->v->compile_fallback) r->v->compile_fallback(r);
      cpu_recompiler_invalidate_speculative_values(r);
      cpu_recompiler_truncate_block(r);
      break;
  }

  cpu_recompiler_clear_host_regs_needed(r);
  cpu_recompiler_update_load_delay(r);
}

#undef CT
#undef CTNB
#undef LST

const void* cpu_recompiler_compile_block(cpu_recompiler_t* r, cpu_code_cache_block_t* block,
                                         u32* host_code_size, u32* host_far_code_size)
{
  /* Backend handles code-buffer alignment + Reset() (allocates emitter). */
  if (r->v && r->v->reset)
    r->v->reset(r, block, NULL, 0u, NULL, 0u);
  else
    cpu_recompiler_reset_state(r, block, NULL, 0u, NULL, 0u);

  if (r->v && r->v->begin_block)
    r->v->begin_block(r);

  /* BeginBlock() seeds these so the first instruction sees a fresh
   * inst/iinfo pointer + advanced compiler_pc. */
  r->inst = cpu_code_cache_block_instructions(block);
  r->iinfo = cpu_code_cache_block_instructions_info(block);
  r->current_instruction_pc = block->pc;
  r->current_instruction_branch_delay_slot = false;
  r->compiler_pc = block->pc + CPU_INSTRUCTION_SIZE;
  r->dirty_pc = true;
  r->dirty_instruction_bits = true;

  for (;;) {
    cpu_recompiler_compile_instruction(r);

    if (r->block_ended || r->iinfo->is_last_instruction) {
      if (!r->block_ended) {
        const u32 npc = r->compiler_pc;
        if (r->v && r->v->end_block)
          r->v->end_block(r, &npc, false);
      }
      break;
    }

    r->inst++;
    r->iinfo++;
    r->current_instruction_pc += CPU_INSTRUCTION_SIZE;
    r->compiler_pc += CPU_INSTRUCTION_SIZE;
    r->dirty_pc = true;
    r->dirty_instruction_bits = true;
  }

  /* All host regs should be released by end of block. */
  for (u32 i = 0; i < CPU_RECOMP_NUM_HOST_REGS; i++)
    DebugAssert(!cpu_recompiler_is_host_reg_allocated(r, i));
  for (u32 i = 1; i < CPU_REG_COUNT; i++) {
    DebugAssert(!cpu_recompiler_has_dirty_constant_reg(r, (cpu_reg_t)i));
  }

  /* Wipe spec-mem entries before next block. */
  if (r->spec_mem_entries != NULL) {
    for (u32 i = 0; i < r->spec_mem_cap; i++)
      r->spec_mem_entries[i].addr = CPU_RECOMP_SPEC_MEM_EMPTY_KEY;
  }
  r->spec_mem_count = 0u;

  u32 cs = 0u, fcs = 0u;
  const void* code = NULL;
  if (r->v && r->v->end_compile)
    code = r->v->end_compile(r, &cs, &fcs);
  *host_code_size = cs;
  *host_far_code_size = fcs;
  return code;
}

cpu_recomp_gte_reg_lookup_t cpu_recompiler_get_gte_register_pointer(u32 index, bool writing)
{
  cpu_recomp_gte_reg_lookup_t out;
  if (!writing) {
    if (index == 15u) index = 14u; /* SXY3 mirrors SXY2 */
    out.ptr = &g_cpu_state.gte_regs.r32[index];
    out.action = (index == 28u || index == 29u) ? CPU_RC_GTE_REG_CALL_HANDLER : CPU_RC_GTE_REG_DIRECT;
    return out;
  }
  switch (index) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
    case 36: case 44: case 52: case 58: case 59: case 61: case 62:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_SIGN_EXTEND_16;
      return out;
    case 7: case 16: case 17: case 18: case 19:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_ZERO_EXTEND_16;
      return out;
    case 15:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_PUSH_FIFO;
      return out;
    case 28: case 30: case 63:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_CALL_HANDLER;
      return out;
    case 29: case 31:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_IGNORE;
      return out;
    default:
      out.ptr = &g_cpu_state.gte_regs.r32[index];
      out.action = CPU_RC_GTE_REG_DIRECT;
      return out;
  }
}

#define SR(R, REG)    cpu_recompiler_spec_read_reg(R, REG)
#define SW(R, REG, V) cpu_recompiler_spec_write_reg(R, REG, (V))
#define SI(R, REG)    cpu_recompiler_spec_invalidate_reg(R, REG)
#define VAL_T(V)      ((cpu_recomp_spec_value_t){.has = true, .val = (V)})
#define MIS_R(R)      ((cpu_reg_t)(R)->inst->r.rs)
#define MIT_R(R)      ((cpu_reg_t)(R)->inst->r.rt)
#define MID_R(R)      ((cpu_reg_t)(R)->inst->r.rd)
#define MIS_I(R)      ((cpu_reg_t)(R)->inst->i.rs)
#define MIT_I(R)      ((cpu_reg_t)(R)->inst->i.rt)

void cpu_recompiler_spec_exec_b(cpu_recompiler_t* r)
{
  const bool link = (((u8)r->inst->i.rt & 0x1Eu) == 0x10u);
  if (link) SW(r, (cpu_reg_t)CPU_REG_RA, VAL_T(r->compiler_pc));
}

void cpu_recompiler_spec_exec_jal(cpu_recompiler_t* r)
{
  SW(r, (cpu_reg_t)CPU_REG_RA, VAL_T(r->compiler_pc));
}

void cpu_recompiler_spec_exec_jalr(cpu_recompiler_t* r)
{
  SW(r, MID_R(r), VAL_T(r->compiler_pc));
}

#define SE_RT_SHIFT(NAME, EXPR) \
  void cpu_recompiler_spec_exec_##NAME(cpu_recompiler_t* r) {                       \
    cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));                                   \
    if (rt.has) SW(r, MID_R(r), VAL_T(EXPR));                                       \
    else        SI(r, MID_R(r));                                                    \
  }
SE_RT_SHIFT(sll, (rt.val << r->inst->r.shamt))
SE_RT_SHIFT(srl, (rt.val >> r->inst->r.shamt))
SE_RT_SHIFT(sra, ((u32)((s32)rt.val >> r->inst->r.shamt)))
#undef SE_RT_SHIFT

#define SE_RST_SHIFT(NAME, EXPR) \
  void cpu_recompiler_spec_exec_##NAME(cpu_recompiler_t* r) {                       \
    cpu_recomp_spec_value_t rs = SR(r, MIS_R(r));                                   \
    cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));                                   \
    if (rs.has && rt.has) SW(r, MID_R(r), VAL_T(EXPR));                             \
    else                  SI(r, MID_R(r));                                          \
  }
SE_RST_SHIFT(sllv, (rt.val << (rs.val & 0x1Fu)))
SE_RST_SHIFT(srlv, (rt.val >> (rs.val & 0x1Fu)))
SE_RST_SHIFT(srav, ((u32)((s32)rt.val >> (rs.val & 0x1Fu))))
SE_RST_SHIFT(addu, (rs.val + rt.val))
SE_RST_SHIFT(subu, (rs.val - rt.val))
SE_RST_SHIFT(and,  (rs.val & rt.val))
SE_RST_SHIFT(or,   (rs.val | rt.val))
SE_RST_SHIFT(xor,  (rs.val ^ rt.val))
SE_RST_SHIFT(nor,  (~(rs.val | rt.val)))
SE_RST_SHIFT(slt,  (((s32)rs.val < (s32)rt.val) ? 1u : 0u))
SE_RST_SHIFT(sltu, ((rs.val < rt.val) ? 1u : 0u))
#undef SE_RST_SHIFT

void cpu_recompiler_spec_exec_add(cpu_recompiler_t* r) { cpu_recompiler_spec_exec_addu(r); }
void cpu_recompiler_spec_exec_sub(cpu_recompiler_t* r) { cpu_recompiler_spec_exec_subu(r); }

void cpu_recompiler_spec_exec_mult(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_R(r));
  cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));
  if (rs.has && rt.has) {
    const u64 res = (u64)((s64)(s32)rs.val * (s64)(s32)rt.val);
    SW(r, (cpu_reg_t)CPU_REG_HI, VAL_T((u32)(res >> 32)));
    SW(r, (cpu_reg_t)CPU_REG_LO, VAL_T((u32)res));
  } else {
    SI(r, (cpu_reg_t)CPU_REG_HI);
    SI(r, (cpu_reg_t)CPU_REG_LO);
  }
}
void cpu_recompiler_spec_exec_multu(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_R(r));
  cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));
  if (rs.has && rt.has) {
    const u64 res = (u64)rs.val * (u64)rt.val;
    SW(r, (cpu_reg_t)CPU_REG_HI, VAL_T((u32)(res >> 32)));
    SW(r, (cpu_reg_t)CPU_REG_LO, VAL_T((u32)res));
  } else {
    SI(r, (cpu_reg_t)CPU_REG_HI);
    SI(r, (cpu_reg_t)CPU_REG_LO);
  }
}
void cpu_recompiler_spec_exec_div(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_R(r));
  cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));
  if (rs.has && rt.has) {
    u32 lo, hi;
    cpu_recompiler_mips_signed_divide((s32)rs.val, (s32)rt.val, &lo, &hi);
    SW(r, (cpu_reg_t)CPU_REG_HI, VAL_T(hi));
    SW(r, (cpu_reg_t)CPU_REG_LO, VAL_T(lo));
  } else {
    SI(r, (cpu_reg_t)CPU_REG_HI); SI(r, (cpu_reg_t)CPU_REG_LO);
  }
}
void cpu_recompiler_spec_exec_divu(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_R(r));
  cpu_recomp_spec_value_t rt = SR(r, MIT_R(r));
  if (rs.has && rt.has) {
    u32 lo, hi;
    cpu_recompiler_mips_unsigned_divide(rs.val, rt.val, &lo, &hi);
    SW(r, (cpu_reg_t)CPU_REG_HI, VAL_T(hi));
    SW(r, (cpu_reg_t)CPU_REG_LO, VAL_T(lo));
  } else {
    SI(r, (cpu_reg_t)CPU_REG_HI); SI(r, (cpu_reg_t)CPU_REG_LO);
  }
}

void cpu_recompiler_spec_exec_addi(cpu_recompiler_t* r) { cpu_recompiler_spec_exec_addiu(r); }
void cpu_recompiler_spec_exec_addiu(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has) SW(r, MIT_I(r), VAL_T(rs.val + cpu_instr_imm_sext32(*r->inst)));
  else        SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_slti(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has)
    SW(r, MIT_I(r), VAL_T(((s32)rs.val < (s32)cpu_instr_imm_sext32(*r->inst)) ? 1u : 0u));
  else
    SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_sltiu(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has)
    SW(r, MIT_I(r), VAL_T((rs.val < cpu_instr_imm_sext32(*r->inst)) ? 1u : 0u));
  else
    SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_andi(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has) SW(r, MIT_I(r), VAL_T(rs.val & cpu_instr_imm_zext32(*r->inst)));
  else        SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_ori(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has) SW(r, MIT_I(r), VAL_T(rs.val | cpu_instr_imm_zext32(*r->inst)));
  else        SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_xori(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has) SW(r, MIT_I(r), VAL_T(rs.val ^ cpu_instr_imm_zext32(*r->inst)));
  else        SI(r, MIT_I(r));
}
void cpu_recompiler_spec_exec_lui(cpu_recompiler_t* r)
{
  SW(r, MIT_I(r), VAL_T(cpu_instr_imm_zext32(*r->inst) << 16));
}

cpu_recomp_spec_value_t cpu_recompiler_spec_exec_loadstore_addr(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t rs = SR(r, MIS_I(r));
  if (rs.has) { rs.val = rs.val + cpu_instr_imm_sext32(*r->inst); }
  return rs;
}

void cpu_recompiler_spec_exec_lxx(cpu_recompiler_t* r, memory_access_size_t size, bool sign)
{
  cpu_recomp_spec_value_t addr = cpu_recompiler_spec_exec_loadstore_addr(r);
  cpu_recomp_spec_value_t val;
  if (!addr.has || !(val = cpu_recompiler_spec_read_mem(r, addr.val)).has) {
    SI(r, MIT_I(r));
    return;
  }
  switch (size) {
    case MEMORY_ACCESS_SIZE_BYTE:
      val.val = sign ? (u32)(s32)(s8)(u8)val.val : (u32)(u8)val.val; break;
    case MEMORY_ACCESS_SIZE_HALFWORD:
      val.val = sign ? (u32)(s32)(s16)(u16)val.val : (u32)(u16)val.val; break;
    case MEMORY_ACCESS_SIZE_WORD: break;
    default: break;
  }
  SW(r, MIT_R(r), val);
}

void cpu_recompiler_spec_exec_lwx(cpu_recompiler_t* r, bool lwr) { (void)lwr; SI(r, MIT_I(r)); }

void cpu_recompiler_spec_exec_sxx(cpu_recompiler_t* r, memory_access_size_t size)
{
  cpu_recomp_spec_value_t addr = cpu_recompiler_spec_exec_loadstore_addr(r);
  if (!addr.has) return;
  cpu_recomp_spec_value_t rt = SR(r, MIT_I(r));
  if (rt.has) {
    switch (size) {
      case MEMORY_ACCESS_SIZE_BYTE:     rt.val = (u32)(u8)rt.val;  break;
      case MEMORY_ACCESS_SIZE_HALFWORD: rt.val = (u32)(u16)rt.val; break;
      case MEMORY_ACCESS_SIZE_WORD: break;
      default: break;
    }
  }
  cpu_recompiler_spec_write_mem(r, addr.val, rt);
}

void cpu_recompiler_spec_exec_swx(cpu_recompiler_t* r, bool swr)
{
  (void)swr;
  cpu_recomp_spec_value_t addr = cpu_recompiler_spec_exec_loadstore_addr(r);
  if (addr.has) cpu_recompiler_spec_invalidate_mem(r, addr.val & ~3u);
}

void cpu_recompiler_spec_exec_swc2(cpu_recompiler_t* r)
{
  cpu_recomp_spec_value_t addr = cpu_recompiler_spec_exec_loadstore_addr(r);
  if (addr.has) cpu_recompiler_spec_invalidate_mem(r, addr.val);
}

void cpu_recompiler_spec_exec_mfc0(cpu_recompiler_t* r)
{
  const cpu_cop0_reg_t rd = (cpu_cop0_reg_t)r->inst->r.rd;
  if (rd != CPU_COP0_REG_SR) { SI(r, MIT_R(r)); return; }
  SW(r, MIT_R(r), r->spec_cop0_sr);
}

void cpu_recompiler_spec_exec_mtc0(cpu_recompiler_t* r)
{
  const cpu_cop0_reg_t rd = (cpu_cop0_reg_t)r->inst->r.rd;
  if (rd != CPU_COP0_REG_SR || !r->spec_cop0_sr.has) return;

  cpu_recomp_spec_value_t v = SR(r, MIT_R(r));
  if (v.has) {
    const u32 mask = CPU_COP0_SR_WRITE_MASK;
    v.val = (r->spec_cop0_sr.val & mask) | (v.val & mask);
  }
  r->spec_cop0_sr = v;
}

void cpu_recompiler_spec_exec_rfe(cpu_recompiler_t* r)
{
  if (!r->spec_cop0_sr.has) return;
  const u32 v = r->spec_cop0_sr.val;
  r->spec_cop0_sr.val = (v & 0x30u) | ((v & 0x3Fu) >> 2);
}

#undef SR
#undef SW
#undef SI
#undef VAL_T
#undef MIS_R
#undef MIT_R
#undef MID_R
#undef MIS_I
#undef MIT_I

#undef MD
#undef MS
#undef MT
#undef KU32
#undef KS32
#undef HASK
#undef HASKV
#undef SETK
