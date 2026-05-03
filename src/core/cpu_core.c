/*
 * R3000A interpreter.  Single dispatcher is a giant switch on
 * InstructionOp / InstructionFunct - laid out so each PS1 quirk (load
 * delay, branch delay, divide-by-zero, COP0 mode-stack) stays auditable
 * against Sony's docs.
 *
 * Out of scope here:
 *  - PGXP (cpu_pgxp deferred) - replaced with no-ops via
 *    CUPID_PGXP_ENABLED guard which is never defined here.
 *  - Cached interpreter / dynarec interop.  Always interpreter, always
 *    fetches from icache.
 *  - Breakpoint / debugger callbacks - debug_dispatcher flag stays for
 *    state-struct ABI but never lights up.
 *  - fastmem; pointer kept on the state struct but unused.
 */

#include "cpu_core.h"

#include "bus.h"
#include "cpu_code_cache.h"
#include "cpu_core_private.h"
#include "cpu_diff_runner.h"
#include "cpu_pc_trigger.h"
#include "cpu_types.h"
#include "gte.h"
#include "interrupt_controller.h"
#include "settings.h"
#include "timing_event.h"
#include "types.h"

#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/types.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_CHANNEL(CPU);

extern u32  gte_read_register(u32 index);
extern void gte_write_register(u32 index, u32 value);
extern void pgxp_reset(void);
void        gte_execute_instruction(cpu_instruction_t inst); /* defined in gte.c */

cpu_state_t g_cpu_state;

/* Scoped to this translation unit: dispatcher locals that survive across
 * a longjmp out of cpu_execute (so we don't clobber caller stack). */
typedef struct {
  cpu_execution_mode_t current_execution_mode;
  jmp_buf exit_jmp_buf;
} cpu_locals_t;

static cpu_locals_t s_locals;

static void cpu_update_load_delay(void);
static void cpu_branch(u32 target);
static void cpu_flush_load_delay(void);
static void cpu_flush_pipeline(void);

static u32  cpu_get_exception_vector(void);
static void cpu_raise_exception_with_vector(u32 cause_bits, u32 epc, u32 vector);
static void cpu_raise_data_bus_exception(void);

static u32  cpu_read_reg(cpu_reg_t rs);
static void cpu_write_reg(cpu_reg_t rd, u32 value);
static void cpu_write_reg_delayed(cpu_reg_t rd, u32 value);

static void cpu_handle_write_syscall(void);
static void cpu_handle_putc_syscall(void);
static void cpu_handle_puts_syscall(void);

static void cpu_check_for_execution_mode_change(void);
NORETURN static void cpu_execute_interpreter(void);
NORETURN static void cpu_execute_impl(void);

#ifdef CUPID_DEBUG_RECOMP_TRACE
__attribute__((visibility("default")))
void cpu_recomp_trace_block_entry(u32 pc);
void cpu_recomp_trace_block_entry(u32 pc)
{
  static u32 _n = 0;
  if (_n++ < 200u || (_n & 0xFFFu) == 0u)
    ERROR_LOG("recomp enter pc=%08X t2=%08X t3=%08X v0=%08X v1=%08X",
              pc,
              g_cpu_state.regs.r[10],  /* $t2 */
              g_cpu_state.regs.r[11],  /* $t3 */
              g_cpu_state.regs.r[2],   /* $v0 */
              g_cpu_state.regs.r[3]);  /* $v1 */
}
#endif

/* Re-entered by cpu_code_cache.c when the recompiler driver falls back to
 * interpretation (skeleton, and any backend's Compile_Fallback path).
 * Hidden from the rest of the code base. */
NORETURN void cpu_run_interpreter_loop(void);

static void cpu_execute_instruction(void);

static bool cpu_fetch_instruction(void);
static bool cpu_fetch_instruction_for_interp_fallback(void);

static bool cpu_do_instruction_read(physical_memory_address_t address, u32* data,
                                    bool add_ticks, bool icache_read, u32 word_count,
                                    bool raise_exceptions);

static bool cpu_do_safe_memory_access_read(virtual_memory_address_t address, memory_access_size_t size, u32* value);
static bool cpu_do_safe_memory_access_write(virtual_memory_address_t address, memory_access_size_t size, u32 value);
static bool cpu_do_alignment_check(memory_access_type_t type, memory_access_size_t size,
                                   virtual_memory_address_t address);

static bool cpu_read_memory_byte(virtual_memory_address_t addr, u8* value);
static bool cpu_read_memory_halfword(virtual_memory_address_t addr, u16* value);
static bool cpu_read_memory_word(virtual_memory_address_t addr, u32* value);
static bool cpu_write_memory_byte(virtual_memory_address_t addr, u32 value);
static bool cpu_write_memory_halfword(virtual_memory_address_t addr, u32 value);
static bool cpu_write_memory_word(virtual_memory_address_t addr, u32 value);

static u32  cpu_read_icache(virtual_memory_address_t address);

void cpu_initialize(void)
{
  /* PRID = 0x00000002 per nocash spec. */
  g_cpu_state.cop0_regs.PRID = 0x00000002u;

  s_locals.current_execution_mode = CPU_EXECUTION_MODE_INTERPRETER;
  g_cpu_state.using_debug_dispatcher = false;
  g_cpu_state.using_interpreter = true;

  cpu_update_memory_pointers();
  cpu_update_debug_dispatcher_flag();
}

void cpu_shutdown(void)
{
  /* Nothing to release in the interpreter-only build. */
}

void cpu_reset(void)
{
  g_cpu_state.exception_raised = false;
  g_cpu_state.bus_error = false;

  memset(&g_cpu_state.regs, 0, sizeof(g_cpu_state.regs));

  g_cpu_state.cop0_regs.BPC = 0u;
  g_cpu_state.cop0_regs.BDA = 0u;
  g_cpu_state.cop0_regs.TAR = 0u;
  g_cpu_state.cop0_regs.BadVaddr = 0u;
  g_cpu_state.cop0_regs.BDAM = 0u;
  g_cpu_state.cop0_regs.BPCM = 0u;
  g_cpu_state.cop0_regs.EPC = 0u;
  g_cpu_state.cop0_regs.dcic.bits = 0u;
  g_cpu_state.cop0_regs.sr.bits = 0u;
  g_cpu_state.cop0_regs.cause.bits = 0u;

  cpu_clear_icache();
  cpu_update_memory_pointers();
  cpu_update_debug_dispatcher_flag();

  /* Reset the GTE coprocessor here (inside cpu_reset).
   * system_internal_reset never calls gte_reset on its own, so without this
   * the GTE registers carry stale values across a warm reset. */
  gte_reset();

  /* PGXP is reset in system_internal_reset when the setting is on; reset it
   * here too so the order is stable.  Skipping it here is harmless because
   * system_internal_reset still does it. */
  if (g_settings.gpu_pgxp_enable)
    pgxp_reset();

  /* Seat both pc and npc on the reset vector before flushing the pipeline.
   * cpu_set_pc only writes npc, which is fine on first boot (pc is zero-init)
   * but on a warm reset pc still holds the pre-reset address and so does
   * current_instruction_pc after the flush; the first BIOS instruction
   * then executes with a stale PC context, which manifests as a hang. */
  g_cpu_state.pc  = CPU_RESET_VECTOR;
  g_cpu_state.npc = CPU_RESET_VECTOR;
  cpu_set_pc(CPU_RESET_VECTOR);

  g_cpu_state.downcount = 0u;
  g_cpu_state.pending_ticks = 0u;
  g_cpu_state.gte_completion_tick = 0u;
  g_cpu_state.muldiv_completion_tick = 0u;
}

bool cpu_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_u32(sw, &g_cpu_state.pending_ticks);
  state_wrapper_do_u32(sw, &g_cpu_state.downcount);
  state_wrapper_do_u32(sw, &g_cpu_state.gte_completion_tick);
  state_wrapper_do_u32(sw, &g_cpu_state.muldiv_completion_tick);

  state_wrapper_do_array(sw, g_cpu_state.regs.r, sizeof(u32), CPU_REG_COUNT);
  state_wrapper_do_u32(sw, &g_cpu_state.pc);
  state_wrapper_do_u32(sw, &g_cpu_state.npc);

  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.BPC);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.BDA);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.TAR);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.BadVaddr);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.BDAM);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.BPCM);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.EPC);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.PRID);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.sr.bits);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.cause.bits);
  state_wrapper_do_u32(sw, &g_cpu_state.cop0_regs.dcic.bits);

  state_wrapper_do_u32(sw, &g_cpu_state.next_instruction.bits);
  state_wrapper_do_u32(sw, &g_cpu_state.current_instruction.bits);
  state_wrapper_do_u32(sw, &g_cpu_state.current_instruction_pc);
  state_wrapper_do_bool(sw, &g_cpu_state.current_instruction_in_branch_delay_slot);
  state_wrapper_do_bool(sw, &g_cpu_state.current_instruction_was_branch_taken);
  state_wrapper_do_bool(sw, &g_cpu_state.next_instruction_is_branch_delay_slot);
  state_wrapper_do_bool(sw, &g_cpu_state.branch_was_taken);
  state_wrapper_do_bool(sw, &g_cpu_state.exception_raised);
  state_wrapper_do_bool(sw, &g_cpu_state.bus_error);

  {
    u8 ld = (u8)g_cpu_state.load_delay_reg;
    u8 nld = (u8)g_cpu_state.next_load_delay_reg;
    state_wrapper_do_u8(sw, &ld);
    state_wrapper_do_u32(sw, &g_cpu_state.load_delay_value);
    state_wrapper_do_u8(sw, &nld);
    state_wrapper_do_u32(sw, &g_cpu_state.next_load_delay_value);
    g_cpu_state.load_delay_reg      = (cpu_reg_t)ld;
    g_cpu_state.next_load_delay_reg = (cpu_reg_t)nld;
  }

  state_wrapper_do_u32(sw, &g_cpu_state.cache_control.bits);
  state_wrapper_do_bytes(sw, g_cpu_state.scratchpad, sizeof(g_cpu_state.scratchpad));

  if (!gte_do_state(sw)) return false;

  state_wrapper_do_array(sw, g_cpu_state.icache_tags, sizeof(u32), CPU_ICACHE_LINES);
  state_wrapper_do_array(sw, g_cpu_state.icache_data, sizeof(u32),
                         CPU_ICACHE_LINES * CPU_ICACHE_WORDS_PER_LINE);

  state_wrapper_do_bool(sw, &g_cpu_state.using_interpreter);

  if (state_wrapper_is_reading(sw)) {
    s_locals.current_execution_mode =
      g_cpu_state.using_interpreter
        ? CPU_EXECUTION_MODE_INTERPRETER
        : ((g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_INTERPRETER)
             ? CPU_EXECUTION_MODE_CACHED_INTERPRETER 
             : g_settings.cpu_execution_mode);
    g_cpu_state.gte_completion_tick = 0u;
    g_cpu_state.muldiv_completion_tick = 0u;
    cpu_update_memory_pointers();
    cpu_update_debug_dispatcher_flag();
  }

  return true;
}

