/*
 * `CPU::Recompiler::Recompiler` class; per-arch backends derive from it
 * and override the virtual methods.  In cupid-ps1 we use a vtable-of-function-
 * pointers struct (cpu_recompiler_vtable_t), and the per-arch concrete
 * structs (e.g. cpu_recompiler_x64_t) start with an embedded
 * cpu_recompiler_t followed by their own state.
 *
 * Provides the full arch-agnostic register allocator, load-delay engine,
 * host-state backup/restore, speculative-constant scaffolding, the
 * per-MIPS-op compile_*_const helpers, and the compile_template /
 * compile_loadstore_template / compile_instruction dispatch.  Backends wire
 * themselves into the vtable.
 */

#ifndef CUPID_CORE_CPU_RECOMPILER_H
#define CUPID_CORE_CPU_RECOMPILER_H

#include "common/types.h"
#include "cpu_code_cache.h"
#include "cpu_types.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* x86_64 host: 16 GPRs, supports r/m-with-memory operand on most ops. */
enum {
  CPU_RECOMP_NUM_HOST_REGS                = 16u,
  CPU_RECOMP_HAS_MEMORY_OPERANDS          = 1u,
  CPU_RECOMP_FUNCTION_ALIGNMENT           = 16u,
  CPU_RECOMP_MAX_NEAR_HOST_BYTES_PER_INST = 32u, /* worst-case near-block growth */
  CPU_RECOMP_MIN_CODE_RESERVE_FOR_BLOCK   = 512u,
};

 /* Global compile-time options: EMULATE_LOAD_DELAYS / SWAP_BRANCH_DELAY_SLOTS. */
enum {
  CPU_RECOMP_EMULATE_LOAD_DELAYS     = 1u,
  CPU_RECOMP_SWAP_BRANCH_DELAY_SLOTS = 1u,
};

/* CPU_REG_COUNT must fit the constant-tracking u64 bitset. */
_Static_assert(CPU_REG_COUNT <= 64u, "constant-reg bitset assumes <=64 guest regs");

typedef enum {
  CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS              = (1u << 0),
  CPU_RC_FLUSH_INVALIDATE_MIPS_REGISTERS         = (1u << 1),
  CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS       = (1u << 2),
  CPU_RC_FLUSH_FREE_UNNEEDED_CALLER_SAVED_REGS   = (1u << 3),
  CPU_RC_FLUSH_FREE_ALL_REGISTERS                = (1u << 4),
  CPU_RC_FLUSH_PC                                = (1u << 5),
  CPU_RC_FLUSH_INSTRUCTION_BITS                  = (1u << 6),
  CPU_RC_FLUSH_CYCLES                            = (1u << 7),
  CPU_RC_FLUSH_LOAD_DELAY                        = (1u << 8),
  CPU_RC_FLUSH_LOAD_DELAY_FROM_STATE             = (1u << 9),
  CPU_RC_FLUSH_GTE_DONE_CYCLE                    = (1u << 10),
  CPU_RC_FLUSH_GTE_STALL_FROM_STATE              = (1u << 11),
  CPU_RC_FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS  = (1u << 12),
  /* Stall pending_ticks up to muldiv_completion_tick (state value).  Used
   * by MFHI/MFLO/MTHI/MTLO when r->dirty_muldiv_done_cycle is set (e.g.
   * after a non-const-rs MULT/MULTU stashed completion to state, or
   * cross-block).  Mirrors CPU_RC_FLUSH_GTE_STALL_FROM_STATE. */
  CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE           = (1u << 13),
  /* Flush in-block muldiv_done_cycle to muldiv_completion_tick state at
   * end of block (so cross-block MFHI/MFLO sees the right value).  Mirrors
   * CPU_RC_FLUSH_GTE_DONE_CYCLE. */
  CPU_RC_FLUSH_MULDIV_DONE_CYCLE                 = (1u << 14),
} cpu_recomp_flush_flags_t;

