/*
 * R3000A interpreter public surface.  The CPU is a process-wide singleton on
 * PS1 (one core, no SMP), so register state lives in the global g_cpu_state.
 *
 *  - PGXP plumbing (cpu_pgxp deferred).
 *  - Cached interpreter / dynarec integration (interpreter is the only path).
 *  - Breakpoint/debugger surface (tied to host UI; will resurface when
 *    a debugger frontend lands).
 *  - fastmem; we keep the field on the state struct for ABI parity with
 *    future ports but never populate or use it.
 */

#ifndef CUPID_CORE_CPU_CORE_H
#define CUPID_CORE_CPU_CORE_H

#include "cpu_pgxp.h"
#include "cpu_types.h"
#include "types.h"

#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;

/* PS1 boot vector: BIOS ROM in KSEG1 (uncached). */
enum {
  CPU_RESET_VECTOR = 0xBFC00000u,
};

enum {
  CPU_SCRATCHPAD_ADDR        = 0x1F800000u,
  CPU_SCRATCHPAD_ADDR_MASK   = 0x7FFFFC00u,
  CPU_SCRATCHPAD_OFFSET_MASK = 0x000003FFu,
  CPU_SCRATCHPAD_SIZE        = 0x00000400u,
};

/* I-cache (4 KB direct-mapped, 16-byte lines, 4 words/line). */
enum {
  CPU_ICACHE_SIZE             = 0x00001000u,
  CPU_ICACHE_SLOTS            = CPU_ICACHE_SIZE / 4u,
  CPU_ICACHE_LINE_SIZE        = 16u,
  CPU_ICACHE_LINES            = CPU_ICACHE_SIZE / CPU_ICACHE_LINE_SIZE,
  CPU_ICACHE_WORDS_PER_LINE   = CPU_ICACHE_SLOTS / CPU_ICACHE_LINES,
  CPU_ICACHE_TAG_ADDRESS_MASK = 0xFFFFFFF0u,
  CPU_ICACHE_INVALID_BITS     = 0x0Fu,
};

/* CACHECTL ($FFFE0130) bit layout. */
typedef union {
  u32 bits;
  struct {
    unsigned int lock_mode : 1;
    unsigned int invalidate_mode : 1;
    unsigned int tag_test_mode : 1;
    unsigned int dcache_scratchpad : 1;
    unsigned int : 3;
    unsigned int dcache_enable : 1;
    unsigned int icache_fill_size : 2;
    unsigned int : 1;
    unsigned int icache_enable : 1;
  };
} cpu_cache_control_t;

_Static_assert(sizeof(cpu_cache_control_t) == 4, "cpu_cache_control_t must be 32 bits");

/* GTE register file shadow.  The interpreter copies COP2 reads/writes through
 * gte_*; this struct is opaque to cpu_core.c.  Keeping a u32[64] inline lets
 * future ARM dynarecs reach the regs with one offset. */
typedef struct {
  union {
    u32 r32[64];
  };
} cpu_gte_regs_t;

/* CPU state struct: layout is part of the dynarec ABI so that
 * future dynarec ports can rely on stable offsets. */
