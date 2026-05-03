/*
 * Public surface for the CPU code-cache / recompiler driver.  Mirrors the
 * CPU::CodeCache namespace one-for-one in name and contract.
 *
 * Provides the public block + instruction-info types so the recompiler base
 * layer can typecheck against them, plus the block pool, LUT, dispatcher
 * entry points, and JIT memory allocator.
 */

#ifndef CUPID_CORE_CPU_CODE_CACHE_H
#define CUPID_CORE_CPU_CODE_CACHE_H

#include "common/types.h"
#include "cpu_types.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Error Error;

enum {
  CPU_CODE_CACHE_LUT_TABLE_COUNT  = 0x10000u,
  CPU_CODE_CACHE_LUT_TABLE_SIZE   = 0x10000u / 4u, /* 16384 entries per page */
  CPU_CODE_CACHE_LUT_TABLE_SHIFT  = 16u,
  CPU_CODE_CACHE_MAX_BLOCK_EXIT_LINKS = 2u,
};

typedef enum {
  CPU_CODE_CACHE_RI_LIVE    = (1u << 0),
  CPU_CODE_CACHE_RI_USED    = (1u << 1),
  CPU_CODE_CACHE_RI_LASTUSE = (1u << 2),
} cpu_code_cache_reg_info_flag_t;

typedef struct cpu_code_cache_inst_info {
  /* 9 1-bit fields packed for layout parity. */
  u8 is_branch_instruction              : 1;
  u8 is_direct_branch_instruction       : 1;
  u8 is_unconditional_branch_instruction: 1;
  u8 is_branch_delay_slot               : 1;
  u8 is_load_instruction                : 1;
  u8 is_store_instruction               : 1;
  u8 is_load_delay_slot                 : 1;
  u8 is_last_instruction                : 1;
  u8 has_load_delay                     : 1;

  u8 reg_flags[CPU_REG_COUNT];
  cpu_reg_t read_reg[3];
} cpu_code_cache_inst_info_t;

enum { CPU_CODE_CACHE_WRITE_DEAD_VALUES = 1 };

ALWAYS_INLINE bool cpu_code_cache_inst_used_test(const cpu_code_cache_inst_info_t* ii, cpu_reg_t r)
{
  return (ii->reg_flags[(u32)r] & (CPU_CODE_CACHE_RI_USED | CPU_CODE_CACHE_RI_LASTUSE))
         == CPU_CODE_CACHE_RI_USED;
}
ALWAYS_INLINE bool cpu_code_cache_inst_live_test(const cpu_code_cache_inst_info_t* ii, cpu_reg_t r)
{
  return CPU_CODE_CACHE_WRITE_DEAD_VALUES ||
         ((ii->reg_flags[(u32)r] & CPU_CODE_CACHE_RI_LIVE) != 0u);
}
ALWAYS_INLINE bool cpu_code_cache_inst_rename_test(const cpu_code_cache_inst_info_t* ii, cpu_reg_t r)
{
  return (r == (cpu_reg_t)0) || !cpu_code_cache_inst_used_test(ii, r) ||
         !cpu_code_cache_inst_live_test(ii, r);
}
ALWAYS_INLINE bool cpu_code_cache_inst_reads_reg(const cpu_code_cache_inst_info_t* ii, cpu_reg_t r)
{
  return ii->read_reg[0] == r || ii->read_reg[1] == r || ii->read_reg[2] == r;
}

typedef enum : u8 {
  CPU_CODE_CACHE_BS_VALID,
  CPU_CODE_CACHE_BS_INVALIDATED,
  CPU_CODE_CACHE_BS_NEEDS_RECOMPILE,
  CPU_CODE_CACHE_BS_FALLBACK_TO_INTERPRETER,
} cpu_code_cache_block_state_t;

typedef enum : u8 {
  CPU_CODE_CACHE_BF_NONE                    = 0u,
  CPU_CODE_CACHE_BF_CONTAINS_LOADSTORE      = (1u << 0),
  CPU_CODE_CACHE_BF_SPANS_PAGES             = (1u << 1),
  CPU_CODE_CACHE_BF_BRANCH_DELAY_SPANS_PAGES= (1u << 2),
  CPU_CODE_CACHE_BF_IS_USING_ICACHE         = (1u << 3),
  CPU_CODE_CACHE_BF_NEEDS_DYNAMIC_FETCH_TICKS = (1u << 4),
} cpu_code_cache_block_flags_t;

typedef enum : u8 {
  CPU_CODE_CACHE_PROT_WRITE_PROTECTED,
  CPU_CODE_CACHE_PROT_MANUAL_CHECK,
  CPU_CODE_CACHE_PROT_UNPROTECTED,
} cpu_code_cache_page_protection_mode_t;

/* Forward decl for block-link-map iterator handle (storage owned by 6.D.1). */
typedef struct cpu_code_cache_block_link_entry cpu_code_cache_block_link_entry_t;

 /* Block storage record.  Native code emits behind `host_code`; followed in
 * memory by `Instruction[size]` and then `InstructionInfo[size]` arrays
 * (allocated by the bump allocator in 6.D.1). */