cpu_execution_mode_t cpu_get_current_execution_mode(void)
{
  return s_locals.current_execution_mode;
}

bool cpu_update_debug_dispatcher_flag(void)
{
  /* No breakpoints, no trace; debug dispatcher is never enabled. */
  if (!g_cpu_state.using_debug_dispatcher)
    return false;
  g_cpu_state.using_debug_dispatcher = false;
  return true;
}

void cpu_update_memory_pointers(void)
{
  g_cpu_state.memory_handlers = bus_get_memory_handlers(g_cpu_state.cop0_regs.sr.Isc != 0u,
                                                        g_cpu_state.cop0_regs.sr.Swc != 0u);
  g_cpu_state.fastmem_base = bus_get_fastmem_base(g_cpu_state.cop0_regs.sr.Isc != 0u);
}

void cpu_set_pc(u32 new_pc)
{
  DebugAssert(IsAlignedPow2(new_pc, 4u));
  g_cpu_state.npc = new_pc;
  cpu_flush_pipeline();
}

static void cpu_branch(u32 target)
{
  if (!IsAlignedPow2(target, 4u)) {
    /* BadVaddr/EPC use the fetch address, not the instruction we were about
     * to execute when we tried to follow this branch. */
    g_cpu_state.cop0_regs.BadVaddr = target;
    cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_ADEL, false, false, 0u), target);
    return;
  }

  g_cpu_state.npc = target;
  g_cpu_state.branch_was_taken = true;
}

static u32 cpu_get_exception_vector(void)
{
  /* SR.BEV picks between the BIOS ROM vectors (KSEG1) and the standard ones. */
  const u32 base = g_cpu_state.cop0_regs.sr.BEV ? 0xBFC00100u : 0x80000000u;
  return base | 0x00000080u;
}

static void cpu_raise_exception_with_vector(u32 cause_bits, u32 epc, u32 vector)
{
  g_cpu_state.cop0_regs.EPC = epc;
  g_cpu_state.cop0_regs.cause.bits = (g_cpu_state.cop0_regs.cause.bits & ~CPU_COP0_CAUSE_EXCEPTION_WRITE_MASK) |
                                     (cause_bits & CPU_COP0_CAUSE_EXCEPTION_WRITE_MASK);

  if (g_cpu_state.cop0_regs.cause.BD) {
    /* TAR holds the destination of the branch we were stepping into when
     * the trap fired in the delay slot.  EPC backs up so RFE re-executes
     * the branch itself. */
    g_cpu_state.cop0_regs.EPC -= 4u;
    g_cpu_state.cop0_regs.TAR = g_cpu_state.pc;
  }

  /* Push the SR mode stack two bits left.  Implicitly switches to kernel
   * mode (KUc=0) and disables interrupts (IEc=0). */
  {
    const u32 old_sr = g_cpu_state.cop0_regs.sr.bits;
    const u32 mode_bits = old_sr & 0x3Fu;
    const u32 new_mode = (mode_bits << 2) & 0x3Fu;
    g_cpu_state.cop0_regs.sr.bits = (old_sr & ~0x3Fu) | new_mode;
  }

  /* Discard the prefetched instruction and arm the vector. */
  g_cpu_state.npc = vector;
  g_cpu_state.exception_raised = true;
  cpu_flush_pipeline();
}

void cpu_raise_exception_bits(u32 cause_bits, u32 epc)
{
  cpu_raise_exception_with_vector(cause_bits, epc, cpu_get_exception_vector());
}

void cpu_raise_exception(cpu_exception_t excode)
{
  cpu_raise_exception_with_vector(
    cpu_cop0_cause_make(excode, g_cpu_state.current_instruction_in_branch_delay_slot,
                        g_cpu_state.current_instruction_was_branch_taken,
                        (u8)g_cpu_state.current_instruction.cop.cop_n), 
    g_cpu_state.current_instruction_pc, cpu_get_exception_vector());
}

void cpu_raise_break_exception(u32 cause_bits, u32 epc, u32 instruction_bits)
{
  /* PCDrv HLE not ported; just take the normal exception. */
  (void)instruction_bits;
  WARNING_LOG("PCDrv is not enabled, break HLE will not be executed.");
  cpu_raise_exception_with_vector(cause_bits, epc, cpu_get_exception_vector());
}

void cpu_set_irq_request(bool state)
{
  /* INTC routes everything to CAUSE.Ip[10] (HW_IRQ).  Only that bit moves. */
  enum { IRQ_BIT = (1u << 10) };
  const u32 old_cause = g_cpu_state.cop0_regs.cause.bits;
  g_cpu_state.cop0_regs.cause.bits = (g_cpu_state.cop0_regs.cause.bits & ~(u32)IRQ_BIT) | (state ? (u32)IRQ_BIT : 0u);
  if ((old_cause ^ g_cpu_state.cop0_regs.cause.bits) && state)
    cpu_check_for_pending_interrupt();
}

static void cpu_update_load_delay(void)
{
  g_cpu_state.regs.r[(u8)g_cpu_state.load_delay_reg] = g_cpu_state.load_delay_value;
  g_cpu_state.load_delay_reg = g_cpu_state.next_load_delay_reg;
  g_cpu_state.load_delay_value = g_cpu_state.next_load_delay_value;
  g_cpu_state.next_load_delay_reg = CPU_REG_COUNT;
}

static void cpu_flush_load_delay(void)
{
  g_cpu_state.next_load_delay_reg = CPU_REG_COUNT;
  g_cpu_state.regs.r[(u8)g_cpu_state.load_delay_reg] = g_cpu_state.load_delay_value;
  g_cpu_state.load_delay_reg = CPU_REG_COUNT;
}

static void cpu_flush_pipeline(void)
{
  cpu_flush_load_delay();

  g_cpu_state.branch_was_taken = false;
  g_cpu_state.next_instruction_is_branch_delay_slot = false;
  g_cpu_state.current_instruction_pc = g_cpu_state.pc;

  cpu_fetch_instruction();

  g_cpu_state.current_instruction.bits = g_cpu_state.next_instruction.bits;
  g_cpu_state.current_instruction_in_branch_delay_slot = false;
  g_cpu_state.current_instruction_was_branch_taken = false;
}

ALWAYS_INLINE static u32 cpu_read_reg(cpu_reg_t rs)
{
  return g_cpu_state.regs.r[(u8)rs];
}

ALWAYS_INLINE static void cpu_write_reg(cpu_reg_t rd, u32 value)
{
  g_cpu_state.regs.r[(u8)rd] = value;
  /* If we're overwriting a register that has a pending load, the load is
   * cancelled (the instruction we just executed already produced a value). */
  g_cpu_state.load_delay_reg = (rd == g_cpu_state.load_delay_reg) ? CPU_REG_COUNT : g_cpu_state.load_delay_reg;
  g_cpu_state.regs.named.zero = 0u;
}

static void cpu_write_reg_delayed(cpu_reg_t rd, u32 value)
{
  if (rd == CPU_REG_ZERO)
    return;

  /* Load-into-load: a pending load to the same register is dropped (the
   * second load wins; the first one's value is never observable). */
  if (g_cpu_state.load_delay_reg == rd)
    g_cpu_state.load_delay_reg = CPU_REG_COUNT;

  g_cpu_state.next_load_delay_reg = rd;
  g_cpu_state.next_load_delay_value = value;
}

static void cpu_handle_write_syscall(void)
{
  const cpu_registers_t* regs = &g_cpu_state.regs;
  if (regs->named.a0 != 1u) /* fd != stdout */
    return;

  u32 addr = regs->named.a1;
  const u32 count = regs->named.a2;
  for (u32 i = 0; i < count; i++) {
    u8 value;
    if (!cpu_safe_read_memory_byte(addr++, &value) || value == 0u)
      break;
    bus_add_tty_character((char)value);
  }
}

static void cpu_handle_putc_syscall(void)
{
  const cpu_registers_t* regs = &g_cpu_state.regs;
  if (regs->named.a0 != 0u)
    bus_add_tty_character((char)regs->named.a0);
}

static void cpu_handle_puts_syscall(void)
{
  const cpu_registers_t* regs = &g_cpu_state.regs;
  u32 addr = regs->named.a0;
  for (u32 i = 0; i < 1024u; i++) {
    u8 value;
    if (!cpu_safe_read_memory_byte(addr++, &value) || value == 0u)
      break;
    bus_add_tty_character((char)value);
  }
}

void cpu_handle_a0_syscall(void)
{
  const u32 call = g_cpu_state.regs.named.t1;
  if (call == 0x03u)
    cpu_handle_write_syscall();
  else if (call == 0x09u || call == 0x3cu)
    cpu_handle_putc_syscall();
  else if (call == 0x3eu)
    cpu_handle_puts_syscall();
}

void cpu_handle_b0_syscall(void)
{
  const u32 call = g_cpu_state.regs.named.t1;
  if (call == 0x35u)
    cpu_handle_write_syscall();
  else if (call == 0x3bu || call == 0x3du)
    cpu_handle_putc_syscall();
  else if (call == 0x3fu)
    cpu_handle_puts_syscall();
}

ALWAYS_INLINE static bool cpu_add_overflow(u32 old_value, u32 add_value, u32* new_value)
{
  return __builtin_add_overflow((s32)old_value, (s32)add_value, (s32*)new_value);
}

ALWAYS_INLINE static bool cpu_sub_overflow(u32 old_value, u32 sub_value, u32* new_value)
{
  return __builtin_sub_overflow((s32)old_value, (s32)sub_value, (s32*)new_value);
}