/* Composite flush masks (FLUSH_FOR_* aliases). */
enum {
  CPU_RC_FLUSH_FOR_C_CALL          = CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS,
  CPU_RC_FLUSH_FOR_LOADSTORE       = CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS | CPU_RC_FLUSH_CYCLES |
                                     CPU_RC_FLUSH_INSTRUCTION_BITS,
  CPU_RC_FLUSH_FOR_BRANCH          = CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS,
  CPU_RC_FLUSH_FOR_EXCEPTION       = CPU_RC_FLUSH_CYCLES | CPU_RC_FLUSH_GTE_DONE_CYCLE |
                                     CPU_RC_FLUSH_MULDIV_DONE_CYCLE,
  CPU_RC_FLUSH_FOR_EARLY_BLOCK_EXIT = CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS | CPU_RC_FLUSH_CYCLES |
                                      CPU_RC_FLUSH_GTE_DONE_CYCLE | CPU_RC_FLUSH_MULDIV_DONE_CYCLE |
                                      CPU_RC_FLUSH_PC |
                                      CPU_RC_FLUSH_LOAD_DELAY,
  CPU_RC_FLUSH_FOR_INTERPRETER     = CPU_RC_FLUSH_FLUSH_MIPS_REGISTERS |
                                     CPU_RC_FLUSH_INVALIDATE_MIPS_REGISTERS |
                                     CPU_RC_FLUSH_FREE_CALLER_SAVED_REGISTERS |
                                     CPU_RC_FLUSH_PC | CPU_RC_FLUSH_CYCLES |
                                     CPU_RC_FLUSH_INSTRUCTION_BITS |
                                     CPU_RC_FLUSH_LOAD_DELAY |
                                     CPU_RC_FLUSH_GTE_DONE_CYCLE |
                                     CPU_RC_FLUSH_MULDIV_DONE_CYCLE |
                                     CPU_RC_FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS,
  CPU_RC_FLUSH_END_BLOCK           = 0xFFFFFFFFu &
                                     ~(CPU_RC_FLUSH_PC | CPU_RC_FLUSH_CYCLES |
                                       CPU_RC_FLUSH_GTE_DONE_CYCLE |
                                       CPU_RC_FLUSH_MULDIV_DONE_CYCLE |
                                       CPU_RC_FLUSH_INSTRUCTION_BITS |
                                       CPU_RC_FLUSH_GTE_STALL_FROM_STATE |
                                       CPU_RC_FLUSH_MULDIV_STALL_FROM_STATE |
                                       CPU_RC_FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS),
};

typedef enum {
  CPU_RC_TF_READS_S            = (1u << 0),
  CPU_RC_TF_READS_T            = (1u << 1),
  CPU_RC_TF_READS_LO           = (1u << 2),
  CPU_RC_TF_READS_HI           = (1u << 3),
  CPU_RC_TF_WRITES_D           = (1u << 4),
  CPU_RC_TF_WRITES_T           = (1u << 5),
  CPU_RC_TF_WRITES_LO          = (1u << 6),
  CPU_RC_TF_WRITES_HI          = (1u << 7),
  CPU_RC_TF_COMMUTATIVE        = (1u << 8),
  CPU_RC_TF_CAN_OVERFLOW       = (1u << 9),
  CPU_RC_TF_LOAD_DELAY         = (1u << 10),
  CPU_RC_TF_GTE_STALL          = (1u << 11),
  CPU_RC_TF_NO_NOP             = (1u << 12),
  CPU_RC_TF_NEEDS_REG_S        = (1u << 13),
  CPU_RC_TF_NEEDS_REG_T        = (1u << 14),
  CPU_RC_TF_CAN_SWAP_DELAY_SLOT= (1u << 15),
  CPU_RC_TF_RENAME_WITH_ZERO_T = (1u << 16),
  CPU_RC_TF_RENAME_WITH_ZERO_IMM = (1u << 17),
  CPU_RC_TF_PGXP_WITHOUT_CPU   = (1u << 18),
} cpu_recomp_template_flag_t;

