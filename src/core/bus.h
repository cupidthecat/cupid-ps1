/*
 * PS1 main bus: 2 MB system RAM (with 8 MB dev-kit option), 1 KB CPU
 * scratchpad, 512 KB BIOS ROM, and the 4 KB MMIO window at 0x1F801000.
 * Address decode follows the MIPS R3000A KUSEG/KSEG0/KSEG1 mirror layout -
 * the bus does not see KSEG bits because the CPU strips them via the cache
 * mask, but the LUT tables are 4 GB wide and indexed by raw virtual address
 * so we duplicate handlers across all three segments.
 *
 * C++ namespace Bus → bus_* prefix.
 */

#ifndef CUPID_CORE_BUS_H
#define CUPID_CORE_BUS_H

#include "types.h"

#include "common/types.h"

typedef struct Error Error;
typedef struct state_wrapper state_wrapper_t;

enum {
  BUS_RAM_BASE                  = 0x00000000u,
  BUS_RAM_2MB_SIZE              = 0x200000u,
  BUS_RAM_2MB_MASK              = BUS_RAM_2MB_SIZE - 1u,
  BUS_RAM_8MB_SIZE              = 0x800000u,
  BUS_RAM_8MB_MASK              = BUS_RAM_8MB_SIZE - 1u,
  BUS_RAM_MIRROR_END            = 0x800000u, /* end of the four 2MB mirrors */
  BUS_RAM_MIRROR_SIZE           = 0x800000u,

  BUS_EXP1_BASE                 = 0x1F000000u,
  BUS_EXP1_SIZE                 = 0x800000u,
  BUS_EXP1_MASK                 = BUS_EXP1_SIZE - 1u,

  BUS_HW_BASE                   = 0x1F801000u,
  BUS_HW_SIZE                   = 0x1000u,

  BUS_MEMCTRL_BASE              = 0x1F801000u,
  BUS_MEMCTRL_SIZE              = 0x40u,
  BUS_MEMCTRL_MASK              = BUS_MEMCTRL_SIZE - 1u,
  BUS_PAD_BASE                  = 0x1F801040u,
  BUS_PAD_SIZE                  = 0x10u,
  BUS_PAD_MASK                  = BUS_PAD_SIZE - 1u,
  BUS_SIO_BASE                  = 0x1F801050u,
  BUS_SIO_SIZE                  = 0x10u,
  BUS_SIO_MASK                  = BUS_SIO_SIZE - 1u,
  BUS_MEMCTRL2_BASE             = 0x1F801060u,
  BUS_MEMCTRL2_SIZE             = 0x10u,
  BUS_MEMCTRL2_MASK             = BUS_MEMCTRL2_SIZE - 1u,
  BUS_INTC_BASE                 = 0x1F801070u,
  BUS_INTC_SIZE                 = 0x10u,
  BUS_INTERRUPT_CONTROLLER_MASK = BUS_INTC_SIZE - 1u,
  BUS_DMA_BASE                  = 0x1F801080u,
  BUS_DMA_SIZE                  = 0x80u,
  BUS_DMA_MASK                  = BUS_DMA_SIZE - 1u,
  BUS_TIMERS_BASE               = 0x1F801100u,
  BUS_TIMERS_SIZE               = 0x40u,
  BUS_TIMERS_MASK               = BUS_TIMERS_SIZE - 1u,
  BUS_CDROM_BASE                = 0x1F801800u,
  BUS_CDROM_SIZE                = 0x10u,
  BUS_CDROM_MASK                = BUS_CDROM_SIZE - 1u,
  BUS_GPU_BASE                  = 0x1F801810u,
  BUS_GPU_SIZE                  = 0x10u,
  BUS_GPU_MASK                  = BUS_GPU_SIZE - 1u,
  BUS_MDEC_BASE                 = 0x1F801820u,
  BUS_MDEC_SIZE                 = 0x10u,
  BUS_MDEC_MASK                 = BUS_MDEC_SIZE - 1u,
  BUS_SPU_BASE                  = 0x1F801C00u,
  BUS_SPU_SIZE                  = 0x400u,
  BUS_SPU_MASK                  = BUS_SPU_SIZE - 1u,

  BUS_SIO2_BASE                 = 0x1F808000u,
  BUS_SIO2_SIZE                 = 0x1000u,
  BUS_SIO2_MASK                 = BUS_SIO2_SIZE - 1u,
  BUS_EXP2_BASE                 = 0x1F802000u,
  BUS_EXP2_SIZE                 = 0x2000u,
  BUS_EXP2_MASK                 = BUS_EXP2_SIZE - 1u,
  BUS_EXP3_BASE                 = 0x1FA00000u,
  BUS_EXP3_SIZE                 = 0x200000u,
  BUS_EXP3_MASK                 = BUS_EXP3_SIZE - 1u,