static void cpu_execute_instruction(void)
{
restart_instruction:;
  const cpu_instruction_t inst = g_cpu_state.current_instruction;

  /* Fast-path nops without entering the switch. */
  if (inst.bits == 0u)
    return;

  switch ((cpu_instruction_op_t)inst.any.op) {
    case CPU_OP_FUNCT: {
      switch ((cpu_instruction_funct_t)inst.r.funct) {
        case CPU_FUNCT_SLL: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rt_val << inst.r.shamt);
        } break;

        case CPU_FUNCT_SRL: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rt_val >> inst.r.shamt);
        } break;

        case CPU_FUNCT_SRA: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, (u32)((s32)rt_val >> inst.r.shamt));
        } break;

        case CPU_FUNCT_SLLV: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          const u32 shamt = cpu_read_reg((cpu_reg_t)inst.r.rs) & 0x1Fu;
          cpu_write_reg((cpu_reg_t)inst.r.rd, rt_val << shamt);
        } break;

        case CPU_FUNCT_SRLV: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          const u32 shamt = cpu_read_reg((cpu_reg_t)inst.r.rs) & 0x1Fu;
          cpu_write_reg((cpu_reg_t)inst.r.rd, rt_val >> shamt);
        } break;

        case CPU_FUNCT_SRAV: {
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          const u32 shamt = cpu_read_reg((cpu_reg_t)inst.r.rs) & 0x1Fu;
          cpu_write_reg((cpu_reg_t)inst.r.rd, (u32)((s32)rt_val >> shamt));
        } break;

        case CPU_FUNCT_AND: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rs_val & rt_val);
        } break;

        case CPU_FUNCT_OR: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rs_val | rt_val);
        } break;

        case CPU_FUNCT_XOR: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rs_val ^ rt_val);
        } break;

        case CPU_FUNCT_NOR: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, ~(rs_val | rt_val));
        } break;

        case CPU_FUNCT_ADD: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          u32 rd_val;
          if (cpu_add_overflow(rs_val, rt_val, &rd_val)) {
            cpu_raise_exception(CPU_EXCEPTION_OV);
            return;
          }
          cpu_write_reg((cpu_reg_t)inst.r.rd, rd_val);
        } break;

        case CPU_FUNCT_ADDU: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rs_val + rt_val);
        } break;

        case CPU_FUNCT_SUB: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          u32 rd_val;
          if (cpu_sub_overflow(rs_val, rt_val, &rd_val)) {
            cpu_raise_exception(CPU_EXCEPTION_OV);
            return;
          }
          cpu_write_reg((cpu_reg_t)inst.r.rd, rd_val);
        } break;

        case CPU_FUNCT_SUBU: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, rs_val - rt_val);
        } break;

        case CPU_FUNCT_SLT: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, (u32)((s32)rs_val < (s32)rt_val));
        } break;

        case CPU_FUNCT_SLTU: {
          const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rt_val = cpu_read_reg((cpu_reg_t)inst.r.rt);
          cpu_write_reg((cpu_reg_t)inst.r.rd, (u32)(rs_val < rt_val));
        } break;

        case CPU_FUNCT_MFHI: {
          cpu_write_reg((cpu_reg_t)inst.r.rd, g_cpu_state.regs.named.hi);
          cpu_stall_until_muldiv_complete();
        } break;

        case CPU_FUNCT_MTHI: {
          g_cpu_state.regs.named.hi = cpu_read_reg((cpu_reg_t)inst.r.rs);
          cpu_stall_until_muldiv_complete();
        } break;

        case CPU_FUNCT_MFLO: {
          cpu_write_reg((cpu_reg_t)inst.r.rd, g_cpu_state.regs.named.lo);
          cpu_stall_until_muldiv_complete();
        } break;

        case CPU_FUNCT_MTLO: {
          g_cpu_state.regs.named.lo = cpu_read_reg((cpu_reg_t)inst.r.rs);
          cpu_stall_until_muldiv_complete();
        } break;

        case CPU_FUNCT_MULT: {
          const u32 lhs = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rhs = cpu_read_reg((cpu_reg_t)inst.r.rt);
          const s64 result = (s64)(s32)lhs * (s64)(s32)rhs;
          g_cpu_state.regs.named.hi = (u32)((u64)result >> 32);
          g_cpu_state.regs.named.lo = (u32)result;
          cpu_stall_until_muldiv_complete();
          cpu_add_muldiv_ticks(cpu_get_mult_ticks_signed((s32)lhs));
        } break;

        case CPU_FUNCT_MULTU: {
          const u32 lhs = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 rhs = cpu_read_reg((cpu_reg_t)inst.r.rt);
          const u64 result = (u64)lhs * (u64)rhs;
          g_cpu_state.regs.named.hi = (u32)(result >> 32);
          g_cpu_state.regs.named.lo = (u32)result;
          cpu_stall_until_muldiv_complete();
          cpu_add_muldiv_ticks(cpu_get_mult_ticks_unsigned(lhs));
        } break;

        case CPU_FUNCT_DIV: {
          const s32 num = (s32)cpu_read_reg((cpu_reg_t)inst.r.rs);
          const s32 denom = (s32)cpu_read_reg((cpu_reg_t)inst.r.rt);

          if (denom == 0) {
            /* Divide by zero: lo = -1 (positive numerator) or +1 (negative);
             * hi = numerator.  This matches PS1 hardware, not C semantics. */
            g_cpu_state.regs.named.lo = (num >= 0) ? 0xFFFFFFFFu : 1u;
            g_cpu_state.regs.named.hi = (u32)num;
          } else if ((u32)num == 0x80000000u && denom == -1) {
            /* INT_MIN / -1 is unrepresentable; the hardware returns
             * lo = INT_MIN, hi = 0 instead of trapping. */
            g_cpu_state.regs.named.lo = 0x80000000u;
            g_cpu_state.regs.named.hi = 0u;
          } else {
            g_cpu_state.regs.named.lo = (u32)(num / denom);
            g_cpu_state.regs.named.hi = (u32)(num % denom);
          }
          cpu_stall_until_muldiv_complete();
          cpu_add_muldiv_ticks(cpu_get_div_ticks());
        } break;

        case CPU_FUNCT_DIVU: {
          const u32 num = cpu_read_reg((cpu_reg_t)inst.r.rs);
          const u32 denom = cpu_read_reg((cpu_reg_t)inst.r.rt);
          if (denom == 0u) {
            g_cpu_state.regs.named.lo = 0xFFFFFFFFu;
            g_cpu_state.regs.named.hi = num;
          } else {
            g_cpu_state.regs.named.lo = num / denom;
            g_cpu_state.regs.named.hi = num % denom;
          }
          cpu_stall_until_muldiv_complete();
          cpu_add_muldiv_ticks(cpu_get_div_ticks());
        } break;

        case CPU_FUNCT_JR: {
          g_cpu_state.next_instruction_is_branch_delay_slot = true;
          cpu_branch(cpu_read_reg((cpu_reg_t)inst.r.rs));
        } break;

        case CPU_FUNCT_JALR: {
          g_cpu_state.next_instruction_is_branch_delay_slot = true;
          const u32 target = cpu_read_reg((cpu_reg_t)inst.r.rs);
          cpu_write_reg((cpu_reg_t)inst.r.rd, g_cpu_state.npc);
          cpu_branch(target);
        } break;

        case CPU_FUNCT_SYSCALL: {
          cpu_raise_exception(CPU_EXCEPTION_SYSCALL);
        } break;

        case CPU_FUNCT_BREAK: {
          cpu_raise_break_exception(
            cpu_cop0_cause_make(CPU_EXCEPTION_BP, g_cpu_state.current_instruction_in_branch_delay_slot,
                                g_cpu_state.current_instruction_was_branch_taken,
                                (u8)g_cpu_state.current_instruction.cop.cop_n), 
            g_cpu_state.current_instruction_pc, g_cpu_state.current_instruction.bits);
        } break;

        default:
          cpu_raise_exception(CPU_EXCEPTION_RI);
          break;
      }
    } break;

    case CPU_OP_LUI: {
      cpu_write_reg((cpu_reg_t)inst.i.rt, cpu_instr_imm_zext32(inst) << 16);
    } break;

    case CPU_OP_ANDI: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, rs_val & cpu_instr_imm_zext32(inst));
    } break;

    case CPU_OP_ORI: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, rs_val | cpu_instr_imm_zext32(inst));
    } break;

    case CPU_OP_XORI: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, rs_val ^ cpu_instr_imm_zext32(inst));
    } break;

    case CPU_OP_ADDI: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      const u32 imm = cpu_instr_imm_sext32(inst);
      u32 rt_val;
      if (cpu_add_overflow(rs_val, imm, &rt_val)) {
        cpu_raise_exception(CPU_EXCEPTION_OV);
        return;
      }
      cpu_write_reg((cpu_reg_t)inst.i.rt, rt_val);
    } break;

    case CPU_OP_ADDIU: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, rs_val + cpu_instr_imm_sext32(inst));
    } break;

    case CPU_OP_SLTI: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, (u32)((s32)rs_val < (s32)cpu_instr_imm_sext32(inst)));
    } break;

    case CPU_OP_SLTIU: {
      const u32 rs_val = cpu_read_reg((cpu_reg_t)inst.i.rs);
      cpu_write_reg((cpu_reg_t)inst.i.rt, (u32)(rs_val < cpu_instr_imm_sext32(inst)));
    } break;

    case CPU_OP_LB: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u8 value;
      if (!cpu_read_memory_byte(addr, &value))
        return;
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, SignExtend32_s8((s8)value));
    } break;

    case CPU_OP_LH: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u16 value;
      if (!cpu_read_memory_halfword(addr, &value))
        return;
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, SignExtend32_s16((s16)value));
    } break;

    case CPU_OP_LW: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u32 value;
      if (!cpu_read_memory_word(addr, &value))
        return;
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, value);
    } break;

    case CPU_OP_LBU: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u8 value;
      if (!cpu_read_memory_byte(addr, &value))
        return;
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, ZeroExtend32_u8(value));
    } break;

    case CPU_OP_LHU: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u16 value;
      if (!cpu_read_memory_halfword(addr, &value))
        return;
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, ZeroExtend32_u16(value));
    } break;

    case CPU_OP_LWL:
    case CPU_OP_LWR: {
      /* LWL/LWR merge an unaligned word access into the destination register
       * across two instructions; the second one bypasses load-delay so that
       * the partial value from the first one is observable. */
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      const virtual_memory_address_t aligned_addr = addr & ~3u;
      u32 aligned_value;
      if (!cpu_read_memory_word(aligned_addr, &aligned_value))
        return;
      const u32 existing_value = ((cpu_reg_t)inst.i.rt == g_cpu_state.load_delay_reg)
                                   ? g_cpu_state.load_delay_value
                                   : cpu_read_reg((cpu_reg_t)inst.i.rt);
      const u8 shift = ((u8)addr & 3u) * 8u;
      u32 new_value;
      if ((cpu_instruction_op_t)inst.any.op == CPU_OP_LWL) {
        const u32 mask = 0x00FFFFFFu >> shift;
        new_value = (existing_value & mask) | (aligned_value << (24u - shift));
      } else {
        const u32 mask = 0xFFFFFF00u << (24u - shift);
        new_value = (existing_value & mask) | (aligned_value >> shift);
      }
      cpu_write_reg_delayed((cpu_reg_t)inst.i.rt, new_value);
    } break;

    case CPU_OP_SB: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      cpu_write_memory_byte(addr, cpu_read_reg((cpu_reg_t)inst.i.rt));
    } break;

    case CPU_OP_SH: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      cpu_write_memory_halfword(addr, cpu_read_reg((cpu_reg_t)inst.i.rt));
    } break;

    case CPU_OP_SW: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      cpu_write_memory_word(addr, cpu_read_reg((cpu_reg_t)inst.i.rt));
    } break;

    case CPU_OP_SWL:
    case CPU_OP_SWR: {
      /* SWL/SWR perform a read-modify-write on the aligned word containing
       * the requested address. */
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      const virtual_memory_address_t aligned_addr = addr & ~3u;
      const u32 reg_value = cpu_read_reg((cpu_reg_t)inst.i.rt);
      u32 mem_value;
      if (!cpu_read_memory_word(aligned_addr, &mem_value))
        return;
      const u8 shift = ((u8)addr & 3u) * 8u;
      u32 new_value;
      if ((cpu_instruction_op_t)inst.any.op == CPU_OP_SWL) {
        const u32 mem_mask = 0xFFFFFF00u << shift;
        new_value = (mem_value & mem_mask) | (reg_value >> (24u - shift));
      } else {
        const u32 mem_mask = 0x00FFFFFFu >> (24u - shift);
        new_value = (mem_value & mem_mask) | (reg_value << shift);
      }
      cpu_write_memory_word(aligned_addr, new_value);
    } break;

    case CPU_OP_J: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      cpu_branch((g_cpu_state.pc & 0xF0000000u) | (inst.j.target << 2));
    } break;

    case CPU_OP_JAL: {
      cpu_write_reg(CPU_REG_RA, g_cpu_state.npc);
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      cpu_branch((g_cpu_state.pc & 0xF0000000u) | (inst.j.target << 2));
    } break;

    case CPU_OP_BEQ: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      if (cpu_read_reg((cpu_reg_t)inst.i.rs) == cpu_read_reg((cpu_reg_t)inst.i.rt))
        cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    } break;

    case CPU_OP_BNE: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      if (cpu_read_reg((cpu_reg_t)inst.i.rs) != cpu_read_reg((cpu_reg_t)inst.i.rt))
        cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    } break;

    case CPU_OP_BGTZ: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      if ((s32)cpu_read_reg((cpu_reg_t)inst.i.rs) > 0)
        cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    } break;

    case CPU_OP_BLEZ: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      if ((s32)cpu_read_reg((cpu_reg_t)inst.i.rs) <= 0)
        cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    } break;

    case CPU_OP_B: {
      g_cpu_state.next_instruction_is_branch_delay_slot = true;
      const u8 rt = (u8)inst.i.rt;
      const bool bgez = (rt & 1u) != 0u;
      const bool branch = ((s32)cpu_read_reg((cpu_reg_t)inst.i.rs) < 0) ^ bgez;
      const bool link = (rt & 0x1Eu) == 0x10u;
      if (link)
        cpu_write_reg(CPU_REG_RA, g_cpu_state.npc);
      if (branch)
        cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    } break;

    case CPU_OP_COP0: {
      if (cpu_in_user_mode() && !g_cpu_state.cop0_regs.sr.CU0) {
        WARNING_LOG("Coprocessor 0 not present in user mode");
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }

      if (cpu_cop_is_common_instruction(inst)) {
        switch (cpu_cop_common_op(inst)) {
          case CPU_COP_COMMON_MFCN: {
            u32 value;
            switch ((cpu_cop0_reg_t)inst.r.rd) {
              case CPU_COP0_REG_BPC:       value = g_cpu_state.cop0_regs.BPC; break;
              case CPU_COP0_REG_BPCM:      value = g_cpu_state.cop0_regs.BPCM; break;
              case CPU_COP0_REG_BDA:       value = g_cpu_state.cop0_regs.BDA; break;
              case CPU_COP0_REG_BDAM:      value = g_cpu_state.cop0_regs.BDAM; break;
              case CPU_COP0_REG_DCIC:      value = g_cpu_state.cop0_regs.dcic.bits; break;
              case CPU_COP0_REG_JUMPDEST:  value = g_cpu_state.cop0_regs.TAR; break;
              case CPU_COP0_REG_BAD_VADDR: value = g_cpu_state.cop0_regs.BadVaddr; break;
              case CPU_COP0_REG_SR:        value = g_cpu_state.cop0_regs.sr.bits; break;
              case CPU_COP0_REG_CAUSE:     value = g_cpu_state.cop0_regs.cause.bits; break;
              case CPU_COP0_REG_EPC:       value = g_cpu_state.cop0_regs.EPC; break;
              case CPU_COP0_REG_PRID:      value = g_cpu_state.cop0_regs.PRID; break;
              default:
                cpu_raise_exception(CPU_EXCEPTION_RI);
                return;
            }
            cpu_write_reg_delayed((cpu_reg_t)inst.r.rt, value);
          } break;

          case CPU_COP_COMMON_MTCN: {
            const u32 value = cpu_read_reg((cpu_reg_t)inst.r.rt);
            switch ((cpu_cop0_reg_t)inst.r.rd) {
              case CPU_COP0_REG_BPC:
                g_cpu_state.cop0_regs.BPC = value;
                break;
              case CPU_COP0_REG_BPCM:
                g_cpu_state.cop0_regs.BPCM = value;
                if (cpu_update_debug_dispatcher_flag())
                  cpu_exit_execution();
                break;
              case CPU_COP0_REG_BDA:
                g_cpu_state.cop0_regs.BDA = value;
                break;
              case CPU_COP0_REG_BDAM:
                g_cpu_state.cop0_regs.BDAM = value;
                break;
              case CPU_COP0_REG_DCIC:
                g_cpu_state.cop0_regs.dcic.bits = (g_cpu_state.cop0_regs.dcic.bits & ~CPU_COP0_DCIC_WRITE_MASK) |
                                                  (value & CPU_COP0_DCIC_WRITE_MASK);
                if (cpu_update_debug_dispatcher_flag())
                  cpu_exit_execution();
                break;
              case CPU_COP0_REG_SR:
                g_cpu_state.cop0_regs.sr.bits = (g_cpu_state.cop0_regs.sr.bits & ~CPU_COP0_SR_WRITE_MASK) |
                                                (value & CPU_COP0_SR_WRITE_MASK);
                cpu_update_memory_pointers();
                cpu_check_for_pending_interrupt();
                break;
              case CPU_COP0_REG_CAUSE:
                g_cpu_state.cop0_regs.cause.bits = (g_cpu_state.cop0_regs.cause.bits & ~CPU_COP0_CAUSE_WRITE_MASK) |
                                                   (value & CPU_COP0_CAUSE_WRITE_MASK);
                cpu_check_for_pending_interrupt();
                break;
              case CPU_COP0_REG_JUMPDEST:
              case CPU_COP0_REG_BAD_VADDR:
              case CPU_COP0_REG_EPC:
                /* Read-only on PSX; writes silently ignored. */
                break;
              default:
                cpu_raise_exception(CPU_EXCEPTION_RI);
                return;
            }
          } break;

          default:
            ERROR_LOG("Unhandled COP0 common at 0x%08X: %08X",
                      g_cpu_state.current_instruction_pc, inst.bits);
            break;
        }
      } else {
        switch (cpu_cop0_op(inst)) {
          case CPU_COP0_RFE: {
            const u32 mode_bits = g_cpu_state.cop0_regs.sr.bits & 0x3Fu;
            const u32 new_mode = (mode_bits & 0x30u) | (mode_bits >> 2);
            g_cpu_state.cop0_regs.sr.bits = (g_cpu_state.cop0_regs.sr.bits & ~0x3Fu) | new_mode;
            cpu_check_for_pending_interrupt();
          } break;

          case CPU_COP0_TLBR:
          case CPU_COP0_TLBWI:
          case CPU_COP0_TLBWR:
          case CPU_COP0_TLBP:
            /* PS1 R3000A has no TLB. */
            cpu_raise_exception(CPU_EXCEPTION_RI);
            return;

          default:
            ERROR_LOG("Unhandled COP0 instruction at 0x%08X: %08X",
                      g_cpu_state.current_instruction_pc, inst.bits);
            break;
        }
      }
    } break;

    case CPU_OP_COP2: {
      if (!g_cpu_state.cop0_regs.sr.CE2) {
        WARNING_LOG("Coprocessor 2 not enabled");
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }

      if (cpu_cop_is_common_instruction(inst)) {
        switch (cpu_cop_common_op(inst)) {
          case CPU_COP_COMMON_CFCN: {
            cpu_stall_until_gte_complete();
            const u32 value = gte_read_register((u32)inst.r.rd + 32u);
            cpu_write_reg_delayed((cpu_reg_t)inst.r.rt, value);
          } break;

          case CPU_COP_COMMON_CTCN: {
            const u32 value = cpu_read_reg((cpu_reg_t)inst.r.rt);
            gte_write_register((u32)inst.r.rd + 32u, value);
          } break;

          case CPU_COP_COMMON_MFCN: {
            cpu_stall_until_gte_complete();
            const u32 value = gte_read_register((u32)inst.r.rd);
            cpu_write_reg_delayed((cpu_reg_t)inst.r.rt, value);
          } break;

          case CPU_COP_COMMON_MTCN: {
            const u32 value = cpu_read_reg((cpu_reg_t)inst.r.rt);
            gte_write_register((u32)inst.r.rd, value);
          } break;

          default:
            ERROR_LOG("Unhandled COP2 common at 0x%08X: %08X",
                      g_cpu_state.current_instruction_pc, inst.bits);
            break;
        }
      } else {
        cpu_stall_until_gte_complete();
        gte_execute_instruction(inst);
      }
    } break;

    case CPU_OP_LWC2: {
      if (!g_cpu_state.cop0_regs.sr.CE2) {
        WARNING_LOG("Coprocessor 2 not enabled");
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u32 value;
      if (!cpu_read_memory_word(addr, &value))
        return;
      gte_write_register(ZeroExtend32_u8((u8)inst.i.rt), value);
    } break;

    case CPU_OP_SWC2: {
      if (!g_cpu_state.cop0_regs.sr.CE2) {
        WARNING_LOG("Coprocessor 2 not enabled");
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }
      cpu_stall_until_gte_complete();
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      const u32 value = gte_read_register(ZeroExtend32_u8((u8)inst.i.rt));
      cpu_write_memory_word(addr, value);
    } break;

    case CPU_OP_COP1:
    case CPU_OP_COP3:
      break;

    case CPU_OP_LWC0:
    case CPU_OP_LWC1:
    case CPU_OP_LWC3: {
      /* The memory access still happens (and bus errors are still raised) but
       * the value is discarded. */
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u32 value;
      cpu_read_memory_word(addr, &value);
    } break;

    case CPU_OP_SWC0:
    case CPU_OP_SWC1:
    case CPU_OP_SWC3: {
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      cpu_write_memory_word(addr, 0u);
    } break;

    /* Reserved primary opcodes: 1*15 (illegal), 20..31 (reserved), 39, 44, 45,
     * 47, 52..55, 60..63.  Each falls through to RI; if the icache happens to
     * be stale we re-read RAM and try once more before raising. */
    default: {
      u32 ram_value;
      if (cpu_safe_read_instruction(g_cpu_state.current_instruction_pc, &ram_value) &&
          ram_value != g_cpu_state.current_instruction.bits) {
        ERROR_LOG("Stale icache at 0x%08X - ICache: %08X RAM: %08X",
                  g_cpu_state.current_instruction_pc, g_cpu_state.current_instruction.bits, ram_value);
        g_cpu_state.current_instruction.bits = ram_value;
        goto restart_instruction;
      }
      cpu_raise_exception(CPU_EXCEPTION_RI);
    } break;
  }
}