typedef enum : u8 {
  CPU_RC_GTE_REG_IGNORE,
  CPU_RC_GTE_REG_DIRECT,
  CPU_RC_GTE_REG_ZERO_EXTEND_16,
  CPU_RC_GTE_REG_SIGN_EXTEND_16,
  CPU_RC_GTE_REG_CALL_HANDLER,
  CPU_RC_GTE_REG_PUSH_FIFO,
} cpu_recomp_gte_access_action_t;

typedef enum {
  CPU_RC_BC_EQ          = 0,
  CPU_RC_BC_NE          = 1,
  CPU_RC_BC_GT_ZERO     = 2,
  CPU_RC_BC_GE_ZERO     = 3,
  CPU_RC_BC_LT_ZERO     = 4,
  CPU_RC_BC_LE_ZERO     = 5,
} cpu_recomp_branch_cond_t;

typedef enum {
  CPU_RC_HR_ALLOCATED   = (1u << 0),
  CPU_RC_HR_NEEDED      = (1u << 1),
  CPU_RC_HR_MODE_READ   = (1u << 2),
  CPU_RC_HR_MODE_WRITE  = (1u << 3),
  CPU_RC_HR_CALLEE_SAVED= (1u << 6),
  CPU_RC_HR_USABLE      = (1u << 7),
} cpu_recomp_host_reg_flags_t;

enum {
  CPU_RC_HR_ALLOWED_FLAGS    = CPU_RC_HR_MODE_READ | CPU_RC_HR_MODE_WRITE,
  CPU_RC_HR_IMMUTABLE_FLAGS  = CPU_RC_HR_USABLE | CPU_RC_HR_CALLEE_SAVED,
};

typedef enum {
  CPU_RC_HRT_TEMP                  = 0,
  CPU_RC_HRT_CPU_REG               = 1,
  CPU_RC_HRT_PC_WRITEBACK          = 2,
  CPU_RC_HRT_LOAD_DELAY_VALUE      = 3,
  CPU_RC_HRT_NEXT_LOAD_DELAY_VALUE = 4,
  CPU_RC_HRT_MEMBASE               = 5,
} cpu_recomp_host_reg_alloc_type_t;

typedef struct {
  u8                                flags;
  cpu_recomp_host_reg_alloc_type_t  type;
  cpu_reg_t                         reg;
  u16                               counter;
} cpu_recomp_host_reg_alloc_t;

typedef union {
  struct {
    u32 const_s        : 1;
    u32 const_t        : 1;
    u32 const_lo       : 1;
    u32 const_hi       : 1;

    u32 valid_host_d   : 1;
    u32 valid_host_s   : 1;
    u32 valid_host_t   : 1;
    u32 valid_host_lo  : 1;
    u32 valid_host_hi  : 1;

    u32 host_d         : 5;
    u32 host_s         : 5;
    u32 host_t         : 5;
    u32 host_lo        : 5;

    u32 delay_slot_swapped : 1;
    u32 _pad1          : 2;

    u32 host_hi        : 5;
    u32 mips_s         : 5;
    u32 mips_t         : 5;

    u32 _pad2          : 15;
  };
  u64 bits;
} cpu_recomp_compile_flags_t;
_Static_assert(sizeof(cpu_recomp_compile_flags_t) == 8,
               "cpu_recomp_compile_flags_t must pack into 64 bits");

ALWAYS_INLINE cpu_reg_t cpu_recomp_cf_mips_s(cpu_recomp_compile_flags_t cf) { return (cpu_reg_t)cf.mips_s; }
ALWAYS_INLINE cpu_reg_t cpu_recomp_cf_mips_t(cpu_recomp_compile_flags_t cf) { return (cpu_reg_t)cf.mips_t; }

typedef struct {
  bool has;
  u32  val;
} cpu_recomp_spec_value_t;

typedef struct {
  u32                       addr;     /* 0xFFFFFFFF == empty slot */
  cpu_recomp_spec_value_t   value;
} cpu_recomp_spec_mem_entry_t;