typedef struct cpu_code_cache_block {
  u32 pc;
  u32 size; /* in guest instructions */
  const void* host_code;

  struct cpu_code_cache_block* next_block_in_page;

  cpu_code_cache_block_link_entry_t* exit_links[CPU_CODE_CACHE_MAX_BLOCK_EXIT_LINKS];
  u8 num_exit_links;

  cpu_code_cache_block_state_t          state;
  cpu_code_cache_block_flags_t          flags;
  cpu_code_cache_page_protection_mode_t protection;

  tick_count_t uncached_fetch_ticks;
  u32 icache_line_count;

  u32 host_code_size;
  u32 compile_frame;
  u8  compile_count;
} cpu_code_cache_block_t;

ALWAYS_INLINE cpu_instruction_t* cpu_code_cache_block_instructions(cpu_code_cache_block_t* b)
{
  return (cpu_instruction_t*)(b + 1);
}
ALWAYS_INLINE const cpu_instruction_t*
cpu_code_cache_block_instructions_const(const cpu_code_cache_block_t* b)
{
  return (const cpu_instruction_t*)(b + 1);
}
ALWAYS_INLINE cpu_code_cache_inst_info_t*
cpu_code_cache_block_instructions_info(cpu_code_cache_block_t* b)
{
  return (cpu_code_cache_inst_info_t*)(cpu_code_cache_block_instructions(b) + b->size);
}
ALWAYS_INLINE const cpu_code_cache_inst_info_t*
cpu_code_cache_block_instructions_info_const(const cpu_code_cache_block_t* b)
{
  return (const cpu_code_cache_inst_info_t*)(cpu_code_cache_block_instructions_const(b) + b->size);
}
ALWAYS_INLINE bool cpu_code_cache_block_has_flag(const cpu_code_cache_block_t* b,
                                                 cpu_code_cache_block_flags_t f)
{
  return ((u32)b->flags & (u32)f) != 0u;
}

/* Returns true if any recompiler is in use (currently always false until 6.D). */
bool cpu_code_cache_is_using_recompiler(void);

/* Returns true if the recompiler is using fastmem (always false until 6.J). */
bool cpu_code_cache_is_using_fastmem(void);

/* Allocates resources, call once at startup. */
bool cpu_code_cache_process_startup(Error* error);

/* Frees resources, call once at shutdown. */
void cpu_code_cache_process_shutdown(void);

/* Runs the system.  Does not return.  When a recompiler backend is wired in,
 * this enters the dispatcher; until then it falls through to the interpreter
 * so RECOMPILER / CACHED_INTERPRETER mode boots at interpreter speed. */
NORETURN void cpu_code_cache_execute(void);

/* Flushes the code cache, forcing all blocks to be recompiled. */
void cpu_code_cache_reset(void);

/* Free non-persistent resources. */
void cpu_code_cache_shutdown(void);

/* Invalidates all blocks in the range of the specified RAM code page. */
void cpu_code_cache_invalidate_blocks_with_page_index(u32 page_index);

/* Invalidates every block backed by RAM. */
void cpu_code_cache_invalidate_all_ram_blocks(void);

/* JIT code-buffer allocator (used by backend during compile_block). */
u8*  cpu_code_cache_get_free_code_pointer    (void);
u32  cpu_code_cache_get_free_code_space      (void);
u8*  cpu_code_cache_get_free_far_code_pointer(void);
u32  cpu_code_cache_get_free_far_code_space  (void);
void cpu_code_cache_commit_code              (u32 length);
void cpu_code_cache_commit_far_code          (u32 length);
void cpu_code_cache_align_code               (u32 alignment);

/* Code-LUT entry table base (used by the dispatcher's PC→host lookup).
 * `g_code_lut[pc>>16][(pc&0xFFFF)>>2]` resolves to the host code pointer. */
extern const void** g_code_lut[CPU_CODE_CACHE_LUT_TABLE_COUNT];

/* Dispatcher entry points emitted at startup by the backend in 6.D.2.  All
 * NULL until backend registers itself. */
extern NORETURN_FUNCTION_POINTER void (*g_enter_recompiler)(void);
extern const void* g_compile_or_revalidate_block;
extern const void* g_run_events_and_dispatch;
extern const void* g_dispatcher;
extern const void* g_interpret_block;
extern const void* g_discard_and_recompile_block;

const void* cpu_code_cache_dispatch_lookup(u32 pc);

/* Block-link / dispatch entry points called by the dispatcher and backends. */
void cpu_code_cache_compile_or_revalidate_block (u32 start_pc);
void cpu_code_cache_discard_and_recompile_block (u32 start_pc);
const void* cpu_code_cache_create_block_link    (cpu_code_cache_block_t* from_block,
                                                 void* code, u32 newpc);
const void* cpu_code_cache_create_self_block_link(cpu_code_cache_block_t* b, void* code,
                                                  const void* block_start);