static u32 s_dbg_irq_dispatch_count;
__attribute__((visibility("default")))
u32 cpu_dbg_irq_dispatch_count(void) { return s_dbg_irq_dispatch_count; }

void cpu_dispatch_interrupt(void)
{
  s_dbg_irq_dispatch_count++;
  cpu_safe_read_instruction(g_cpu_state.pc, &g_cpu_state.next_instruction.bits);
  if ((cpu_instruction_op_t)g_cpu_state.next_instruction.any.op == CPU_OP_COP2 &&
      !cpu_cop_is_common_instruction(g_cpu_state.next_instruction)) {
    cpu_stall_until_gte_complete();
    gte_execute_instruction(g_cpu_state.next_instruction);
  }

  cpu_raise_exception_bits(
    cpu_cop0_cause_make(CPU_EXCEPTION_INT, g_cpu_state.next_instruction_is_branch_delay_slot,
                        g_cpu_state.branch_was_taken,
                        (u8)g_cpu_state.next_instruction.cop.cop_n), 
    g_cpu_state.pc);

  /* The pending interrupt zeroed downcount; restore it from the timing
   * scheduler now that the interrupt has been latched. */
  timing_events_update_cpu_downcount();
}

NORETURN static void cpu_execute_impl(void)
{
  if (g_cpu_state.pending_ticks >= g_cpu_state.downcount)
    timing_events_run_events();

  for (;;) {
    do {
      g_cpu_state.pending_ticks++;

      g_cpu_state.current_instruction.bits = g_cpu_state.next_instruction.bits;
      g_cpu_state.current_instruction_pc = g_cpu_state.pc;
      g_cpu_state.current_instruction_in_branch_delay_slot = g_cpu_state.next_instruction_is_branch_delay_slot;
      g_cpu_state.current_instruction_was_branch_taken = g_cpu_state.branch_was_taken;
      g_cpu_state.next_instruction_is_branch_delay_slot = false;
      g_cpu_state.branch_was_taken = false;

#ifdef CUPID_DEBUG_RECOMP_TRACE
      {
        static u32 _trace_n = 0;
        if (_trace_n++ < 5000u)
          ERROR_LOG("interp pc=%08X inst=%08X",
                    g_cpu_state.current_instruction_pc,
                    g_cpu_state.current_instruction.bits);
      }
#endif

      /* PC-trigger fires from interp dispatch the same way it fires from
       * the JIT prologue.  cpu_core_step_for_diff (the per-block replay
       * loop) does NOT route through this function, so the trigger cannot
       * accidentally double-fire during a recomp-diff replay. */
      if (cpu_pc_trigger_is_active() &&
          g_cpu_state.pc == cpu_pc_trigger_target_pc())
        cpu_pc_trigger_on_block(g_cpu_state.pc);

      if (!cpu_fetch_instruction())
        continue;

      if (cpu_diff_is_enabled())
        cpu_diff_record_instruction(g_cpu_state.current_instruction_pc,
                                    g_cpu_state.current_instruction.bits);
      cpu_execute_instruction();
      cpu_update_load_delay();
    } while (g_cpu_state.pending_ticks < g_cpu_state.downcount);

    timing_events_run_events();
  }
}