#define CPU_RECOMP_SPEC_MEM_EMPTY_KEY UINT32_C(0xFFFFFFFF)

/* Optional handle for vtable hooks expecting std::optional<u32>-like return. */
typedef struct {
  bool has;
  u32  val;
} cpu_recomp_opt_u32_t;

typedef struct {
  s32       cycles;
  s32       gte_done_cycle;
  s32       muldiv_done_cycle;
  u32       compiler_pc;
  bool      dirty_pc;
  bool      dirty_instruction_bits;
  bool      dirty_gte_done_cycle;
  bool      dirty_muldiv_done_cycle;
  bool      block_ended;
  const cpu_instruction_t*       inst;
  cpu_code_cache_inst_info_t*    iinfo;
  u32       current_instruction_pc;
  bool      current_instruction_delay_slot;
  u64       const_regs_valid;
  u64       const_regs_dirty;
  u32       const_regs_values[CPU_REG_COUNT];
  cpu_recomp_host_reg_alloc_t host_regs[CPU_RECOMP_NUM_HOST_REGS];
  u16       register_alloc_counter;
  bool      load_delay_dirty;
  cpu_reg_t load_delay_register;
  u32       load_delay_value_register;
  cpu_reg_t next_load_delay_register;
  u32       next_load_delay_value_register;
} cpu_recomp_state_backup_t;

typedef struct cpu_recompiler        cpu_recompiler_t;
typedef struct cpu_recompiler_vtable cpu_recompiler_vtable_t;

struct cpu_recompiler {
  const cpu_recompiler_vtable_t*    v;        /* arch backend */

  cpu_code_cache_block_t*           block;
  u32                               compiler_pc;
  s32                               cycles;
  s32                               gte_done_cycle;
  /* In-block muldiv completion tracking (offset relative to r->cycles).  When
   * dirty_muldiv_done_cycle is false, the producer wrote a known-static
   * cycle count (DIV/DIVU = 35 ticks, or MULT/MULTU with const rs) into
   * muldiv_done_cycle and the consumer can stall via simple r->cycles
   * adjustment, no state load.  When true, the producer wrote
   * muldiv_completion_tick to state at runtime (MULT/MULTU with non-const
   * rs, or coming from a previous block) and the consumer must flush from
   * state.  Mirrors r->gte_done_cycle / r->dirty_gte_done_cycle. */
  s32                               muldiv_done_cycle;

  const cpu_instruction_t*          inst;
  cpu_code_cache_inst_info_t*       iinfo;
  u32                               current_instruction_pc;
  bool                              current_instruction_branch_delay_slot;
  bool                              branch_delay_slot_swapped;
  /* When compiling a delay slot, set true if the branch was statically known
   * to be taken (uncond J/JAL/JR/JALR, or conditional with const-eval result).
   * end_block_with_exception reads this for the BT bit in cause when raising
   * a delay-slot exception. */
  bool                              delay_slot_branch_was_taken;

  bool                              dirty_pc;
  bool                              dirty_instruction_bits;
  bool                              dirty_gte_done_cycle;
  bool                              dirty_muldiv_done_cycle;
  bool                              block_ended;

  u64                               constant_regs_valid;
  u64                               constant_regs_dirty;
  u32                               constant_reg_values[CPU_REG_COUNT];

  cpu_recomp_host_reg_alloc_t       host_regs[CPU_RECOMP_NUM_HOST_REGS];
  u16                               register_alloc_counter;

  bool                              load_delay_dirty;
  cpu_reg_t                         load_delay_register;
  u32                               load_delay_value_register;
  cpu_reg_t                         next_load_delay_register;
  u32                               next_load_delay_value_register;

  cpu_recomp_state_backup_t         host_state_backup[2];
  u32                               host_state_backup_count;

  /* Speculative constants. */
  cpu_recomp_spec_value_t           spec_regs[CPU_REG_COUNT];
  cpu_recomp_spec_value_t           spec_cop0_sr;
  cpu_recomp_spec_mem_entry_t*      spec_mem_entries;
  u32                               spec_mem_count;
  u32                               spec_mem_cap;
};

