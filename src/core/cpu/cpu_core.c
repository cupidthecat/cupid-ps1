/*
 * R3000A instruction execution, register state and exception handling.
 * The instruction dispatcher implements branch and load delays, division
 * edge cases and COP0 mode-stack transitions. Cached execution and recompiler
 * dispatch use the shared CPU state and code-cache interfaces.
 */

#include "cpu_core.h"

#include "core/memory/bus.h"
#include "cpu_code_cache.h"
#include "cpu_core_private.h"
#include "cpu_diff_runner.h"
#include "cpu_execution_stats.h"
#include "cpu_pc_trigger.h"
#include "cpu_types.h"
#include "core/gte/gte.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/config/settings.h"
#include "core/scheduler/timing_event.h"
#include "core/shared/types.h"

#include "util/state/state_wrapper.h"

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

cpu_state_t g_cpu_state;

/* Scoped to this translation unit: dispatcher locals that survive across
 * a longjmp out of cpu_execute (so we don't clobber caller stack). */
typedef struct {
  cpu_execution_mode_t current_execution_mode;
  jmp_buf exit_jmp_buf;
  u32                  prefetched_pc;
  bool                 prefetch_valid;
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

/* Internal interpreter entry point used when cached execution has no
 * registered compiler backend. */
NORETURN void cpu_run_interpreter_loop(void);

static void cpu_execute_instruction(bool observe_tty);

static bool cpu_fetch_instruction(void);
static bool cpu_raise_pending_fetch_exception(void);

static bool cpu_do_instruction_read(physical_memory_address_t address, u32* data,
                                    bool add_ticks, bool icache_read, u32 word_count,
                                    bool allow_device_reads);

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
  if (state_wrapper_get_version(sw) >= 85u)
    state_wrapper_do_u8(sw, &g_cpu_state.next_instruction_fetch_exception);
  else if (state_wrapper_is_reading(sw))
    g_cpu_state.next_instruction_fetch_exception = 0;
  if (state_wrapper_is_reading(sw) && g_cpu_state.next_instruction_fetch_exception != 0 &&
      g_cpu_state.next_instruction_fetch_exception != CPU_EXCEPTION_ADEL &&
      g_cpu_state.next_instruction_fetch_exception != CPU_EXCEPTION_IBE)
    return false;

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

  if (state_wrapper_is_reading(sw) && state_wrapper_get_version(sw) < 86u) {
    /* Older streams used the cached virtual address as the internal tag. */
    for (u32 i = 0; i < CPU_ICACHE_LINES; i++) {
      if ((g_cpu_state.icache_tags[i] & 0xE0000000u) == 0x80000000u)
        g_cpu_state.icache_tags[i] &= ~0x80000000u;
    }
  }

  state_wrapper_do_bool(sw, &g_cpu_state.using_interpreter);

  if (state_wrapper_is_reading(sw)) {
    s_locals.prefetched_pc  = g_cpu_state.pc;
    s_locals.prefetch_valid = state_wrapper_get_version(sw) >= 87u || g_cpu_state.using_interpreter ||
                              g_cpu_state.next_instruction_is_branch_delay_slot ||
                              g_cpu_state.next_instruction_fetch_exception != 0;
    s_locals.current_execution_mode =
      g_cpu_state.using_interpreter
        ? CPU_EXECUTION_MODE_INTERPRETER
        : ((g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_INTERPRETER)
             ? CPU_EXECUTION_MODE_CACHED_INTERPRETER 
             : g_settings.cpu_execution_mode);
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
    if (!cpu_safe_read_memory_byte(addr++, &value))
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
  if (!g_settings.bios_tty_logging) return;
  const u32 call = g_cpu_state.regs.named.t1;
  if (call == 0x03u)
    cpu_handle_write_syscall();
  else if (call == 0x3cu || (call == 0x09u && g_cpu_state.regs.named.a1 == 1u))
    cpu_handle_putc_syscall();
  else if (call == 0x3eu)
    cpu_handle_puts_syscall();
}

void cpu_handle_b0_syscall(void)
{
  if (!g_settings.bios_tty_logging) return;
  const u32 call = g_cpu_state.regs.named.t1;
  if (call == 0x35u)
    cpu_handle_write_syscall();
  else if (call == 0x3du || (call == 0x3bu && g_cpu_state.regs.named.a1 == 1u))
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

static void cpu_execute_instruction(bool observe_tty)
{
  if (observe_tty && g_settings.bios_tty_logging) {
    const u32 pc = cpu_virtual_to_physical(g_cpu_state.current_instruction_pc);
    if (pc == 0xA0u) cpu_handle_a0_syscall();
    else if (pc == 0xB0u) cpu_handle_b0_syscall();
  }
restart_instruction:;
  const cpu_instruction_t inst = g_cpu_state.current_instruction;

  /* Fast-path nops without entering the switch. */
  if (inst.bits == 0u)
    return;

  if (cpu_is_coprocessor_branch_instruction(inst)) {
    /* Branches require CU0 even in kernel mode. The condition input is low
     * for every coprocessor; rt bit 1 does not annul the delay slot. */
    if (!(g_cpu_state.cop0_regs.sr.bits & (1u << (28u + inst.cop.cop_n)))) {
      cpu_raise_exception(CPU_EXCEPTION_CPU);
      return;
    }
    g_cpu_state.next_instruction_is_branch_delay_slot = true;
    if ((inst.i.rt & 1u) == 0u) cpu_branch(g_cpu_state.pc + (cpu_instr_imm_sext32(inst) << 2));
    return;
  }

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
        cpu_execute_gte(inst.bits);
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
      if (!(g_cpu_state.cop0_regs.sr.bits & (1u << (28u + inst.cop.cop_n)))) {
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }
      break;

    case CPU_OP_LWC0:
    case CPU_OP_LWC1:
    case CPU_OP_LWC3: {
      if (!(g_cpu_state.cop0_regs.sr.bits & (1u << (28u + inst.cop.cop_n)))) {
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }
      /* The memory access still happens (and bus errors are still raised) but
       * the value is discarded. */
      const virtual_memory_address_t addr = cpu_read_reg((cpu_reg_t)inst.i.rs) + cpu_instr_imm_sext32(inst);
      u32 value;
      cpu_read_memory_word(addr, &value);
    } break;

    case CPU_OP_SWC0:
    case CPU_OP_SWC1:
    case CPU_OP_SWC3: {
      if (!(g_cpu_state.cop0_regs.sr.bits & (1u << (28u + inst.cop.cop_n)))) {
        cpu_raise_exception(CPU_EXCEPTION_CPU);
        return;
      }
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

static bool cpu_execute_prefetched_instruction(cpu_instruction_t instruction, bool observe_tty)
{
  if (cpu_raise_pending_fetch_exception())
    return false;

  g_cpu_state.pending_ticks++;

  g_cpu_state.current_instruction = instruction;
  g_cpu_state.current_instruction_pc = g_cpu_state.pc;
  g_cpu_state.current_instruction_in_branch_delay_slot = g_cpu_state.next_instruction_is_branch_delay_slot;
  g_cpu_state.current_instruction_was_branch_taken = g_cpu_state.branch_was_taken;
  g_cpu_state.next_instruction_is_branch_delay_slot = false;
  g_cpu_state.branch_was_taken = false;
  g_cpu_state.exception_raised = false;

#ifdef CUPID_DEBUG_RECOMP_TRACE
  {
    static u32 _trace_n = 0;
    if (_trace_n++ < 5000u)
      ERROR_LOG("interp pc=%08X inst=%08X",
                g_cpu_state.current_instruction_pc,
                g_cpu_state.current_instruction.bits);
  }
#endif

  /* Match the interpreter PC trigger to the native block-entry hook. */
  if (cpu_pc_trigger_is_active() &&
      g_cpu_state.pc == cpu_pc_trigger_target_pc())
    cpu_pc_trigger_on_block(g_cpu_state.pc);

  cpu_fetch_instruction();

  if (cpu_diff_is_enabled())
    cpu_diff_record_instruction(g_cpu_state.current_instruction_pc,
                                g_cpu_state.current_instruction.bits);
  cpu_execute_instruction(observe_tty);
  cpu_update_load_delay();
  if (cpu_execution_stats_is_enabled())
    cpu_execution_stats_get()->interpreted_instructions++;
  if (cpu_diff_after_instruction())
    cpu_exit_execution();
  return true;
}

void cpu_core_execute_interpreter_instruction(void)
{
  cpu_execute_prefetched_instruction(g_cpu_state.next_instruction, true);
}

bool cpu_get_prefetched_instruction(u32 pc, u32* word)
{
  if (!s_locals.prefetch_valid || s_locals.prefetched_pc != pc || g_cpu_state.next_instruction_fetch_exception)
    return false;
  *word = g_cpu_state.next_instruction.bits;
  return true;
}

u32 cpu_native_prepare_instruction(u32 pc, u32 word, u32 flags)
{
  const bool delay_slot = (flags & CPU_NATIVE_FETCH_DELAY_SLOT) != 0;
  const u32  next_pc    = delay_slot ? g_cpu_state.pc : pc + CPU_INSTRUCTION_SIZE;
  const bool changed =
      !s_locals.prefetch_valid || s_locals.prefetched_pc != pc || g_cpu_state.next_instruction.bits != word;
  if (g_cpu_state.pending_ticks >= g_cpu_state.downcount || g_cpu_state.next_instruction_fetch_exception || changed) {
    g_cpu_state.pc                                    = s_locals.prefetched_pc;
    g_cpu_state.npc                                   = next_pc;
    g_cpu_state.next_instruction_is_branch_delay_slot = delay_slot;
    g_cpu_state.branch_was_taken                      = (flags & CPU_NATIVE_FETCH_BRANCH_TAKEN) != 0;
    return changed ? CPU_NATIVE_FETCH_RECOMPILE : CPU_NATIVE_FETCH_DISPATCH;
  }

  const u32 wire_pc = g_cpu_state.pc;
  g_cpu_state.pending_ticks++;
  g_cpu_state.current_instruction.bits                 = word;
  g_cpu_state.current_instruction_pc                   = pc;
  g_cpu_state.current_instruction_in_branch_delay_slot = delay_slot;
  g_cpu_state.current_instruction_was_branch_taken     = (flags & CPU_NATIVE_FETCH_BRANCH_TAKEN) != 0;
  g_cpu_state.next_instruction_is_branch_delay_slot    = false;
  g_cpu_state.branch_was_taken                         = false;
  g_cpu_state.exception_raised                         = false;
  g_cpu_state.npc                                      = next_pc;
  cpu_fetch_instruction();
  /* Native branch code uses pc to carry its resolved target into the slot. */
  g_cpu_state.pc = wire_pc;
  return CPU_NATIVE_FETCH_READY;
}

void cpu_native_prepare_dispatch(void)
{
  if (!s_locals.prefetch_valid) {
    g_cpu_state.npc = g_cpu_state.pc;
    cpu_fetch_instruction();
  }
  g_cpu_state.pc = s_locals.prefetched_pc;
  for (;;) {
    if (g_cpu_state.pending_ticks >= g_cpu_state.downcount) timing_events_run_events();
    if (cpu_raise_pending_fetch_exception()) continue;
    if (!g_cpu_state.next_instruction_is_branch_delay_slot) return;
    cpu_core_execute_interpreter_instruction();
  }
}

void cpu_native_execute_prepared_instruction(void)
{
  g_cpu_state.pc = s_locals.prefetched_pc;
  cpu_execute_instruction(false);
  cpu_update_load_delay();
  if (cpu_execution_stats_is_enabled()) cpu_execution_stats_get()->interpreted_instructions++;
}

cpu_cached_block_result_t cpu_core_execute_cached_block(const cpu_code_cache_block_t* block)
{
  const cpu_instruction_t* instructions = cpu_code_cache_block_instructions_const(block);
  cpu_cached_block_result_t result = {0};
  for (u32 i = 0; i < block->size; i++) {
    /* Fetch still models the emulated instruction cache and pipeline. A
     * changed block or a prefetched word that differs from RAM must leave
     * this block before consuming another cached instruction. */
    if (block->state != CPU_CODE_CACHE_BS_VALID ||
        g_cpu_state.pc != block->pc + i * CPU_INSTRUCTION_SIZE ||
        g_cpu_state.next_instruction.bits != instructions[i].bits)
      break;
    result.steps++;
    if (cpu_execute_prefetched_instruction(instructions[i], true)) result.instructions++;
    if (g_cpu_state.exception_raised || g_cpu_state.pending_ticks >= g_cpu_state.downcount)
      break;
  }
  return result;
}

NORETURN static void cpu_execute_impl(void)
{
  if (g_cpu_state.pending_ticks >= g_cpu_state.downcount)
    timing_events_run_events();

  for (;;) {
    do {
      cpu_core_execute_interpreter_instruction();
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
  const cpu_execution_mode_t requested = g_settings.cpu_execution_mode;
  const bool native = cpu_code_cache_is_using_recompiler();
  const bool was_native = !g_cpu_state.using_interpreter;
  /* A native block starts at an instruction boundary outside a branch slot.
   * Finish pending slots with the same prefetch/load-delay path that produced
   * them. Event callbacks can interrupt this work through cpu_execute's scope. */
  if (native) {
    while (g_cpu_state.next_instruction_is_branch_delay_slot) {
      if (g_cpu_state.pending_ticks >= g_cpu_state.downcount)
        timing_events_run_events();
      cpu_core_execute_interpreter_instruction();
    }
    cpu_raise_pending_fetch_exception();
  }

  if (s_locals.current_execution_mode == requested && native == was_native)
    return;

  g_cpu_state.bus_error = false;
  if (!s_locals.prefetch_valid) {
    g_cpu_state.npc = g_cpu_state.pc;
    (void)cpu_fetch_instruction();
  }

  s_locals.current_execution_mode = requested;
  g_cpu_state.using_interpreter = !native;
  if (requested != CPU_EXECUTION_MODE_INTERPRETER)
    cpu_code_cache_reset();
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

void cpu_execute_gte(u32 instruction)
{
  cpu_add_gte_ticks(gte_get_instruction_cycles(instruction));
  gte_execute(instruction);
}

void cpu_recompiler_thunk_interpret_instruction(void)
{
  if (cpu_diff_is_enabled())
    cpu_diff_record_instruction(g_cpu_state.current_instruction_pc,
                                g_cpu_state.current_instruction.bits);
  g_cpu_state.exception_raised = false;
  cpu_execute_instruction(true);
  cpu_update_load_delay();
  if (cpu_execution_stats_is_enabled())
    cpu_execution_stats_get()->interpreted_instructions++;
}

static void cpu_recompiler_run_uncached_block_impl(bool observe_tty)
{
  cpu_native_prepare_dispatch();

  bool in_branch_delay_slot = false;
  for (;;) {
    const bool branch = cpu_is_branch_instruction(g_cpu_state.next_instruction);
    cpu_execute_prefetched_instruction(g_cpu_state.next_instruction, observe_tty);
    observe_tty = true;

    if (g_cpu_state.exception_raised || g_cpu_state.pending_ticks >= g_cpu_state.downcount ||
        (!branch && in_branch_delay_slot) || cpu_is_exit_block_instruction(g_cpu_state.current_instruction))
      break;

    if ((g_cpu_state.current_instruction.bits & 0xFFC0FFFFu) == 0x40806000u &&
        cpu_has_pending_interrupt())
      break;

    in_branch_delay_slot = branch;
  }
}

void cpu_recompiler_run_uncached_block(void)
{
  cpu_recompiler_run_uncached_block_impl(true);
}

void cpu_recompiler_run_uncached_block_after_tty(void)
{
  cpu_recompiler_run_uncached_block_impl(false);
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
  if (setjmp(s_locals.exit_jmp_buf) != 0)
    return;

  cpu_raise_pending_fetch_exception();
  cpu_check_for_execution_mode_change();

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
                                    bool allow_device_reads)
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

  if (allow_device_reads &&
      ((address >= BUS_DMA_BASE && address < BUS_DMA_BASE + BUS_DMA_SIZE) ||
       (address >= BUS_SPU_BASE && address < BUS_SPU_BASE + BUS_SPU_SIZE))) {
    /* Instruction reads use the bus even while data-cache isolation is set.
     * Speculative decoding never enters this path. */
    bus_memory_read_handler_t* handlers =
      bus_offset_read_handler_array(bus_get_memory_handlers(false, false), MEMORY_ACCESS_SIZE_WORD);
    for (u32 i = 0; i < word_count; i++, address += sizeof(u32))
      data[i] = handlers[address >> BUS_MEMORY_LUT_PAGE_SHIFT](address);
    return true;
  }
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

static u32 cpu_icache_refill_word_count(virtual_memory_address_t address)
{
  const u32 offset = cpu_get_icache_line_word_offset(address);
  /* Two-word mode shortens a miss at word zero; other offsets fill to the line end. */
  return g_cpu_state.cache_control.icache_fill_size == 0 && offset == 0 ? 2u : CPU_ICACHE_WORDS_PER_LINE - offset;
}

tick_count_t cpu_get_icache_fill_ticks(virtual_memory_address_t address)
{
  address &= CPU_KSEG_MASK;
  if (address < BUS_RAM_MIRROR_END) return (tick_count_t)cpu_icache_refill_word_count(address);
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_MIRROR_SIZE))
    return (tick_count_t)(bus_get_bios_access_time()[MEMORY_ACCESS_SIZE_WORD] *
                          (s32)cpu_icache_refill_word_count(address));
  return 0;
}

void cpu_check_and_update_icache_tags(u32 line_count)
{
  virtual_memory_address_t current_pc = g_cpu_state.pc & CPU_ICACHE_TAG_ADDRESS_MASK;
  tick_count_t ticks = 0;
  const tick_count_t cached_ticks_per_line = cpu_get_icache_fill_ticks(current_pc);
  for (u32 i = 0; i < line_count; i++, current_pc += CPU_ICACHE_LINE_SIZE) {
    const u32 line = cpu_get_icache_line(current_pc);
    const u32 tag = cpu_get_icache_tag_for_address(current_pc);
    if (g_cpu_state.icache_tags[line] != tag) {
      g_cpu_state.icache_tags[line] = tag;
      ticks += cached_ticks_per_line;
    }
  }
  g_cpu_state.pending_ticks += (u32)ticks;
}

bool cpu_fill_icache(virtual_memory_address_t address, u32* value)
{
  const u32 line = cpu_get_icache_line(address);
  const u32 line_word_offset = cpu_get_icache_line_word_offset(address);
  const u32  word_count       = cpu_icache_refill_word_count(address);
  u32* const line_data = g_cpu_state.icache_data + (line * CPU_ICACHE_WORDS_PER_LINE);
  u32* const offset_line_data = line_data + line_word_offset;
  if (!cpu_do_instruction_read(address, offset_line_data, true, true, word_count, true)) {
    g_cpu_state.icache_tags[line] = CPU_ICACHE_INVALID_BITS;
    return false;
  }
  const u32 tag     = cpu_get_icache_tag_for_address(address);
  const u32 old_tag = g_cpu_state.icache_tags[line];
  const u32 invalid =
      (old_tag & CPU_ICACHE_TAG_ADDRESS_MASK) == tag ? old_tag & CPU_ICACHE_INVALID_BITS : CPU_ICACHE_INVALID_BITS;
  const u32 filled              = ((1u << word_count) - 1u) << line_word_offset;
  g_cpu_state.icache_tags[line] = tag | (invalid & ~filled);
  *value = offset_line_data[0];
  return true;
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
  const physical_memory_address_t address = g_cpu_state.npc;
  u32 instruction = 0;
  u8 exception = 0;
  if (!IsAlignedPow2(address, 4u)) {
    exception = CPU_EXCEPTION_ADEL;
  } else switch (address >> 29) {
    case 0x00: /* KUSEG 0M-512M */
    case 0x04: /* KSEG0   cached */
      if (!g_cpu_state.cache_control.icache_enable ||
          (!g_cpu_state.using_interpreter && !g_settings.cpu_recompiler_icache)) {
        if (!cpu_do_instruction_read(address, &instruction, true, false, 1u, true)) exception = CPU_EXCEPTION_IBE;
      } else if (cpu_compare_icache_tag(address))
        instruction = cpu_read_icache(address);
      else if (!cpu_fill_icache(address, &instruction))
        exception = CPU_EXCEPTION_IBE;
      break;

    case 0x05: /* KSEG1 uncached */
      if (!cpu_do_instruction_read(address, &instruction, true, false, 1u, true))
        exception = CPU_EXCEPTION_IBE;
      break;

    case 0x01: case 0x02: case 0x03: /* KUSEG > 512M */
    case 0x06: case 0x07: default:   /* KSEG2 */
      exception = CPU_EXCEPTION_IBE;
      break;
  }
  g_cpu_state.next_instruction.bits = instruction;
  g_cpu_state.next_instruction_fetch_exception = exception;
  s_locals.prefetched_pc                       = address;
  s_locals.prefetch_valid                      = true;
  g_cpu_state.pc = g_cpu_state.npc;
  g_cpu_state.npc += sizeof(g_cpu_state.next_instruction.bits);
  return exception == 0;
}

static bool cpu_raise_pending_fetch_exception(void)
{
  const u8 exception = g_cpu_state.next_instruction_fetch_exception;
  if (exception == 0)
    return false;

  const u32 address = g_cpu_state.pc;
  const bool delay_slot = g_cpu_state.next_instruction_is_branch_delay_slot;
  const bool taken = g_cpu_state.branch_was_taken;
  const u32 target = g_cpu_state.npc;
  g_cpu_state.pending_ticks++;
  if (exception == CPU_EXCEPTION_ADEL)
    g_cpu_state.cop0_regs.BadVaddr = address;
  cpu_raise_exception_bits(cpu_cop0_cause_make((cpu_exception_t)exception, delay_slot, taken, 0u), address);
  if (delay_slot)
    g_cpu_state.cop0_regs.TAR = target;
  return true;
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