NORETURN static void cpu_execute_interpreter(void)
{
  cpu_execute_impl();
}

static void cpu_check_for_execution_mode_change(void)
{
  /* Settings can request RECOMPILER / CACHED_INTERPRETER, but if no backend
   * is registered, the code-cache driver falls through to the interpreter
   * loop.  We still record the requested mode (lets save-states and tooling
   * see what the user asked for) but flag using_interpreter so sibling
   * modules know there is no native code being emitted. */
  s_locals.current_execution_mode = g_settings.cpu_execution_mode;
  g_cpu_state.using_interpreter = !cpu_code_cache_is_using_recompiler();
}

NORETURN void cpu_exit_execution(void)
{
  DebugAssert(!timing_events_is_running_events());
  longjmp(s_locals.exit_jmp_buf, 1);
}

/* Public hook for cpu_code_cache.c so the code-cache fallback path can
 * re-enter the interpreter inner loop without exposing cpu_execute_impl()
 * directly.  Called only from inside the setjmp scope established by
 * cpu_execute(); cpu_exit_execution() longjmps back out. */
NORETURN void cpu_run_interpreter_loop(void)
{
  cpu_execute_impl();
}

/* Per-block recomp/interp diff: replays exactly `instruction_count`
 * interpreter steps on the current g_cpu_state.  Caller (cpu_diff_block_exit)
 * has already memcpy'd the enter snapshot into g_cpu_state and is about to
 * memcpy the post-recomp state back over us once we return.  We MUST NOT:
 *   - call timing_events_run_events (would advance the scheduler that the
 *     recomp side already advanced via downcount/end_block accounting),
 *   - longjmp out of cpu_exit_execution (no setjmp here),
 *   - touch icache, RAM (RAM tap is already routed to the interp ring).
 * Mirrors the inner cpu_execute_impl loop bounded by an instruction
 * counter instead of pending_ticks < downcount. */
void cpu_core_step_for_diff(u32 instruction_count)
{
  /* Re-seed the interpreter prefetch pipeline.  The JIT block we just exited
   * does NOT maintain next_instruction.bits / npc; it executes from inline
   * pre-decoded bits.  After a JIT exit the interpreter view is stale, so
   * before stepping we prime next_instruction with a fresh fetch at pc.
   * Mirrors cpu_recompiler_run_uncached_block's prologue.
   *
   * Fetch-count parity with recomp: an N-instruction block recompiled
   * with a non-icache fetch-tick prefix charges exactly N fetches at the
   * block's region cost (`imul size, [bios_access_time_ptr]; add`).
   * Real interp pays N+1 fetches per block in aggregate; the seed +
   * one prefetch per iter; but the +1 is the *next* block's first
   * fetch (the prefetch reads npc which the last execute already moved
   * to the next block's start).  For the diff harness we want
   * step_for_diff to charge exactly N fetches per call so per-block
   * deltas line up with recomp.  We do that by skipping the prefetch on
   * the last iter (the cost the real interp pays then is "for the next
   * block", which step_for_diff does not run).  Net: 1 seed + (N-1)
   * in-loop = N fetches.  This avoids a phantom cross-region tick
   * divergence whenever a block ends with a jr/jalr/branch whose target
   * lives in a region with a different fetch-tick cost than the block
   * itself (e.g. BIOS block ending with jr to a KSEG1 RAM mirror). */
  g_cpu_state.npc = g_cpu_state.pc;
  g_cpu_state.next_instruction_is_branch_delay_slot = false;
  g_cpu_state.branch_was_taken = false;
  if (!cpu_fetch_instruction()) return;

  /* Env-gated per-iter trace.  When CUPID_TRACE_STEP_DIFF is set to a hex
   * PC, dump current_instruction.bits + r[26] (K0) per iter for blocks
   * whose enter PC matches.  Pinpoints which iteration mutates the
   * diverging register and what opcode it ran.  Zero overhead off-path. */
  static int s_trace_step_diff_armed = -1;
  static u32 s_trace_step_diff_pc    = 0u;
  if (s_trace_step_diff_armed < 0) {
    const char* en = getenv("CUPID_TRACE_STEP_DIFF");
    if (en && en[0] != '\0') {
      s_trace_step_diff_armed = 1;
      s_trace_step_diff_pc = (u32)strtoul(en, NULL, 0);
    } else {
      s_trace_step_diff_armed = 0;
    }
  }
  /* Gate by *next_instruction.bits* (the prologue-prefetched first inst)
   * AND instruction_count == 4; isolates the suspect 4-block at
   * 0x80000080 (lui k0,0; addiu k0,k0,3200; jr k0; nop) from larger
   * blocks that happen to start with the same first opcode. */
  const bool trace_active = s_trace_step_diff_armed &&
                            instruction_count == 4u &&
                            (s_trace_step_diff_pc == 0u ||
                             g_cpu_state.next_instruction.bits == s_trace_step_diff_pc);
  if (trace_active) {
    fprintf(stderr,
            "[step_for_diff enter] pc=0x%08x npc=0x%08x next_inst=0x%08x "
            "r26=0x%08x ld_reg=%u ld_val=0x%08x next_ld_reg=%u\n",
            g_cpu_state.pc, g_cpu_state.npc, g_cpu_state.next_instruction.bits,
            g_cpu_state.regs.r[26],
            (unsigned)g_cpu_state.load_delay_reg, g_cpu_state.load_delay_value,
            (unsigned)g_cpu_state.next_load_delay_reg);
  }
  for (u32 i = 0; i < instruction_count; i++) {
    g_cpu_state.pending_ticks++;
    g_cpu_state.current_instruction.bits = g_cpu_state.next_instruction.bits;
    g_cpu_state.current_instruction_pc = g_cpu_state.pc;
    g_cpu_state.current_instruction_in_branch_delay_slot = g_cpu_state.next_instruction_is_branch_delay_slot;
    g_cpu_state.current_instruction_was_branch_taken = g_cpu_state.branch_was_taken;
    g_cpu_state.next_instruction_is_branch_delay_slot = false;
    g_cpu_state.branch_was_taken = false;

    if (i + 1 < instruction_count) {
      if (!cpu_fetch_instruction()) {
        /* Fetch raised an exception; the interpreter's exception path
         * already mutated state.  Stop replay so we compare at the same
         * boundary. */
        if (trace_active)
          fprintf(stderr, "[step_for_diff iter=%u FETCH FAILED]\n", i);
        break;
      }
    }
    cpu_execute_instruction();
    cpu_update_load_delay();

    if (trace_active) {
      fprintf(stderr,
              "[step_for_diff iter=%u] cur_inst=0x%08x r26=0x%08x "
              "ld_reg=%u ld_val=0x%08x next_ld_reg=%u next_inst=0x%08x "
              "exc=%u\n",
              i, g_cpu_state.current_instruction.bits, g_cpu_state.regs.r[26],
              (unsigned)g_cpu_state.load_delay_reg, g_cpu_state.load_delay_value,
              (unsigned)g_cpu_state.next_load_delay_reg,
              g_cpu_state.next_instruction.bits,
              (unsigned)g_cpu_state.exception_raised);
    }

    if (g_cpu_state.exception_raised) break;
  }
}