struct cpu_recompiler_vtable {
  /* Compilation lifecycle */
  void        (*reset)         (cpu_recompiler_t*, cpu_code_cache_block_t*,
                                u8* code, u32 code_size,
                                u8* far_code, u32 far_code_size);
  void        (*begin_block)   (cpu_recompiler_t*);
  const void* (*end_compile)   (cpu_recompiler_t*, u32* code_size, u32* far_code_size);
  void        (*end_block)     (cpu_recompiler_t*, const u32* newpc, bool do_event_test);
  void        (*end_block_with_exception)(cpu_recompiler_t*, cpu_exception_t excode);

  /* Codegen primitives */
  const void* (*get_current_code_pointer)(cpu_recompiler_t*);
  void        (*generate_block_protect_check)(cpu_recompiler_t*,
                                              const u8* ram, const u8* shadow, u32 size);
  void        (*generate_icache_check_and_update)(cpu_recompiler_t*);
  void        (*generate_call)(cpu_recompiler_t*, const void* func,
                               s32 a1reg, s32 a2reg, s32 a3reg);
  void        (*generate_pgxp_call_with_mips_regs)(cpu_recompiler_t*,
                                                   const void* func,
                                                    u32 arg1val,
                                                   cpu_reg_t arg2, 
                                                   cpu_reg_t arg3);

  /* Host-reg primitives consumed by the arch-agnostic allocator. */
  void        (*load_host_reg_with_constant) (cpu_recompiler_t*, u32 host_reg, u32 val);
  void        (*load_host_reg_from_cpu_pointer)(cpu_recompiler_t*, u32 host_reg, const void* ptr);
  void        (*store_host_reg_to_cpu_pointer)(cpu_recompiler_t*, u32 host_reg, const void* ptr);
  void        (*store_constant_to_cpu_pointer)(cpu_recompiler_t*, u32 val, const void* ptr);
  void        (*copy_host_reg)               (cpu_recompiler_t*, u32 dst, u32 src);
  void        (*flush)                       (cpu_recompiler_t*, u32 flags);

  void (*compile_fallback)(cpu_recompiler_t*);

  void (*compile_jr)   (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_jalr) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_bxx)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t, cpu_recomp_branch_cond_t);

  void (*compile_sll)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_srl)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_sra)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_sllv) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_srlv) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_srav) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

  void (*compile_mult) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_multu)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_div)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_divu) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

  void (*compile_add)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_addu) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_sub)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_subu) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_and)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_or)   (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_xor)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_nor)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_slt)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_sltu) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

  void (*compile_addi) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_addiu)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_slti) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_sltiu)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_andi) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_ori)  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_xori) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

  /* Loadstore hooks: extra (size, sign, use_fastmem, known_addr_or_NULL) args. */
  void (*compile_lxx) (cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);
  void (*compile_lwx) (cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);
  void (*compile_lwc2)(cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);
  void (*compile_sxx) (cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);
  void (*compile_swx) (cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);
  void (*compile_swc2)(cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                       memory_access_size_t size, bool sign, bool use_fastmem,
                       const u32* known_addr);

  void (*compile_mtc0)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_rfe) (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_mfc2)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_mtc2)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
  void (*compile_cop2)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);


  const char* (*get_host_reg_name)(u32 reg);
};

/* Resets per-block state at the start of CompileBlock(). */
void  cpu_recompiler_reset_state(cpu_recompiler_t* r,
                                 cpu_code_cache_block_t* block,
                                 u8* code_buffer, u32 code_buffer_space,
                                 u8* far_code_buffer, u32 far_code_space);