typedef struct {
  /* ticks the CPU has executed */
  u32 downcount;
  u32 pending_ticks;
  u32 gte_completion_tick;
  u32 muldiv_completion_tick;

  cpu_registers_t regs;
  cpu_cop0_registers_t cop0_regs;

  u32 pc;  /* execution time: address of the next instruction to execute (already fetched) */
  u32 npc; /* execution time: address of the next instruction to fetch */

  /* address of the instruction currently being executed */
  cpu_instruction_t current_instruction;
  u32 current_instruction_pc;
  bool current_instruction_in_branch_delay_slot;
  bool current_instruction_was_branch_taken;
  bool next_instruction_is_branch_delay_slot;
  bool branch_was_taken;
  bool exception_raised;
  bool bus_error;

  /* load-delay slot bookkeeping.  MIPS R3000A defers the value of an
   * LB/LH/LW/etc by one instruction; the destination reads its old value
   * during the delay slot.  load_delay_reg/value hold the pending write
   * applied at the *end* of the next instruction; next_load_delay_*
   * captures any new load issued during the delay slot. */
  cpu_reg_t load_delay_reg;
  cpu_reg_t next_load_delay_reg;
  u32 load_delay_value;
  u32 next_load_delay_value;

  cpu_instruction_t next_instruction;
  cpu_cache_control_t cache_control;

  /* GTE state lives here so dynarecs can fetch it with a single base+offset. */
  cpu_gte_regs_t gte_regs;

  bool using_interpreter;
  bool using_debug_dispatcher;

  void* fastmem_base;
  void** memory_handlers;

  u32 icache_tags[CPU_ICACHE_LINES];
  u32 icache_data[CPU_ICACHE_LINES * CPU_ICACHE_WORDS_PER_LINE];
  u8  scratchpad[CPU_SCRATCHPAD_SIZE];

  pgxp_value_t pgxp_gpr [PGXP_GPR_COUNT];   /* 32 GPRs + HI + LO */
  pgxp_value_t pgxp_cop0[PGXP_COP0_COUNT];
  pgxp_value_t pgxp_gte [PGXP_GTE_COUNT];   /* 32 data + 32 ctrl */
} cpu_state_t;

extern cpu_state_t g_cpu_state;

void cpu_initialize(void);
void cpu_shutdown(void);
void cpu_reset(void);
bool cpu_do_state(state_wrapper_t* sw);
void cpu_clear_icache(void);

/* Per-instruction fetch cost at `address` (BIOS access time, RAM read
 * ticks, etc).  cpu_diff_block uses this to subtract step_for_diff's
 * extra seed-fetch cost from the interp side of the per-block tick
 * compare. */
tick_count_t cpu_get_instruction_read_ticks(virtual_memory_address_t address);

cpu_execution_mode_t cpu_get_current_execution_mode(void);
bool cpu_update_debug_dispatcher_flag(void);
void cpu_update_memory_pointers(void);

/* Runs the interpreter loop. */
void cpu_execute(void);

/* Long-jumps out of the dispatcher.  Must be called from within cpu_execute
 * (or a callback it triggered). */
NORETURN void cpu_exit_execution(void);

ALWAYS_INLINE cpu_registers_t* cpu_get_regs(void)
{
  return &g_cpu_state.regs;
}

ALWAYS_INLINE u32 cpu_get_pending_ticks(void)
{
  return g_cpu_state.pending_ticks;
}

ALWAYS_INLINE void cpu_reset_pending_ticks(void)
{
  g_cpu_state.gte_completion_tick = (g_cpu_state.pending_ticks < g_cpu_state.gte_completion_tick) ?
                                      (g_cpu_state.gte_completion_tick - g_cpu_state.pending_ticks) : 
                                      0u;
  g_cpu_state.muldiv_completion_tick = (g_cpu_state.pending_ticks < g_cpu_state.muldiv_completion_tick) ?
                                         (g_cpu_state.muldiv_completion_tick - g_cpu_state.pending_ticks) : 
                                         0u;
  g_cpu_state.pending_ticks = 0u;
}

ALWAYS_INLINE void cpu_add_pending_ticks(tick_count_t ticks)
{
  g_cpu_state.pending_ticks += (u32)ticks;
}

ALWAYS_INLINE bool cpu_in_user_mode(void)
{
  return g_cpu_state.cop0_regs.sr.KUc != 0u;
}

ALWAYS_INLINE bool cpu_in_kernel_mode(void)
{
  return g_cpu_state.cop0_regs.sr.KUc == 0u;
}