void cpu_recompiler_thunk_interpret_instruction(void)
{
  if (cpu_diff_is_enabled())
    cpu_diff_record_instruction(g_cpu_state.current_instruction_pc,
                                g_cpu_state.current_instruction.bits);
  g_cpu_state.exception_raised = false;
  cpu_execute_instruction();
  cpu_update_load_delay();
}

void cpu_recompiler_run_uncached_block(void)
{
  g_cpu_state.npc = g_cpu_state.pc;
  g_cpu_state.exception_raised = false;
  if (!cpu_fetch_instruction_for_interp_fallback())
    return;

  bool in_branch_delay_slot = false;
  for (;;) {
    g_cpu_state.pending_ticks++;

    g_cpu_state.current_instruction.bits = g_cpu_state.next_instruction.bits;
    g_cpu_state.current_instruction_pc = g_cpu_state.pc;
    g_cpu_state.current_instruction_in_branch_delay_slot = g_cpu_state.next_instruction_is_branch_delay_slot;
    g_cpu_state.current_instruction_was_branch_taken = g_cpu_state.branch_was_taken;
    g_cpu_state.next_instruction_is_branch_delay_slot = false;
    g_cpu_state.branch_was_taken = false;

    const bool branch = cpu_is_branch_instruction(g_cpu_state.current_instruction);
    if (!g_cpu_state.current_instruction_in_branch_delay_slot || branch) {
      if (!cpu_fetch_instruction_for_interp_fallback())
        break;
    } else {
      g_cpu_state.pc = g_cpu_state.npc;
    }

    if (cpu_diff_is_enabled())
      cpu_diff_record_instruction(g_cpu_state.current_instruction_pc,
                                  g_cpu_state.current_instruction.bits);
    cpu_execute_instruction();
    cpu_update_load_delay();

    if (g_cpu_state.exception_raised || (!branch && in_branch_delay_slot) ||
        cpu_is_exit_block_instruction(g_cpu_state.current_instruction))
      break;

    if ((g_cpu_state.current_instruction.bits & 0xFFC0FFFFu) == 0x40806000u &&
        cpu_has_pending_interrupt())
      break;

    in_branch_delay_slot = branch;
  }
}

u32 cpu_recompiler_thunk_read_memory_byte(u32 address)
{
  u8 v = 0u;
  return cpu_read_memory_byte(address, &v) ? (u32)v : 0u;
}

u32 cpu_recompiler_thunk_read_memory_halfword(u32 address)
{
  u16 v = 0u;
  return cpu_read_memory_halfword(address, &v) ? (u32)v : 0u;
}

u32 cpu_recompiler_thunk_read_memory_word(u32 address)
{
  u32 v = 0u;
  (void)cpu_read_memory_word(address, &v);
  return v;
}

void cpu_recompiler_thunk_write_memory_byte(u32 address, u32 value)
{
  (void)cpu_write_memory_byte(address, value);
}

void cpu_recompiler_thunk_write_memory_halfword(u32 address, u32 value)
{
  (void)cpu_write_memory_halfword(address, value);
}

void cpu_recompiler_thunk_write_memory_word(u32 address, u32 value)
{
  (void)cpu_write_memory_word(address, value);
}

void cpu_execute(void)
{
  cpu_check_for_execution_mode_change();

  if (setjmp(s_locals.exit_jmp_buf) != 0)
    return;

  if (s_locals.current_execution_mode == CPU_EXECUTION_MODE_INTERPRETER)
    cpu_execute_interpreter();
  else
    cpu_code_cache_execute();
}

bus_memory_read_handler_t cpu_get_memory_read_handler(virtual_memory_address_t address, memory_access_size_t size)
{
  bus_memory_read_handler_t* base = bus_offset_read_handler_array(g_cpu_state.memory_handlers, size);
  return base[address >> BUS_MEMORY_LUT_PAGE_SHIFT];
}

bus_memory_write_handler_t cpu_get_memory_write_handler(virtual_memory_address_t address, memory_access_size_t size)
{
  bus_memory_write_handler_t* base = bus_offset_write_handler_array(g_cpu_state.memory_handlers, size);
  return base[address >> BUS_MEMORY_LUT_PAGE_SHIFT];
}

static bool cpu_do_instruction_read(physical_memory_address_t address, u32* data,
                                    bool add_ticks, bool icache_read, u32 word_count,
                                    bool raise_exceptions)
{
  /* Caller is required to ensure the address is within the physical map;
   * the high bits get masked out below. */
  address &= CPU_KSEG_MASK;

  if (address < BUS_RAM_MIRROR_END) {
    const u32 offset = address & bus_get_ram_mask();
    memcpy(data, bus_get_ram_pointer() + offset, sizeof(u32) * word_count);
    if (add_ticks)
      g_cpu_state.pending_ticks += (icache_read ? 1u : (u32)BUS_RAM_READ_TICKS) * word_count;
    return true;
  }
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_SIZE)) {
    memcpy(data, bus_get_bios_pointer() + ((address - BUS_BIOS_BASE) & BUS_BIOS_MASK), sizeof(u32) * word_count);
    if (add_ticks)
      g_cpu_state.pending_ticks += (u32)bus_get_bios_access_time()[MEMORY_ACCESS_SIZE_WORD] * word_count;
    return true;
  }
  if (address >= BUS_EXP1_BASE && address < (BUS_EXP1_BASE + BUS_EXP1_SIZE)) {
    memset(data, 0, sizeof(u32) * word_count);
    if (add_ticks)
      g_cpu_state.pending_ticks += (u32)bus_get_exp1_access_time()[MEMORY_ACCESS_SIZE_WORD] * word_count;
    return true;
  }

  if (raise_exceptions) {
    g_cpu_state.cop0_regs.BadVaddr = address;
    cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_IBE, false, false, 0u), address);
  }
  memset(data, 0, sizeof(u32) * word_count);
  return false;
}

tick_count_t cpu_get_instruction_read_ticks(virtual_memory_address_t address)
{
  address &= CPU_KSEG_MASK;
  if (address < BUS_RAM_MIRROR_END)
    return BUS_RAM_READ_TICKS;
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_MIRROR_SIZE))
    return bus_get_bios_access_time()[MEMORY_ACCESS_SIZE_WORD];
  return 0;
}

tick_count_t cpu_get_icache_fill_ticks(virtual_memory_address_t address)
{
  address &= CPU_KSEG_MASK;
  if (address < BUS_RAM_MIRROR_END)
    return (tick_count_t)((CPU_ICACHE_LINE_SIZE - (address & (CPU_ICACHE_LINE_SIZE - 1u))) / 4u);
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_MIRROR_SIZE))
    return (tick_count_t)(bus_get_bios_access_time()[MEMORY_ACCESS_SIZE_WORD] *
                          (s32)((CPU_ICACHE_LINE_SIZE - (address & (CPU_ICACHE_LINE_SIZE - 1u))) / 4u));
  return 0;
}

void cpu_check_and_update_icache_tags(u32 line_count)
{
  virtual_memory_address_t current_pc = g_cpu_state.pc & CPU_ICACHE_TAG_ADDRESS_MASK;
  tick_count_t ticks = 0;
  const tick_count_t cached_ticks_per_line = cpu_get_icache_fill_ticks(current_pc);
  for (u32 i = 0; i < line_count; i++, current_pc += CPU_ICACHE_LINE_SIZE) {
    const u32 line = cpu_get_icache_line(current_pc);
    if (g_cpu_state.icache_tags[line] != current_pc) {
      g_cpu_state.icache_tags[line] = current_pc;
      ticks += cached_ticks_per_line;
    }
  }
  g_cpu_state.pending_ticks += (u32)ticks;
}