/* Constant-register tracking. */
bool  cpu_recompiler_has_constant_reg     (const cpu_recompiler_t*, cpu_reg_t);
bool  cpu_recompiler_has_dirty_constant_reg(const cpu_recompiler_t*, cpu_reg_t);
bool  cpu_recompiler_has_constant_reg_value(const cpu_recompiler_t*, cpu_reg_t, u32);
u32   cpu_recompiler_get_constant_reg_u32 (const cpu_recompiler_t*, cpu_reg_t);
s32   cpu_recompiler_get_constant_reg_s32 (const cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_set_constant_reg     (cpu_recompiler_t*, cpu_reg_t, u32);
void  cpu_recompiler_clear_constant_reg   (cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_flush_constant_reg   (cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_flush_constant_regs  (cpu_recompiler_t*, bool invalidate);

/* Host-register allocator. */
bool  cpu_recompiler_is_host_reg_allocated(const cpu_recompiler_t*, u32 host_reg);
u32   cpu_recompiler_get_free_host_reg    (cpu_recompiler_t*, u32 flags);
u32   cpu_recompiler_allocate_host_reg    (cpu_recompiler_t*, u32 flags,
                                           cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg);
cpu_recomp_opt_u32_t
      cpu_recompiler_check_host_reg       (cpu_recompiler_t*, u32 flags,
                                           cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg);
u32   cpu_recompiler_allocate_temp_host_reg(cpu_recompiler_t*, u32 flags);
void  cpu_recompiler_swap_host_reg_alloc  (cpu_recompiler_t*, u32 lhs, u32 rhs);
void  cpu_recompiler_flush_host_reg       (cpu_recompiler_t*, u32 host_reg);
void  cpu_recompiler_free_host_reg        (cpu_recompiler_t*, u32 host_reg);
void  cpu_recompiler_clear_host_reg       (cpu_recompiler_t*, u32 host_reg);
void  cpu_recompiler_mark_regs_needed     (cpu_recompiler_t*,
                                           cpu_recomp_host_reg_alloc_type_t type, cpu_reg_t reg);
void  cpu_recompiler_rename_host_reg      (cpu_recompiler_t*, u32 host_reg, u32 new_flags,
                                           cpu_recomp_host_reg_alloc_type_t new_type, cpu_reg_t new_reg);
void  cpu_recompiler_clear_host_reg_needed(cpu_recompiler_t*, u32 host_reg);
void  cpu_recompiler_clear_host_regs_needed(cpu_recompiler_t*);
void  cpu_recompiler_delete_mips_reg      (cpu_recompiler_t*, cpu_reg_t reg, bool flush);
bool  cpu_recompiler_try_rename_mips_reg  (cpu_recompiler_t*, cpu_reg_t to, cpu_reg_t from,
                                           u32 fromhost, cpu_reg_t other);
void  cpu_recompiler_update_host_reg_counters(cpu_recompiler_t*);

/* Composite flush + per-reg writeback (calls into vtable). */
void  cpu_recompiler_flush                (cpu_recompiler_t*, u32 flags);

/* Load-delay engine. */
bool  cpu_recompiler_has_load_delay       (const cpu_recompiler_t*);
void  cpu_recompiler_cancel_load_delays_to_reg(cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_update_load_delay    (cpu_recompiler_t*);
void  cpu_recompiler_finish_load_delay    (cpu_recompiler_t*);
void  cpu_recompiler_finish_load_delay_to_reg(cpu_recompiler_t*, cpu_reg_t);
u32   cpu_recompiler_get_flags_for_new_load_delayed_reg(const cpu_recompiler_t*);
u32   cpu_recompiler_get_flags_for_lwx_value_reg(const cpu_recompiler_t*);

/* Host-state backup (delay-slot rollback / overflow-in-delay-slot). */
void  cpu_recompiler_backup_host_state    (cpu_recompiler_t*);
void  cpu_recompiler_restore_host_state   (cpu_recompiler_t*);

/* Speculative constants. */
void  cpu_recompiler_init_speculative_regs(cpu_recompiler_t*);
void  cpu_recompiler_invalidate_speculative_values(cpu_recompiler_t*);
cpu_recomp_spec_value_t cpu_recompiler_spec_read_reg (cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_spec_write_reg       (cpu_recompiler_t*, cpu_reg_t, cpu_recomp_spec_value_t);
void  cpu_recompiler_spec_invalidate_reg  (cpu_recompiler_t*, cpu_reg_t);
void  cpu_recompiler_spec_copy_reg        (cpu_recompiler_t*, cpu_reg_t dst, cpu_reg_t src);
cpu_recomp_spec_value_t cpu_recompiler_spec_read_mem (cpu_recompiler_t*, u32 addr);
void  cpu_recompiler_spec_write_mem       (cpu_recompiler_t*, virtual_memory_address_t addr,
                                           cpu_recomp_spec_value_t);
void  cpu_recompiler_spec_invalidate_mem  (cpu_recompiler_t*, virtual_memory_address_t addr);
bool  cpu_recompiler_spec_is_cache_isolated(cpu_recompiler_t*);

const void* cpu_recompiler_compile_block(cpu_recompiler_t*, cpu_code_cache_block_t*,
                                         u32* out_code_size, u32* out_far_code_size);
void  cpu_recompiler_compile_instruction      (cpu_recompiler_t*);
void  cpu_recompiler_compile_branch_delay_slot(cpu_recompiler_t*, bool dirty_pc);
void  cpu_recompiler_truncate_block           (cpu_recompiler_t*);
void  cpu_recompiler_set_compiler_pc          (cpu_recompiler_t*, u32 newpc);
cpu_reg_t cpu_recompiler_mips_d               (const cpu_recompiler_t*);
u32   cpu_recompiler_get_conditional_branch_target(const cpu_recompiler_t*, cpu_recomp_compile_flags_t);
u32   cpu_recompiler_get_branch_return_address    (const cpu_recompiler_t*, cpu_recomp_compile_flags_t);
bool  cpu_recompiler_try_swap_delay_slot      (cpu_recompiler_t*, cpu_reg_t rs, cpu_reg_t rt, cpu_reg_t rd);
void  cpu_recompiler_flush_for_load_store     (cpu_recompiler_t*, const u32* known_addr,
                                               bool store, bool use_fastmem);
void  cpu_recompiler_compile_move_reg_template(cpu_recompiler_t*, cpu_reg_t dst, cpu_reg_t src,
                                               bool pgxp_move);
const tick_count_t* cpu_recompiler_get_fetch_memory_access_time_ptr(const cpu_recompiler_t*);

/* compile_template: invokes the matching backend hook (or const_func when all
 * source regs are constant).  pgxp_cpu_func may be NULL. */
typedef void (*cpu_recomp_compile_const_fn_t)(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
typedef void (*cpu_recomp_compile_fn_t)      (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

void cpu_recompiler_compile_template(cpu_recompiler_t*,
                                      cpu_recomp_compile_const_fn_t const_func,
                                     cpu_recomp_compile_fn_t func, 
                                     const void* pgxp_cpu_func,
                                     u32 tflags);

typedef void (*cpu_recomp_compile_loadstore_fn_t)(cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                                                  memory_access_size_t, bool sign, bool use_fastmem,
                                                  const u32* known_addr);
void cpu_recompiler_compile_loadstore_template(cpu_recompiler_t*,
                                                cpu_recomp_compile_loadstore_fn_t func,
                                               memory_access_size_t size, bool store, bool sign, 
                                               u32 tflags);

void cpu_recompiler_add_gte_ticks(cpu_recompiler_t*, tick_count_t ticks);
void cpu_recompiler_stall_until_gte_complete(cpu_recompiler_t*);

/* Multi-cycle muldiv stall (mirrors GTE's stall pattern).  Called from
 * the dispatch site at MFHI/MFLO/MTHI/MTLO, before spec_copy_reg, so the
 * stall bumps r->cycles (or pending_ticks via state flush) up to
 * muldiv_done_cycle (matches interp cpu_stall_until_muldiv_complete()). */
void cpu_recompiler_stall_until_muldiv_complete(cpu_recompiler_t*);

/* Producer: record a statically-known cycle count for the muldiv (DIV/DIVU
 * = 35 cycles, MULT/MULTU with compile-time-constant rs).  Marks
 * dirty_muldiv_done_cycle false so in-block consumers stay in r->cycles
 * space (no state mutation). */
void cpu_recompiler_set_muldiv_done_cycle_static(cpu_recompiler_t*, s32 cycles);

/* GTE register pointer table (used by compile_mfc2/mtc2). */
typedef struct {
  u32* ptr;
  cpu_recomp_gte_access_action_t action;
} cpu_recomp_gte_reg_lookup_t;
cpu_recomp_gte_reg_lookup_t cpu_recompiler_get_gte_register_pointer(u32 index, bool writing);

/* Per-MIPS-op _const helpers + non-vtable entry points (Compile_j etc.). */
void cpu_recompiler_compile_j   (cpu_recompiler_t*);
void cpu_recompiler_compile_jal (cpu_recompiler_t*);
void cpu_recompiler_compile_jr_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_jalr_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_syscall  (cpu_recompiler_t*);
void cpu_recompiler_compile_break    (cpu_recompiler_t*);

void cpu_recompiler_compile_b_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_b        (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_blez     (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_blez_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_bgtz     (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_bgtz_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_beq      (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_beq_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_bne      (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_bne_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_bxx_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t,
                                      cpu_recomp_branch_cond_t);

void cpu_recompiler_compile_sll_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_srl_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_sra_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_sllv_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_srlv_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_srav_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_mult_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_multu_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_div_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_divu_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_add_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_addu_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_sub_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_subu_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_and_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_or_const   (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_xor_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_nor_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_slt_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_sltu_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_addi_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_addiu_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_slti_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_sltiu_const(cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_andi_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_ori_const  (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_xori_const (cpu_recompiler_t*, cpu_recomp_compile_flags_t);
void cpu_recompiler_compile_lui        (cpu_recompiler_t*);
void cpu_recompiler_compile_mfc0       (cpu_recompiler_t*, cpu_recomp_compile_flags_t);

/* Spec-exec helpers (per-op constant-prop simulation). */
void cpu_recompiler_spec_exec_b   (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_jal (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_jalr(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sll (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_srl (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sra (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sllv(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_srlv(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_srav(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_mult(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_multu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_div (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_divu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_add (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_addu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sub (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_subu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_and (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_or  (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_xor (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_nor (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_slt (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sltu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_addi(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_addiu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_slti(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_sltiu(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_andi(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_ori (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_xori(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_lui (cpu_recompiler_t*);
void cpu_recompiler_spec_exec_lxx (cpu_recompiler_t*, memory_access_size_t size, bool sign);
void cpu_recompiler_spec_exec_lwx (cpu_recompiler_t*, bool lwr);
void cpu_recompiler_spec_exec_sxx (cpu_recompiler_t*, memory_access_size_t size);
void cpu_recompiler_spec_exec_swx (cpu_recompiler_t*, bool swr);
void cpu_recompiler_spec_exec_swc2(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_mfc0(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_mtc0(cpu_recompiler_t*);
void cpu_recompiler_spec_exec_rfe (cpu_recompiler_t*);
cpu_recomp_spec_value_t cpu_recompiler_spec_exec_loadstore_addr(cpu_recompiler_t*);

/* COP0 register helpers (read-mask + pointer). */
u32*  cpu_recompiler_get_cop0_reg_ptr        (cpu_cop0_reg_t);
u32   cpu_recompiler_get_cop0_reg_write_mask (cpu_cop0_reg_t);

/* MIPS divide helpers (called from compiled code via generate_call). */
void  cpu_recompiler_mips_signed_divide  (s32 num, s32 denom, u32* lo, u32* hi);
void  cpu_recompiler_mips_unsigned_divide(u32 num, u32 denom, u32* lo, u32* hi);

extern cpu_recompiler_t* g_cpu_compiler;

#ifdef __cplusplus
}
#endif

#endif /* CUPID_CORE_CPU_RECOMPILER_H */