  BUS_BIOS_BASE                 = 0x1FC00000u,
  BUS_BIOS_SIZE                 = 0x80000u,
  BUS_BIOS_MIRROR_SIZE          = 0x400000u,
  BUS_BIOS_MASK                 = 0x7FFFFu,
};

enum {
  BUS_MEMCTRL_REG_COUNT = 9
};

/* DRAM access cost in CPU cycles per word (CPU-side, not DMA). */
enum {
  BUS_RAM_READ_TICKS = 6
};

enum {
  BUS_RAM_2MB_CODE_PAGE_COUNT = (BUS_RAM_2MB_SIZE + (MIN_HOST_PAGE_SIZE - 1u)) / MIN_HOST_PAGE_SIZE,
  BUS_RAM_8MB_CODE_PAGE_COUNT = (BUS_RAM_8MB_SIZE + (MIN_HOST_PAGE_SIZE - 1u)) / MIN_HOST_PAGE_SIZE,

  /* 4 KB granularity LUT covering the entire 32-bit virtual address space.
   * A handler pointer is stored per (size, type) tuple → 6 slots / page. */
  BUS_MEMORY_LUT_PAGE_SIZE  = 4096,
  BUS_MEMORY_LUT_PAGE_SHIFT = 12,
  BUS_MEMORY_LUT_PAGE_MASK  = BUS_MEMORY_LUT_PAGE_SIZE - 1,
  BUS_MEMORY_LUT_SIZE       = 0x100000,                  /* 4GB / 4KB */
  BUS_MEMORY_LUT_SLOTS      = BUS_MEMORY_LUT_SIZE * 3 * 2, /* size * (read|write) */

  BUS_FASTMEM_LUT_PAGE_SIZE  = 4096,
  BUS_FASTMEM_LUT_PAGE_MASK  = BUS_FASTMEM_LUT_PAGE_SIZE - 1,
  BUS_FASTMEM_LUT_PAGE_SHIFT = 12,
  BUS_FASTMEM_LUT_SIZE       = 0x100000,
  BUS_FASTMEM_LUT_SLOTS      = BUS_FASTMEM_LUT_SIZE * 2, /* normal + isolated */
};

 /* MMAP-mode fastmem arena: 4 GiB virtual reservation; PSX RAM mmap'd at the
 * 12 KUSEG/KSEG0/KSEG1 mirror offsets.  Out-of-RAM accesses fault →
 * cpu_code_cache_handle_fastmem_exception. */
#define BUS_FASTMEM_ARENA_SIZE 0x100000000ULL

/* Handler signatures: read returns up to a u32 and only consumes the address;
 * write consumes the address plus the (possibly truncated) value to store. */
typedef u32  (*bus_memory_read_handler_t) (virtual_memory_address_t address);
typedef void (*bus_memory_write_handler_t)(virtual_memory_address_t address, u32 value);

/* Allocates the shared-memory backing for RAM/BIOS/LUTs and (if enabled)
 * the fastmem arena.  When export_shared_memory is true a named shm region
 * is created so external debuggers can attach. */
bool bus_allocate_memory(bool export_shared_memory, Error* error);
void bus_release_memory(void);

bool bus_reallocate_memory_map(bool export_shared_memory, Error* error);
void bus_cleanup_memory_map(void);

void bus_initialize(void);
void bus_shutdown(void);
void bus_reset(void);
bool bus_do_state(state_wrapper_t* sw);

/* Returns the appropriate set of handler tables for the current CPU cache
 * state.  When isolate_cache is true reads/writes hit the I-cache shadow
 * tables instead of real memory.  swap_caches is currently informational. */
void** bus_get_memory_handlers(bool isolate_cache, bool swap_caches);

ALWAYS_INLINE bus_memory_read_handler_t* bus_offset_read_handler_array(void** handlers,
                                                                       memory_access_size_t size)
{
  return (bus_memory_read_handler_t*)(handlers + (((size_t)size * 2u + 0u) * BUS_MEMORY_LUT_SIZE));
}
ALWAYS_INLINE bus_memory_write_handler_t* bus_offset_write_handler_array(void** handlers,
                                                                         memory_access_size_t size)
{
  return (bus_memory_write_handler_t*)(handlers + (((size_t)size * 2u + 1u) * BUS_MEMORY_LUT_SIZE));
}

void* bus_get_fastmem_base(bool isc);
void  bus_remap_fastmem_views(void);
bool  bus_can_use_fastmem_for_address(virtual_memory_address_t address);

/* RAM-write tap.  When non-NULL, every successful s_ram[..] write fires
 * the callback with (paddr, size, value, writer_pc).  Default NULL --
 * production cost is one cmp/jne per RAM write.  Used by cpu_diff_block
 * to compare recomp vs interp write logs at block boundaries.  writer_pc
 * is g_cpu_state.current_instruction_pc at the time of the write (the JIT
 * sets this immediate-per-instruction in the prologue, see x64_backend.c
 * line ~627). */