u32 cpu_fill_icache(virtual_memory_address_t address)
{
  const u32 line = cpu_get_icache_line(address);
  const u32 line_word_offset = cpu_get_icache_line_word_offset(address);
  u32* const line_data = g_cpu_state.icache_data + (line * CPU_ICACHE_WORDS_PER_LINE);
  u32* const offset_line_data = line_data + line_word_offset;
  u32 line_tag;
  switch (line_word_offset) {
    case 0:
      cpu_do_instruction_read(address & ~(CPU_ICACHE_LINE_SIZE - 1u), offset_line_data, true, true, 4u, false);
      line_tag = cpu_get_icache_tag_for_address(address);
      break;
    case 1:
      cpu_do_instruction_read(address & (~(CPU_ICACHE_LINE_SIZE - 1u) | 0x4u), offset_line_data, true, true, 3u, false);
      line_tag = cpu_get_icache_tag_for_address(address) | 0x1u;
      break;
    case 2:
      cpu_do_instruction_read(address & (~(CPU_ICACHE_LINE_SIZE - 1u) | 0x8u), offset_line_data, true, true, 2u, false);
      line_tag = cpu_get_icache_tag_for_address(address) | 0x3u;
      break;
    case 3:
    default:
      cpu_do_instruction_read(address & (~(CPU_ICACHE_LINE_SIZE - 1u) | 0xCu), offset_line_data, true, true, 1u, false);
      line_tag = cpu_get_icache_tag_for_address(address) | 0x7u;
      break;
  }
  g_cpu_state.icache_tags[line] = line_tag;
  return offset_line_data[0];
}

void cpu_clear_icache(void)
{
  memset(g_cpu_state.icache_data, 0, CPU_ICACHE_SIZE);
  for (u32 i = 0; i < CPU_ICACHE_LINES; i++)
    g_cpu_state.icache_tags[i] = CPU_ICACHE_INVALID_BITS;
}

static u32 cpu_read_icache(virtual_memory_address_t address)
{
  const u32 line = cpu_get_icache_line(address);
  const u32 line_word_offset = cpu_get_icache_line_word_offset(address);
  const u32* const line_data = g_cpu_state.icache_data + (line * CPU_ICACHE_WORDS_PER_LINE);
  return line_data[line_word_offset];
}

static bool cpu_fetch_instruction(void)
{
  DebugAssert(IsAlignedPow2(g_cpu_state.npc, 4u));
  const physical_memory_address_t address = g_cpu_state.npc;
  switch (address >> 29) {
    case 0x00: /* KUSEG 0M-512M */
    case 0x04: /* KSEG0   cached */
      if (cpu_compare_icache_tag(address))
        g_cpu_state.next_instruction.bits = cpu_read_icache(address);
      else
        g_cpu_state.next_instruction.bits = cpu_fill_icache(address);
      break;

    case 0x05: /* KSEG1 uncached */
      if (!cpu_do_instruction_read(address, &g_cpu_state.next_instruction.bits, true, false, 1u, true))
        return false;
      break;

    case 0x01: case 0x02: case 0x03: /* KUSEG > 512M */
    case 0x06: case 0x07: default:   /* KSEG2 */
      cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_IBE, false, false, 0u), address);
      return false;
  }
  g_cpu_state.pc = g_cpu_state.npc;
  g_cpu_state.npc += sizeof(g_cpu_state.next_instruction.bits);
  return true;
}

/* Recompiler fallback path can be entered with an unaligned npc (e.g. a
 * runaway jr loaded a misaligned target).  Real hardware raises AdEL on the
 * fetch itself; the hot interp path asserts because branches normally trap
 * misalignment up front.  Raise AdEL instead of crashing in debug builds. */
static bool cpu_fetch_instruction_for_interp_fallback(void)
{
  if (!IsAlignedPow2(g_cpu_state.npc, 4u)) {
    g_cpu_state.cop0_regs.BadVaddr = g_cpu_state.npc;
    cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_ADEL, false, false, 0u),
                             g_cpu_state.npc);
    return false;
  }
  return cpu_fetch_instruction();
}

/* Dispatcher safety net helpers.  See cpu_core.h for rationale. */
bool cpu_recompiler_pc_is_executable(u32 pc)
{
  if (!IsAlignedPow2(pc, 4u))
    return false;
  switch (pc >> 29) {
    case 0x00: /* KUSEG 0..512M */
    case 0x04: /* KSEG0   cached */
    case 0x05: /* KSEG1 uncached */
      return true;
    default:
      return false;
  }
}

bool cpu_recompiler_synth_pc_exception_if_invalid(u32 pc)
{
  if (cpu_recompiler_pc_is_executable(pc))
    return false;
  /* Capture cpu_state + last-4 compiled blocks at first trigger so the
   * underlying recomp/interp divergence is debuggable. */
  cpu_code_cache_dbg_log_safety_net_exception(pc);
  if (!IsAlignedPow2(pc, 4u)) {
    g_cpu_state.cop0_regs.BadVaddr = pc;
    cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_ADEL, false, false, 0u), pc);
  } else {
    cpu_raise_exception_bits(cpu_cop0_cause_make(CPU_EXCEPTION_IBE, false, false, 0u), pc);
  }
  return true;
}

bool cpu_safe_read_instruction(virtual_memory_address_t addr, u32* value)
{
  switch (addr >> 29) {
    case 0x00: case 0x04: case 0x05:
      return cpu_do_instruction_read(addr, value, false, false, 1u, false);
    default:
      return false;
  }
}

static bool cpu_do_safe_memory_access_read(virtual_memory_address_t address, memory_access_size_t size, u32* value)
{
  switch (address >> 29) {
    case 0x00: case 0x04:
      if ((address & CPU_SCRATCHPAD_ADDR_MASK) == CPU_SCRATCHPAD_ADDR) {
        const u32 offset = address & CPU_SCRATCHPAD_OFFSET_MASK;
        if (size == MEMORY_ACCESS_SIZE_BYTE) {
          *value = g_cpu_state.scratchpad[offset];
        } else if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
          u16 t; memcpy(&t, &g_cpu_state.scratchpad[offset], sizeof(u16));
          *value = ZeroExtend32_u16(t);
        } else {
          memcpy(value, &g_cpu_state.scratchpad[offset], sizeof(u32));
        }
        return true;
      }
      address &= CPU_KSEG_MASK;
      break;
    case 0x05:
      address &= CPU_KSEG_MASK;
      break;
    default:
      /* KUSEG > 512MB or KSEG2 */
      return false;
  }

  if (address < BUS_RAM_MIRROR_END) {
    const u32 offset = address & bus_get_ram_mask();
    const u8* ram = bus_get_unprotected_ram_pointer();
    if (size == MEMORY_ACCESS_SIZE_BYTE) {
      *value = ram[offset];
    } else if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
      u16 t; memcpy(&t, &ram[offset], sizeof(u16));
      *value = ZeroExtend32_u16(t);
    } else {
      memcpy(value, &ram[offset], sizeof(u32));
    }
    return true;
  }
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_SIZE)) {
    const u32 offset = address & BUS_BIOS_MASK;
    const u8* bios = bus_get_bios_pointer();
    if (size == MEMORY_ACCESS_SIZE_BYTE) {
      *value = ZeroExtend32_u8(bios[offset]);
    } else if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
      u16 t; memcpy(&t, &bios[offset], sizeof(u16));
      *value = ZeroExtend32_u16(t);
    } else {
      memcpy(value, &bios[offset], sizeof(u32));
    }
    return true;
  }
  return false;
}

static bool cpu_do_safe_memory_access_write(virtual_memory_address_t address, memory_access_size_t size, u32 value)
{
  switch (address >> 29) {
    case 0x00: case 0x04:
      if ((address & CPU_SCRATCHPAD_ADDR_MASK) == CPU_SCRATCHPAD_ADDR) {
        const u32 offset = address & CPU_SCRATCHPAD_OFFSET_MASK;
        if (size == MEMORY_ACCESS_SIZE_BYTE) {
          g_cpu_state.scratchpad[offset] = (u8)value;
        } else if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
          memcpy(&g_cpu_state.scratchpad[offset], &value, sizeof(u16));
        } else {
          memcpy(&g_cpu_state.scratchpad[offset], &value, sizeof(u32));
        }
        return true;
      }
      address &= CPU_KSEG_MASK;
      break;
    case 0x05:
      address &= CPU_KSEG_MASK;
      break;
    default:
      return false;
  }

  if (address < BUS_RAM_MIRROR_END) {
    const u32 offset = address & bus_get_ram_mask();
    u8* ram = bus_get_unprotected_ram_pointer();
    if (size == MEMORY_ACCESS_SIZE_BYTE) {
      ram[offset] = (u8)value;
    } else if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
      const u16 t = (u16)value;
      memcpy(&ram[offset], &t, sizeof(u16));
    } else {
      memcpy(&ram[offset], &value, sizeof(u32));
    }
    return true;
  }
  return false;
}

bool cpu_safe_read_memory_byte(virtual_memory_address_t addr, u8* value)
{
  u32 t = 0;
  if (!cpu_do_safe_memory_access_read(addr, MEMORY_ACCESS_SIZE_BYTE, &t))
    return false;
  *value = (u8)t;
  return true;
}

bool cpu_safe_read_memory_halfword(virtual_memory_address_t addr, u16* value)
{
  if ((addr & 1u) == 0u) {
    u32 t = 0;
    if (!cpu_do_safe_memory_access_read(addr, MEMORY_ACCESS_SIZE_HALFWORD, &t))
      return false;
    *value = (u16)t;
    return true;
  }
  u8 lo, hi;
  if (!cpu_safe_read_memory_byte(addr, &lo) || !cpu_safe_read_memory_byte(addr + 1u, &hi))
    return false;
  *value = (u16)((u16)hi << 8) | (u16)lo;
  return true;
}

bool cpu_safe_read_memory_word(virtual_memory_address_t addr, u32* value)
{
  if ((addr & 3u) == 0u)
    return cpu_do_safe_memory_access_read(addr, MEMORY_ACCESS_SIZE_WORD, value);
  u16 lo, hi;
  if (!cpu_safe_read_memory_halfword(addr, &lo) || !cpu_safe_read_memory_halfword(addr + 2u, &hi))
    return false;
  *value = ((u32)hi << 16) | (u32)lo;
  return true;
}

bool cpu_safe_write_memory_byte(virtual_memory_address_t addr, u8 value)
{
  return cpu_do_safe_memory_access_write(addr, MEMORY_ACCESS_SIZE_BYTE, ZeroExtend32_u8(value));
}

bool cpu_safe_write_memory_halfword(virtual_memory_address_t addr, u16 value)
{
  if ((addr & 1u) == 0u)
    return cpu_do_safe_memory_access_write(addr, MEMORY_ACCESS_SIZE_HALFWORD, ZeroExtend32_u16(value));
  return cpu_safe_write_memory_byte(addr, (u8)value) &&
         cpu_safe_write_memory_byte(addr + 1u, (u8)(value >> 8));
}