/* Backend registers this so backlink_blocks can rewrite jmp sites in place. */
void cpu_code_cache_set_emit_jump(void (*fn)(void* site, const void* dst, bool flush_icache));

/* Backend registers a hook for emitting the dispatcher prelude into the
 * head of the code cache.  Called once at first reset after registration
 * and again on each subsequent cpu_code_cache_reset.  `code` is a writable
 * pointer into the JIT region; `code_size` is bytes available.  Returns
 * the number of bytes emitted; the caller commits them. */
void cpu_code_cache_set_emit_asm_functions(u32 (*fn)(void* code, u32 code_size));

bool cpu_code_cache_has_previously_faulted_on_pc(u32 guest_pc);

/* Records a fastmem load/store call site so a SIGSEGV later can locate the
 * faulting JIT site, marshal arguments to the slow C thunk, and patch the
 * site in place to a JMP rel32 to a far-code stub.  M2 wires the table;
 * M4a installs the actual SIGSEGV handler that consumes it. */
void cpu_code_cache_add_load_store_info(void* code_address, u32 code_size,
                                         u32 guest_pc, u32 guest_block,
                                        tick_count_t cycles, u32 gpr_bitmask, 
                                        u8 address_register, u8 data_register,
                                        memory_access_size_t size,
                                        bool is_signed, bool is_load);

/* Backpatch metadata for one fastmem load/store call site.  Sorted by
 * `code_address` in `s_fastmem_backpatch_info` for O(log n) lookup from
 * the SIGSEGV handler. */
typedef struct {
  void*               code_address;
  u32                 code_size;
  u32                 guest_pc;
  u32                 guest_block;
  tick_count_t        cycles;
  u32                 gpr_bitmask;
  memory_access_size_t size;
  u8                  address_register;
  u8                  data_register;
  u8                  is_signed;
  u8                  is_load;
} cpu_code_cache_loadstore_backpatch_info_t;

/* Look up the backpatch entry whose JIT site contains `host_pc`.  Returns
 * NULL if no recorded site overlaps that address.  Signal-safe. */
const cpu_code_cache_loadstore_backpatch_info_t*
cpu_code_cache_find_backpatch_info(const void* host_pc);

/* Drop every backpatch entry whose `code_address` lies in `[start, start+size)`.
 * Called when a block is freed / reset so its entries don't dangle. */
void cpu_code_cache_remove_backpatch_info_for_range(const void* start, u32 size);

/* SIGSEGV handler entry point for fastmem-induced faults.  Forwarded by
 * page_fault_handler.c once cpu_code_cache_process_startup registers it.
 * Returns CONTINUE_EXECUTION when the fault was a fastmem hit and we
 * patched the JIT site to the slow path; EXECUTE_NEXT otherwise so the
 * crash handler chain can produce a backtrace. */
struct page_fault_handler_result;
int cpu_code_cache_handle_fastmem_exception(void* exception_pc, void* fault_address, bool is_write);

/* Diagnostic ring: last 64 compiled blocks (pc, size, host_code).  Each
 * entry is logged in `create_block`.  Shows what the recompiler has been
 * emitting recently; useful when debugging wild-PC dispatch faults
 * (`cpu_recompiler_synth_pc_exception_if_invalid` triggers).  Mirrors
 * `cdrom_dbg_dump_recent_commands` and `gpu_dbg_dump_recent_gp1`. */
#include <stdio.h>
void cpu_code_cache_dbg_dump_recent_blocks(FILE* out);

/* Marker for an AdEL/IBE synthesis triggered by the dispatcher safety net.
 * Logs cpu_state context (PC sources, key cop0 + GPRs) the first N times,
 * then flips quiet.  Defined in cpu_code_cache.c. */
void cpu_code_cache_dbg_log_safety_net_exception(u32 bad_pc);

/* When CUPID_TRACE_LOW_RAM=1, install a tap that logs slow-path RAM
 * writes with paddr<0x100 along with cpu_state.  Used to find the rogue
 * `sw $ra, 0x38(sp)` that corrupts the BIOS exception save area.  No
 * effect under CUPID_RECOMP_DIFF=1 (diff harness owns the tap). */
void cpu_code_cache_dbg_maybe_install_low_ram_watch(void);

/* Dump the low-RAM ring (when CUPID_TRACE_LOW_RAM_RING=N is set).
 * Called from frontend dump_debug_ringbuffers (SIGINT / atexit / crash). */
void cpu_code_cache_dbg_low_ram_dump(void);

/* Per-block execution ring.  When CUPID_TRACE_EXEC=1, the x64 backend
 * emits a call to `cpu_code_cache_dbg_record_exec(pc)` at the head of every
 * block prologue.  The ring is dumped at safety-net trigger.  Cost: one
 * indirect call per block exec (only when env var is set). */
void cpu_code_cache_dbg_init_exec_trace(void);
bool cpu_code_cache_exec_trace_is_active(void);
void cpu_code_cache_dbg_record_exec(u32 pc);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_CODE_CACHE_H */