bool cpu_safe_read_memory_byte(virtual_memory_address_t addr, u8* value);
bool cpu_safe_read_memory_halfword(virtual_memory_address_t addr, u16* value);
bool cpu_safe_read_memory_word(virtual_memory_address_t addr, u32* value);
bool cpu_safe_read_memory_bytes(virtual_memory_address_t addr, void* data, u32 length);
bool cpu_safe_write_memory_byte(virtual_memory_address_t addr, u8 value);
bool cpu_safe_write_memory_halfword(virtual_memory_address_t addr, u16 value);
bool cpu_safe_write_memory_word(virtual_memory_address_t addr, u32 value);
bool cpu_safe_write_memory_bytes(virtual_memory_address_t addr, const void* data, u32 length);
bool cpu_safe_zero_memory_bytes(virtual_memory_address_t addr, u32 length);

/* External IRQ line into COP0_CAUSE.Ip[10] (HW_IRQ from interrupt controller). */
void cpu_set_irq_request(bool state);

/* Lightweight frontend trace counter. */
u32 cpu_dbg_irq_dispatch_count(void);

u32  cpu_get_pc(void);
void cpu_set_bus_error(bool v);

/* mdec.c calls this name; identical semantics to cpu_add_pending_ticks. */
void cpu_core_add_pending_ticks(tick_count_t ticks);

u32  cpu_virtual_to_physical_address(virtual_memory_address_t addr);

u8*  cpu_get_scratchpad_pointer(void);
u32  cpu_get_scratchpad_size(void);
u32  cpu_get_scratchpad_address(void);

u32  cpu_get_cache_control_bits(void);
void cpu_set_cache_control_bits(u32 v);

u32* cpu_get_icache_data(void);
u32* cpu_get_icache_tags(void);

void cpu_code_cache_invalidate_all_ram_blocks(void);

/* Recompiler thunk: interprets a single guest instruction, then returns.
 * Used by the recompiler dispatcher's g_interpret_block path while the
 * x64 backend's per-MIPS hooks are not yet wired (chunks 1-8 of 6.D.2).
 */
void cpu_recompiler_thunk_interpret_instruction(void);

void cpu_recompiler_run_uncached_block(void);

/* Dispatcher safety net: returns true iff `pc` is a legal MIPS instruction
 * fetch target (4-byte aligned AND in KUSEG[0..512M] / KSEG0 / KSEG1).  When
 * `raise_on_fail` is true and the PC is invalid, raises the appropriate
 * exception (AdEL for misalignment, IBE for an unmapped segment) on the
 * live cpu_state so the BIOS handler can take over.  Mirrors the behaviour
 * of cpu_fetch_instruction's prologue without performing the actual fetch.
 *
 * Used by cpu_code_cache_compile_or_revalidate_block and
 * cpu_code_cache_create_block_link to reject wild PCs at the dispatcher
 * boundary instead of pinning g_interpret_block at the bad LUT slot. */
bool cpu_recompiler_pc_is_executable(u32 pc);
bool cpu_recompiler_synth_pc_exception_if_invalid(u32 pc);

 /* Per-block recomp/interp diff replay: runs exactly `instruction_count`
 * interpreter steps on g_cpu_state, no timing events. */
void cpu_core_step_for_diff(u32 instruction_count);

/* Recompiler memory thunks.  Delegate to the same
 * internal `cpu_read/write_memory_*` paths the interpreter uses, so MMIO
 * side-effects fire and exceptions raise normally.  Return value: zero on
 * failure (g_cpu_state.exception_raised is set; caller must early-exit
 * the JIT block). */
u32  cpu_recompiler_thunk_read_memory_byte    (u32 address);
u32  cpu_recompiler_thunk_read_memory_halfword(u32 address);
u32  cpu_recompiler_thunk_read_memory_word    (u32 address);
void cpu_recompiler_thunk_write_memory_byte    (u32 address, u32 value);
void cpu_recompiler_thunk_write_memory_halfword(u32 address, u32 value);
void cpu_recompiler_thunk_write_memory_word    (u32 address, u32 value);

#endif /* CUPID_CORE_CPU_CORE_H */