bool cpu_safe_write_memory_word(virtual_memory_address_t addr, u32 value)
{
  if ((addr & 3u) == 0u)
    return cpu_do_safe_memory_access_write(addr, MEMORY_ACCESS_SIZE_WORD, value);
  return cpu_safe_write_memory_halfword(addr, (u16)value) &&
         cpu_safe_write_memory_halfword(addr + 2u, (u16)(value >> 16));
}

bool cpu_safe_read_memory_bytes(virtual_memory_address_t addr, void* data, u32 length)
{
  const u32 seg = addr >> 29;
  const u32 ram_size = bus_get_ram_size();
  if ((seg != 0u && seg != 4u && seg != 5u) ||
      (((addr + length) & CPU_KSEG_MASK) >= BUS_RAM_MIRROR_END) ||
      (((addr & bus_get_ram_mask()) + length) > ram_size)) {
    u8* ptr = (u8*)data;
    u8* const end = ptr + length;
    while (ptr != end) {
      if (!cpu_safe_read_memory_byte(addr++, ptr++))
        return false;
    }
    return true;
  }
  memcpy(data, bus_get_ram_pointer() + (addr & bus_get_ram_mask()), length);
  return true;
}

bool cpu_safe_write_memory_bytes(virtual_memory_address_t addr, const void* data, u32 length)
{
  const u32 seg = addr >> 29;
  const u32 ram_size = bus_get_ram_size();
  if ((seg != 0u && seg != 4u && seg != 5u) ||
      (((addr + length) & CPU_KSEG_MASK) >= BUS_RAM_MIRROR_END) ||
      (((addr & bus_get_ram_mask()) + length) > ram_size)) {
    const u8* ptr = (const u8*)data;
    const u8* const end = ptr + length;
    while (ptr != end) {
      if (!cpu_safe_write_memory_byte(addr++, *ptr++))
        return false;
    }
    return true;
  }
  memcpy(bus_get_ram_pointer() + (addr & bus_get_ram_mask()), data, length);
  return true;
}

bool cpu_safe_zero_memory_bytes(virtual_memory_address_t addr, u32 length)
{
  const u32 seg = addr >> 29;
  const u32 ram_size = bus_get_ram_size();
  if ((seg != 0u && seg != 4u && seg != 5u) ||
      (((addr + length) & CPU_KSEG_MASK) >= BUS_RAM_MIRROR_END) ||
      (((addr & bus_get_ram_mask()) + length) > ram_size)) {
    while ((addr & 3u) != 0u && length > 0u) {
      if (!cpu_safe_write_memory_byte(addr, 0u))
        return false;
      addr++; length--;
    }
    while (length >= 4u) {
      if (!cpu_safe_write_memory_word(addr, 0u))
        return false;
      addr += 4u; length -= 4u;
    }
    while (length > 0u) {
      if (!cpu_safe_write_memory_byte(addr, 0u))
        return false;
      addr++; length--;
    }
    return true;
  }
  memset(bus_get_ram_pointer() + (addr & bus_get_ram_mask()), 0, length);
  return true;
}

void* cpu_get_direct_read_memory_pointer(virtual_memory_address_t address, memory_access_size_t size,
                                         tick_count_t* read_ticks)
{
  const u32 seg = address >> 29;
  if (seg != 0u && seg != 4u && seg != 5u)
    return NULL;
  const physical_memory_address_t paddr = cpu_virtual_to_physical(address);
  if (paddr < BUS_RAM_MIRROR_END) {
    if (read_ticks) *read_ticks = BUS_RAM_READ_TICKS;
    return bus_get_ram_pointer() + (paddr & bus_get_ram_mask());
  }
  if ((paddr & CPU_SCRATCHPAD_ADDR_MASK) == CPU_SCRATCHPAD_ADDR) {
    if (read_ticks) *read_ticks = 0;
    return &g_cpu_state.scratchpad[paddr & CPU_SCRATCHPAD_OFFSET_MASK];
  }
  if (paddr >= BUS_BIOS_BASE && paddr < (BUS_BIOS_BASE + BUS_BIOS_SIZE)) {
    if (read_ticks) *read_ticks = bus_get_bios_access_time()[size];
    return bus_get_bios_pointer() + (paddr & BUS_BIOS_MASK);
  }
  return NULL;
}

void* cpu_get_direct_write_memory_pointer(virtual_memory_address_t address, memory_access_size_t size)
{
  (void)size;
  const u32 seg = address >> 29;
  if (seg != 0u && seg != 4u && seg != 5u)
    return NULL;
  const physical_memory_address_t paddr = address & CPU_KSEG_MASK;
  if (paddr < BUS_RAM_MIRROR_END)
    return bus_get_ram_pointer() + (paddr & bus_get_ram_mask());
  if ((paddr & CPU_SCRATCHPAD_ADDR_MASK) == CPU_SCRATCHPAD_ADDR)
    return &g_cpu_state.scratchpad[paddr & CPU_SCRATCHPAD_OFFSET_MASK];
  return NULL;
}

static bool cpu_do_alignment_check(memory_access_type_t type, memory_access_size_t size,
                                   virtual_memory_address_t address)
{
  if (size == MEMORY_ACCESS_SIZE_HALFWORD) {
    if (IsAlignedPow2(address, 2u))
      return true;
  } else if (size == MEMORY_ACCESS_SIZE_WORD) {
    if (IsAlignedPow2(address, 4u))
      return true;
  } else {
    return true;
  }
  g_cpu_state.cop0_regs.BadVaddr = address;
  cpu_raise_exception(type == MEMORY_ACCESS_TYPE_READ ? CPU_EXCEPTION_ADEL : CPU_EXCEPTION_ADES);
  return false;
}

static void cpu_raise_data_bus_exception(void)
{
  cpu_raise_exception_bits(
    cpu_cop0_cause_make(CPU_EXCEPTION_DBE, g_cpu_state.current_instruction_in_branch_delay_slot,
                        g_cpu_state.current_instruction_was_branch_taken, 0u),
    g_cpu_state.current_instruction_pc);
}

static bool cpu_read_memory_byte(virtual_memory_address_t addr, u8* value)
{
  *value = (u8)cpu_get_memory_read_handler(addr, MEMORY_ACCESS_SIZE_BYTE)(addr);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

static bool cpu_read_memory_halfword(virtual_memory_address_t addr, u16* value)
{
  if (!cpu_do_alignment_check(MEMORY_ACCESS_TYPE_READ, MEMORY_ACCESS_SIZE_HALFWORD, addr))
    return false;
  *value = (u16)cpu_get_memory_read_handler(addr, MEMORY_ACCESS_SIZE_HALFWORD)(addr);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

static bool cpu_read_memory_word(virtual_memory_address_t addr, u32* value)
{
  if (!cpu_do_alignment_check(MEMORY_ACCESS_TYPE_READ, MEMORY_ACCESS_SIZE_WORD, addr))
    return false;
  *value = cpu_get_memory_read_handler(addr, MEMORY_ACCESS_SIZE_WORD)(addr);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

static bool cpu_write_memory_byte(virtual_memory_address_t addr, u32 value)
{
  cpu_get_memory_write_handler(addr, MEMORY_ACCESS_SIZE_BYTE)(addr, value);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

static bool cpu_write_memory_halfword(virtual_memory_address_t addr, u32 value)
{
  if (!cpu_do_alignment_check(MEMORY_ACCESS_TYPE_WRITE, MEMORY_ACCESS_SIZE_HALFWORD, addr))
    return false;
  cpu_get_memory_write_handler(addr, MEMORY_ACCESS_SIZE_HALFWORD)(addr, value);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

static bool cpu_write_memory_word(virtual_memory_address_t addr, u32 value)
{
  if (!cpu_do_alignment_check(MEMORY_ACCESS_TYPE_WRITE, MEMORY_ACCESS_SIZE_WORD, addr))
    return false;
  cpu_get_memory_write_handler(addr, MEMORY_ACCESS_SIZE_WORD)(addr, value);
  if (g_cpu_state.bus_error) {
    g_cpu_state.bus_error = false;
    cpu_raise_data_bus_exception();
    return false;
  }
  return true;
}

extern void cpu_add_pending_ticks(tick_count_t ticks);
extern u32  cpu_get_icache_line(virtual_memory_address_t address);
extern u32  cpu_get_icache_line_offset(virtual_memory_address_t address);
extern u32  cpu_get_icache_tag_for_address(virtual_memory_address_t address);

u32 cpu_get_pc(void)
{
  return g_cpu_state.pc;
}

void cpu_set_bus_error(bool v)
{
  g_cpu_state.bus_error = v;
}

/* mdec.c historical name; same body as cpu_add_pending_ticks. */
void cpu_core_add_pending_ticks(tick_count_t ticks)
{
  g_cpu_state.pending_ticks += (u32)ticks;
}

 /* VirtualAddressToPhysical: KUSEG (top bit
 * clear) maps to the bottom 2GB; KSEG0/1/2 collapse onto the bottom 512MB. */
u32 cpu_virtual_to_physical_address(virtual_memory_address_t addr)
{
  return addr & ((addr & 0x80000000u) ? CPU_KSEG_MASK : CPU_KUSEG_MASK);
}

u8* cpu_get_scratchpad_pointer(void)
{
  return g_cpu_state.scratchpad;
}

u32 cpu_get_scratchpad_size(void)
{
  return CPU_SCRATCHPAD_SIZE;
}

u32 cpu_get_scratchpad_address(void)
{
  return CPU_SCRATCHPAD_ADDR;
}

u32 cpu_get_cache_control_bits(void)
{
  return g_cpu_state.cache_control.bits;
}

void cpu_set_cache_control_bits(u32 v)
{
  g_cpu_state.cache_control.bits = v;
}

u32* cpu_get_icache_data(void)
{
  return g_cpu_state.icache_data;
}

u32* cpu_get_icache_tags(void)
{
  return g_cpu_state.icache_tags;
}

/* Real implementation lives in cpu_code_cache.c.  This weak no-op only
 * exists so legacy interpreter-only builds (without the code-cache TU)
 * still link; the strong symbol from cpu_code_cache.c supersedes it
 * whenever that file is in the build. */
__attribute__((weak)) void cpu_code_cache_invalidate_all_ram_blocks(void)
{
}