typedef void (*bus_ram_write_tap_fn)(u32 paddr, u8 size, u32 value, u32 writer_pc);
extern bus_ram_write_tap_fn g_bus_ram_write_tap;

u8*  bus_get_ram_pointer(void);             /* page-protected (write triggers SMC) */
u8*  bus_get_unprotected_ram_pointer(void); /* alias view, debugger-safe writes */
u32  bus_get_ram_size(void);
u32  bus_get_ram_mapped_size(void);
u32  bus_get_ram_mask(void);
u8*  bus_get_bios_pointer(void);

/* Per-region access timing tables, indexed by memory_access_size_t. */
const tick_count_t* bus_get_exp1_access_time(void);
const tick_count_t* bus_get_exp2_access_time(void);
const tick_count_t* bus_get_bios_access_time(void);
const tick_count_t* bus_get_cdrom_access_time(void);
const tick_count_t* bus_get_spu_access_time(void);

/* Code page bitmap accessors (covers whichever RAM size is active). */
bool bus_is_ram_code_page(u32 index);
void bus_set_ram_code_page(u32 index);   /* mark page as containing JIT code */
void bus_clear_ram_code_page(u32 index); /* unmark */
void bus_clear_ram_code_page_flags(void);

/* Returns true if the address specified is writable RAM (within the 4
 * mirrors).  Equivalent to (address < RAM_MIRROR_END). */
ALWAYS_INLINE bool bus_is_ram_address(physical_memory_address_t address)
{
  return address < BUS_RAM_MIRROR_END;
}

/* RAM offset → host-page index, masked to active RAM size. */
ALWAYS_INLINE u32 bus_get_ram_code_page_index(physical_memory_address_t address)
{
  return (address & bus_get_ram_mask()) >> HOST_PAGE_SHIFT;
}

/* Estimated DMA tick count for `word_count` words.  PS1 DMA uses DRAM
 * Hyper Page mode at ~1 cycle/word, plus ~1 row-load cycle per 16 words.
 * Substantially faster than CPU DRAM access (1 + 6 waitstates = 7 / word). */
ALWAYS_INLINE tick_count_t bus_get_dma_ram_tick_count(u32 word_count)
{
  return (tick_count_t)(word_count + ((word_count + 15u) / 16u));
}

/* Returns true if the specified physical address overlaps a JIT code page. */
bool bus_is_code_page_address(physical_memory_address_t address);

/* Returns true if the [start, start+size) physical range overlaps any
 * known JIT code page in RAM. */
bool bus_has_code_pages_in_range(physical_memory_address_t start_address, u32 size);

/* Returns a pointer to the cycle count for a non-RAM access (BIOS, EXP1).
 * NULL when the address is in RAM or has no defined timing. */
const tick_count_t* bus_get_memory_access_time_ptr(physical_memory_address_t address,
                                                   memory_access_size_t size);

typedef enum {
  BUS_MEMORY_REGION_RAM,
  BUS_MEMORY_REGION_RAM_MIRROR1,
  BUS_MEMORY_REGION_RAM_MIRROR2,
  BUS_MEMORY_REGION_RAM_MIRROR3,
  BUS_MEMORY_REGION_EXP1,
  BUS_MEMORY_REGION_SCRATCHPAD,
  BUS_MEMORY_REGION_BIOS,
  BUS_MEMORY_REGION_COUNT,
} bus_memory_region_t;

/* optional<MemoryRegion> in C++; we use a sentinel _COUNT for "none". */
bus_memory_region_t       bus_get_memory_region_for_address(physical_memory_address_t address);
physical_memory_address_t bus_get_memory_region_start(bus_memory_region_t region);
physical_memory_address_t bus_get_memory_region_end(bus_memory_region_t region);
bool                      bus_is_memory_region_writable(bus_memory_region_t region);
u8*                       bus_get_memory_region_pointer(bus_memory_region_t region);

/* Linear scan from start_address; returns address of first match, or
 * UINT32_MAX (>= 4GB equivalent) when not found.  Caller checks for the
 * sentinel via bus_search_memory_found(). */
physical_memory_address_t bus_search_memory(physical_memory_address_t start_address,
                                            const u8* pattern, const u8* mask,
                                            u32 pattern_length);
ALWAYS_INLINE bool        bus_search_memory_found(physical_memory_address_t r) { return r != 0xFFFFFFFFu; }

/* TTY ($1F802023, $1F802080, $1FA00000) capture: characters are buffered
 * into one log line per CR/LF. */
void bus_add_tty_character(char ch);
void bus_add_tty_string(const char* str, size_t len);

/* Loads a PS-EXE blob into memory at its requested base.  When set_pc is
 * true, also rewrites $pc/$gp/$sp to the header's entry data. */
bool bus_inject_executable(const u8* buffer, size_t buffer_size, bool set_pc, Error* error);

#endif /* CUPID_CORE_BUS_H */
