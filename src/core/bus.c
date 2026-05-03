/*
 * Bus implementation: shared-memory backed RAM/BIOS/LUT, MMIO dispatch via
 * function-pointer tables indexed by 4 KB virtual page, code-page write
 * protection for SMC detection, and per-region memory timing.
 *
 * Cross-module callees (DMA, GPU, SPU, CDROM, timers, INTC, MDEC, PAD,
 * SIO, CPU code cache, the CPU register state, BIOS) are declared `extern`
 * here so this file links cleanly even before those modules are ported.
 * Each takes the C name the corresponding agent will emit.
 */

#include "bus.h"
#include "bios.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "settings.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/types.h"

#include "util/state_wrapper.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

LOG_CHANNEL(Bus);

extern u8*  cpu_get_scratchpad_pointer(void);             /* 1 KB scratchpad */
extern u32  cpu_get_scratchpad_size(void);
extern u32  cpu_get_scratchpad_address(void);             /* 0x1F800000 */
extern u32  cpu_get_pc(void);
extern void cpu_set_bus_error(bool v);
extern void cpu_update_memory_pointers(void);
extern u32  cpu_virtual_to_physical_address(virtual_memory_address_t addr);
extern u32  cpu_get_icache_line(virtual_memory_address_t addr);
extern u32  cpu_get_icache_line_offset(virtual_memory_address_t addr);
extern u32  cpu_get_icache_tag_for_address(virtual_memory_address_t addr);
extern u32* cpu_get_icache_data(void);
extern u32* cpu_get_icache_tags(void);
extern u32  cpu_get_cache_control_bits(void);
extern void cpu_set_cache_control_bits(u32 v);
extern void cpu_set_pc(u32 new_pc);

extern u32  pad_read_register(u32 offset);
extern void pad_write_register(u32 offset, u32 value);
extern u32  sio_read_register(u32 offset);
extern void sio_write_register(u32 offset, u32 value);
extern u32  interrupt_controller_read_register(u32 offset);
extern void interrupt_controller_write_register(u32 offset, u32 value);
extern u32  dma_read_register(u32 offset);
extern void dma_write_register(u32 offset, u32 value);
extern u32  timers_read_register(u32 offset);
extern void timers_write_register(u32 offset, u32 value);
extern u8   cdrom_read_register(u32 offset);
extern void cdrom_write_register(u32 offset, u8 value);
extern u32  gpu_read_register(u32 offset);
extern void gpu_write_register(u32 offset, u32 value);
extern u32  mdec_read_register(u32 offset);
extern void mdec_write_register(u32 offset, u32 value);
extern u16  spu_read_register(u32 offset);
extern void spu_write_register(u32 offset, u16 value);

extern u8   pio_device_read(u32 offset);
extern void pio_device_write(u32 offset, u8 value);

extern bool system_is_valid(void);
extern bool system_is_using_known_ps1_bios(void);
extern void system_handle_kernel_initialized(void);
extern bool settings_get_cpu_enable_8mb_ram(void);

extern void cpu_code_cache_invalidate_all_ram_blocks(void);

enum {
  MEMMAP_RAM_OFFSET   = 0,
  MEMMAP_RAM_SIZE     = BUS_RAM_8MB_SIZE,
  MEMMAP_BIOS_OFFSET  = MEMMAP_RAM_OFFSET + MEMMAP_RAM_SIZE,
  MEMMAP_BIOS_SIZE    = BUS_BIOS_SIZE,
  MEMMAP_LUT_OFFSET   = MEMMAP_BIOS_OFFSET + MEMMAP_BIOS_SIZE,
};
/* LUT stores BUS_MEMORY_LUT_SLOTS pointers normal + same again for isolated. */
#define MEMMAP_LUT_SIZE   ((sizeof(void*) * (size_t)BUS_MEMORY_LUT_SLOTS) * 2u)
#define MEMMAP_TOTAL_SIZE ((size_t)MEMMAP_LUT_OFFSET + MEMMAP_LUT_SIZE)

#pragma pack(push, 1)
typedef union {
  u32 bits;
  struct {
    u32 unused_low      : 4;
    u32 access_time     : 4; /* cycles */
    u32 use_com0_time   : 1;
    u32 use_com1_time   : 1;
    u32 use_com2_time   : 1;
    u32 use_com3_time   : 1;
    u32 data_bus_16bit  : 1;
    u32 unused_mid      : 3;
    u32 memory_window_size : 5;
    u32 unused_high     : 11;
  } b;
} memdelay_t;

typedef union {
  u32 bits;
  struct {
    u32 com0  : 4;
    u32 com1  : 4;
    u32 com2  : 4;
    u32 com3  : 4;
    u32 comunk: 2;
    u32 unused: 14;
  } b;
} comdelay_t;

typedef union {
  u32 bits;
  struct {
    u32 unused_low    : 9;
    u32 memory_window : 3;
    u32 unused_high   : 20;
  } b;
} ram_size_reg_t;
#pragma pack(pop)

#define MEMDELAY_WRITE_MASK  0xAF1FFFFFu /* 0b10101111'00011111'11111111'11111111 */
#define COMDELAY_WRITE_MASK  0x0003FFFFu /* 0b00000000'00000011'11111111'11111111 */

typedef union {
  u32 regs[BUS_MEMCTRL_REG_COUNT];
  struct {
    u32        exp1_base;
    u32        exp2_base;
    memdelay_t exp1_delay_size;
    memdelay_t exp3_delay_size;
    memdelay_t bios_delay_size;
    memdelay_t spu_delay_size;
    memdelay_t cdrom_delay_size;
    memdelay_t exp2_delay_size;
    comdelay_t common_delay;
  } r;
} memctrl_t;

static void* s_shmem_handle = NULL;
static char  s_shmem_name[128] = {0};
static bool  s_shmem_named = false;

/* Code page bitmap covers up to 8 MB / 4 KB = 2048 bits = 64 u32 words. */
enum { CODE_BITS_WORDS = (BUS_RAM_8MB_CODE_PAGE_COUNT + 31u) / 32u };
static u32 s_ram_code_bits[CODE_BITS_WORDS];

static u8*  s_ram             = NULL; /* page-protected view */
static u8*  s_unprotected_ram = NULL; /* aliasing view, debugger writes */
static u32  s_ram_size        = 0;
static u32  s_ram_mapped_size = 0;
static u32  s_ram_mask        = 0;
static u8*  s_bios            = NULL;

static void** s_memory_handlers     = NULL;
static void** s_memory_handlers_isc = NULL;

static tick_count_t s_exp1_access_time [3] = {0};
static tick_count_t s_exp2_access_time [3] = {0};
static tick_count_t s_bios_access_time [3] = {0};
static tick_count_t s_cdrom_access_time[3] = {0};
static tick_count_t s_spu_access_time  [3] = {0};

static memctrl_t      s_MEMCTRL;
static ram_size_reg_t s_RAM_SIZE;

enum { TTY_LINE_BUFFER_CAPACITY = 4096 };
static char   s_tty_line_buffer[TTY_LINE_BUFFER_CAPACITY];
static size_t s_tty_line_buffer_len = 0;

static u8** s_fastmem_lut = NULL;

/* M3: SIGSEGV-fastmem arena (4 GiB virtual reservation; PSX RAM mmap'd
 * MAP_FIXED|MAP_SHARED at 12 KUSEG/KSEG0/KSEG1 mirror offsets).  When
 * `g_settings.cpu_fastmem_mode == CPU_FASTMEM_MODE_MMAP`, the recompiler's
 * emitted load/store sites do `mov reg, [RBX + addr]` directly into this
 * arena.  Out-of-RAM accesses (KSEG2, scratchpad, MMIO) fault with
 * SIGSEGV → page_fault_handler → cpu_code_cache_handle_fastmem_exception.
 * BUS_FASTMEM_ARENA_SIZE lives in bus.h so the code cache can reference it. */
static u8*  s_fastmem_arena_base   = NULL;
static bool s_fastmem_arena_active = false;  /* views currently mapped */

static bool s_kernel_initialize_hook_run = false;

static bool allocate_memory_map(bool export_shared_memory, Error* error);
static void release_memory_map(void);
static void set_ram_size(bool enable_8mb_ram);

static void calculate_memory_timing(memdelay_t mem_delay, comdelay_t common_delay,
                                    tick_count_t* out_byte, tick_count_t* out_half,
                                    tick_count_t* out_word);
static void recalculate_memory_timings(void);

static void map_fastmem_views_lut(void);
static void map_fastmem_views_mmap(void);
static void unmap_fastmem_views_mmap(void);
static void release_fastmem_arena(void);
static u8*  get_lut_fastmem_pointer(u32 address, u8* ram_ptr);

static void set_ram_page_writable(u32 page_index, bool writable);

static void set_handlers(void);
static void update_mapped_ram_size(void);
static void clear_handlers(void** handlers);
static void set_handler_for_region(void** handlers, virtual_memory_address_t address, u32 size,
                                    bus_memory_read_handler_t  read_byte,
                                   bus_memory_read_handler_t  read_half,
                                   bus_memory_read_handler_t  read_word,
                                   bus_memory_write_handler_t write_byte,
                                   bus_memory_write_handler_t write_half, 
                                   bus_memory_write_handler_t write_word);

static ALWAYS_INLINE bool code_bit_get(u32 idx)  { return (s_ram_code_bits[idx >> 5] >> (idx & 31u)) & 1u; }
static ALWAYS_INLINE void code_bit_set(u32 idx)  { s_ram_code_bits[idx >> 5] |=  (1u << (idx & 31u)); }
static ALWAYS_INLINE void code_bit_clr(u32 idx)  { s_ram_code_bits[idx >> 5] &= ~(1u << (idx & 31u)); }

#define FIXUP_HALFWORD_OFFSET(size, offset)                                                                            \
  (((size) >= MEMORY_ACCESS_SIZE_HALFWORD) ? (offset) : ((offset) & ~1u))
#define FIXUP_HALFWORD_READ_VALUE(size, offset, value)                                                                 \
  (((size) >= MEMORY_ACCESS_SIZE_HALFWORD) ? (value) : ((value) >> (((offset) & 1u) * 8u)))
#define FIXUP_HALFWORD_WRITE_VALUE(size, offset, value)                                                                \
  (((size) >= MEMORY_ACCESS_SIZE_HALFWORD) ? (value) : ((value) << (((offset) & 1u) * 8u)))

#define FIXUP_WORD_OFFSET(size, offset)                                                                                \
  (((size) == MEMORY_ACCESS_SIZE_WORD) ? (offset) : ((offset) & ~3u))
#define FIXUP_WORD_READ_VALUE(size, offset, value)                                                                     \
  (((size) == MEMORY_ACCESS_SIZE_WORD) ? (value) : ((value) >> (((offset) & 3u) * 8u)))
#define FIXUP_WORD_WRITE_VALUE(size, offset, value)                                                                    \
  (((size) == MEMORY_ACCESS_SIZE_WORD) ? (value) : ((value) << (((offset) & 3u) * 8u)))

#define BUS_CYCLES(n) cpu_add_pending_ticks((u32)(n))

static bool allocate_memory_map(bool export_shared_memory, Error* error)
{
  INFO_LOG("Allocating%s shared memory map.", export_shared_memory ? " EXPORTED" : "");
  if (export_shared_memory)
  {
    memmap_get_file_mapping_name(s_shmem_name, sizeof(s_shmem_name), "cupid-ps1");
    s_shmem_named = true;
    INFO_LOG("Shared memory object name is \"%s\".", s_shmem_name);
  }

  s_shmem_handle =
    memmap_create_shared_memory(s_shmem_named ? s_shmem_name : NULL, MEMMAP_TOTAL_SIZE, error);
  if (!s_shmem_handle)
  {
    Error_add_suffix(error,
                     "\nYou may need to close some programs to free up additional memory, "
                     "or increase the size of /dev/shm.");
    return false;
  }

  s_ram = (u8*)memmap_map_shared_memory(s_shmem_handle, MEMMAP_RAM_OFFSET, NULL, MEMMAP_RAM_SIZE,
                                        MEMMAP_PAGE_PROTECT_READ_WRITE);
  s_unprotected_ram = (u8*)memmap_map_shared_memory(s_shmem_handle, MEMMAP_RAM_OFFSET, NULL,
                                                    MEMMAP_RAM_SIZE, MEMMAP_PAGE_PROTECT_READ_WRITE);
  if (!s_ram || !s_unprotected_ram)
  {
    Error_set_string(error, "Failed to map memory for RAM");
    release_memory_map();
    return false;
  }

  VERBOSE_LOG("RAM is mapped at %p.", (void*)s_ram);

  s_bios = (u8*)memmap_map_shared_memory(s_shmem_handle, MEMMAP_BIOS_OFFSET, NULL, MEMMAP_BIOS_SIZE,
                                         MEMMAP_PAGE_PROTECT_READ_WRITE);
  if (!s_bios)
  {
    Error_set_string(error, "Failed to map memory for BIOS");
    release_memory_map();
    return false;
  }
  VERBOSE_LOG("BIOS is mapped at %p.", (void*)s_bios);

  s_memory_handlers = (void**)memmap_map_shared_memory(s_shmem_handle, MEMMAP_LUT_OFFSET, NULL,
                                                       MEMMAP_LUT_SIZE,
                                                       MEMMAP_PAGE_PROTECT_READ_WRITE);
  if (!s_memory_handlers)
  {
    Error_set_string(error, "Failed to map memory for LUTs");
    release_memory_map();
    return false;
  }
  VERBOSE_LOG("LUTs are mapped at %p.", (void*)s_memory_handlers);

  s_memory_handlers_isc = s_memory_handlers + BUS_MEMORY_LUT_SLOTS;
  s_ram_mapped_size     = BUS_RAM_8MB_SIZE;
  set_handlers();
  return true;
}

static void release_memory_map(void)
{
  s_memory_handlers_isc = NULL;
  if (s_memory_handlers)
  {
    memmap_unmap_shared_memory(s_memory_handlers, MEMMAP_LUT_SIZE);
    s_memory_handlers = NULL;
  }
  if (s_bios)
  {
    memmap_unmap_shared_memory(s_bios, MEMMAP_BIOS_SIZE);
    s_bios = NULL;
  }
  if (s_unprotected_ram)
  {
    memmap_unmap_shared_memory(s_unprotected_ram, MEMMAP_RAM_SIZE);
    s_unprotected_ram = NULL;
  }
  if (s_ram)
  {
    memmap_unmap_shared_memory(s_ram, MEMMAP_RAM_SIZE);
    s_ram = NULL;
  }
  if (s_shmem_handle)
  {
    memmap_destroy_shared_memory(s_shmem_handle);
    s_shmem_handle = NULL;
    if (s_shmem_named)
    {
      memmap_delete_shared_memory(s_shmem_name);
      s_shmem_name[0] = '\0';
      s_shmem_named   = false;
    }
  }
}

bool bus_allocate_memory(bool export_shared_memory, Error* error)
{
  if (!allocate_memory_map(export_shared_memory, error))
    return false;
  return true;
}

void bus_release_memory(void)
{
  free(s_fastmem_lut);
  s_fastmem_lut = NULL;
  release_memory_map();
}

bool bus_reallocate_memory_map(bool export_shared_memory, Error* error)
{
  /* Need to back up RAM+BIOS so a re-export keeps the running game alive. */
  u8* ram_backup  = NULL;
  u8* bios_backup = NULL;

  if (system_is_valid())
  {
    cpu_code_cache_invalidate_all_ram_blocks();
    unmap_fastmem_views_mmap();

    ram_backup  = (u8*)malloc(BUS_RAM_8MB_SIZE);
    bios_backup = (u8*)malloc(BUS_BIOS_SIZE);
    if (!ram_backup || !bios_backup)
    {
      Error_set_string(error, "Failed to allocate backup buffers for re-map");
      free(ram_backup);
      free(bios_backup);
      return false;
    }
    memcpy(ram_backup,  s_unprotected_ram, BUS_RAM_8MB_SIZE);
    memcpy(bios_backup, s_bios,            BUS_BIOS_SIZE);
  }

  release_memory_map();
  if (!allocate_memory_map(export_shared_memory, error))
  {
    free(ram_backup);
    free(bios_backup);
    return false;
  }

  if (system_is_valid())
  {
    update_mapped_ram_size();
    memcpy(s_unprotected_ram, ram_backup,  BUS_RAM_8MB_SIZE);
    memcpy(s_bios,            bios_backup, BUS_BIOS_SIZE);
    bus_remap_fastmem_views();
    free(ram_backup);
    free(bios_backup);
  }
  return true;
}

void bus_cleanup_memory_map(void)
{
  /* Crash-handler entry: best-effort unlink of leftover named shm. */
  if (s_shmem_named)
    memmap_delete_shared_memory(s_shmem_name);
}

void bus_initialize(void)
{
  set_ram_size(settings_get_cpu_enable_8mb_ram());
  bus_remap_fastmem_views();
}

static void set_ram_size(bool enable_8mb_ram)
{
  s_ram_size = enable_8mb_ram ? BUS_RAM_8MB_SIZE : BUS_RAM_2MB_SIZE;
  s_ram_mask = enable_8mb_ram ? BUS_RAM_8MB_MASK : BUS_RAM_2MB_MASK;
}

void bus_shutdown(void)
{
  unmap_fastmem_views_mmap();
  release_fastmem_arena();
  free(s_fastmem_lut);
  s_fastmem_lut = NULL;
  /* Clear CPU::g_state.fastmem_base on bus shutdown.  Leaving the stale
   * pointer in g_cpu_state would let a recompiler dispatch / page-fault
   * handler dereference a freed mapping if the bus is shut down while the
   * JIT is still live.  Cheap; correctness over fragility. */
  g_cpu_state.fastmem_base = NULL;
  s_ram_mask = 0;
  s_ram_size = 0;
}

void bus_reset(void)
{
  if (s_ram)
    memset(s_ram, 0, s_ram_size);

  s_MEMCTRL.r.exp1_base            = 0x1F000000u;
  s_MEMCTRL.r.exp2_base            = 0x1F802000u;
  s_MEMCTRL.r.exp1_delay_size.bits = 0x0013243Fu;
  s_MEMCTRL.r.exp3_delay_size.bits = 0x00003022u;
  s_MEMCTRL.r.bios_delay_size.bits = 0x0013243Fu;
  s_MEMCTRL.r.spu_delay_size.bits  = 0x200931E1u;
  s_MEMCTRL.r.cdrom_delay_size.bits= 0x00020843u;
  s_MEMCTRL.r.exp2_delay_size.bits = 0x00070777u;
  s_MEMCTRL.r.common_delay.bits    = 0x00031125u;

  memset(s_ram_code_bits, 0, sizeof(s_ram_code_bits));
  s_kernel_initialize_hook_run = false;
  recalculate_memory_timings();

  if (s_RAM_SIZE.bits != 0x00000B88u)
  {
    s_RAM_SIZE.bits = 0x00000B88u;
    update_mapped_ram_size();
  }
}

bool bus_do_state(state_wrapper_t* sw)
{
  u32 ram_size = s_ram_size;
  state_wrapper_do_u32(sw, &ram_size);
  if (ram_size != s_ram_size) {
    set_ram_size(ram_size == BUS_RAM_8MB_SIZE);
    bus_remap_fastmem_views();
  }

  state_wrapper_do_array(sw, s_exp1_access_time,  sizeof(tick_count_t), 3);
  state_wrapper_do_array(sw, s_exp2_access_time,  sizeof(tick_count_t), 3);
  state_wrapper_do_array(sw, s_bios_access_time,  sizeof(tick_count_t), 3);
  state_wrapper_do_array(sw, s_cdrom_access_time, sizeof(tick_count_t), 3);
  state_wrapper_do_array(sw, s_spu_access_time,   sizeof(tick_count_t), 3);

  state_wrapper_do_bytes(sw, s_unprotected_ram, s_ram_size);

  state_wrapper_do_array(sw, s_MEMCTRL.regs, sizeof(u32), BUS_MEMCTRL_REG_COUNT);

  const ram_size_reg_t old_ram_size_reg = s_RAM_SIZE;
  state_wrapper_do_u32(sw, &s_RAM_SIZE.bits);
  if (s_RAM_SIZE.b.memory_window != old_ram_size_reg.b.memory_window)
    update_mapped_ram_size();

  /* TTY line buffer: serialize length + bytes (cupid-ps1 stores as fixed-cap
   * char[] + size_t). */
  {
    u32 tty_len = (u32)s_tty_line_buffer_len;
    state_wrapper_do_u32(sw, &tty_len);
    if (tty_len > TTY_LINE_BUFFER_CAPACITY) tty_len = TTY_LINE_BUFFER_CAPACITY;
    state_wrapper_do_bytes(sw, s_tty_line_buffer, tty_len);
    if (state_wrapper_is_reading(sw))
      s_tty_line_buffer_len = (size_t)tty_len;
  }

  state_wrapper_do_bool(sw, &s_kernel_initialize_hook_run);

  return true;
}

static void calculate_memory_timing(memdelay_t mem_delay, comdelay_t common_delay,
                                     tick_count_t* out_byte, tick_count_t* out_half,
                                    tick_count_t* out_word) 
{
  /* From nocash spec: MEMDELAY composes one-shot (`first`) and burst (`seq`)
   * timings out of the shared COMDELAY slots. */
  s32 first = 0, seq = 0, mn = 0;
  if (mem_delay.b.use_com0_time)
  {
    first += (s32)common_delay.b.com0 - 1;
    seq   += (s32)common_delay.b.com0 - 1;
  }
  if (mem_delay.b.use_com2_time)
  {
    first += (s32)common_delay.b.com2;
    seq   += (s32)common_delay.b.com2;
  }
  if (mem_delay.b.use_com3_time)
    mn = (s32)common_delay.b.com3;

  if (first < 6)
    first++;

  first = first + (s32)mem_delay.b.access_time + 2;
  seq   = seq   + (s32)mem_delay.b.access_time + 2;

  if (first < (mn + 6))
    first = mn + 6;
  if (seq < (mn + 2))
    seq = mn + 2;

  const tick_count_t b   = first;
  const tick_count_t h   = mem_delay.b.data_bus_16bit ? first : (first + seq);
  const tick_count_t w   = mem_delay.b.data_bus_16bit ? (first + seq) : (first + seq + seq + seq);

  *out_byte = (b - 1 < 0) ? 0 : (b - 1);
  *out_half = (h - 1 < 0) ? 0 : (h - 1);
  *out_word = (w - 1 < 0) ? 0 : (w - 1);
}

static void recalculate_memory_timings(void)
{
  calculate_memory_timing(s_MEMCTRL.r.bios_delay_size, s_MEMCTRL.r.common_delay,
                          &s_bios_access_time[0], &s_bios_access_time[1], &s_bios_access_time[2]);
  calculate_memory_timing(s_MEMCTRL.r.cdrom_delay_size, s_MEMCTRL.r.common_delay,
                          &s_cdrom_access_time[0], &s_cdrom_access_time[1], &s_cdrom_access_time[2]);
  calculate_memory_timing(s_MEMCTRL.r.spu_delay_size, s_MEMCTRL.r.common_delay,
                          &s_spu_access_time[0], &s_spu_access_time[1], &s_spu_access_time[2]);
  calculate_memory_timing(s_MEMCTRL.r.exp1_delay_size, s_MEMCTRL.r.common_delay,
                          &s_exp1_access_time[0], &s_exp1_access_time[1], &s_exp1_access_time[2]);

  TRACE_LOG("BIOS Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
            s_MEMCTRL.r.bios_delay_size.b.data_bus_16bit ? 16u : 8u,
            s_bios_access_time[0] + 1, s_bios_access_time[1] + 1, s_bios_access_time[2] + 1);
}

/* The 12 KUSEG/KSEG0/KSEG1 RAM-mirror base addresses.  Shared between the
 * LUT path and the MMap arena path so both views agree on mirror layout. */
static const u32 s_fastmem_mirror_bases[12] = {
  0x00000000u, 0x00200000u, 0x00400000u, 0x00600000u,  /* KUSEG */
  0x80000000u, 0x80200000u, 0x80400000u, 0x80600000u,  /* KSEG0 */
  0xA0000000u, 0xA0200000u, 0xA0400000u, 0xA0600000u,  /* KSEG1 */
};

void* bus_get_fastmem_base(bool isc)
{
  if (g_settings.cpu_fastmem_mode == CPU_FASTMEM_MODE_MMAP && s_fastmem_arena_base)
    return isc ? NULL : s_fastmem_arena_base;

  if (s_fastmem_lut)
    return (u8*)s_fastmem_lut + (isc ? (BUS_FASTMEM_LUT_SIZE * sizeof(void*)) : 0u);

  return NULL;
}

static u8* get_lut_fastmem_pointer(u32 address, u8* ram_ptr)
{
  return ram_ptr - address;
}

static void map_fastmem_views_lut(void)
{
  if (!s_fastmem_lut)
  {
    s_fastmem_lut = (u8**)malloc(sizeof(u8*) * BUS_FASTMEM_LUT_SLOTS);
    if (!s_fastmem_lut)
    {
      ERROR_LOG("Failed to allocate fastmem LUT");
      return;
    }
    INFO_LOG("Fastmem base (LUT): %p", (void*)s_fastmem_lut);
  }

  /* Top 4KB of address space stays unmapped (sentinel). */
  for (u32 i = 0; i < BUS_FASTMEM_LUT_SLOTS; i++)
    s_fastmem_lut[i] = get_lut_fastmem_pointer(i << BUS_FASTMEM_LUT_PAGE_SHIFT, NULL);

  for (u32 b = 0; b < 12u; b++)
  {
    const u32 base_address = s_fastmem_mirror_bases[b];

    if (cpu_virtual_to_physical_address(base_address) >= s_ram_mapped_size)
      continue;

    u8* ram_ptr = s_ram + (base_address & s_ram_mask);
    for (u32 address = 0; address < s_ram_size; address += BUS_FASTMEM_LUT_PAGE_SIZE)
    {
      const u32 lut_index = (base_address + address) >> BUS_FASTMEM_LUT_PAGE_SHIFT;
      s_fastmem_lut[lut_index] = get_lut_fastmem_pointer(base_address + address, ram_ptr);
      ram_ptr += BUS_FASTMEM_LUT_PAGE_SIZE;
    }
  }
}

static bool allocate_fastmem_arena(void)
{
  if (s_fastmem_arena_base)
    return true;

  void* base = mmap(NULL, BUS_FASTMEM_ARENA_SIZE, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED)
  {
    ERROR_LOG("Failed to reserve %zu bytes for fastmem arena: %m",
              (size_t)BUS_FASTMEM_ARENA_SIZE);
    return false;
  }

  s_fastmem_arena_base = (u8*)base;
  INFO_LOG("Fastmem arena reserved: %p (%zu bytes)",
           (void*)s_fastmem_arena_base, (size_t)BUS_FASTMEM_ARENA_SIZE);
  return true;
}

static void release_fastmem_arena(void)
{
  if (!s_fastmem_arena_base)
    return;
  munmap(s_fastmem_arena_base, BUS_FASTMEM_ARENA_SIZE);
  s_fastmem_arena_base   = NULL;
  s_fastmem_arena_active = false;
}

static void unmap_fastmem_views_mmap(void)
{
  if (!s_fastmem_arena_base || !s_fastmem_arena_active)
    return;

  /* Reverse of map_fastmem_views_mmap: replace each mirror with a fresh
   * PROT_NONE reservation so the slot stays inside the arena but any
   * access to it segfaults again. */
  for (u32 b = 0; b < 12u; b++)
  {
    const u32 base_address = s_fastmem_mirror_bases[b];
    if (cpu_virtual_to_physical_address(base_address) >= s_ram_mapped_size)
      continue;
    void* slot = s_fastmem_arena_base + base_address;
    void* p = mmap(slot, s_ram_size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
      WARNING_LOG("Failed to unmap fastmem mirror at %p (%m)", slot);
  }
  s_fastmem_arena_active = false;
}

static void map_fastmem_views_mmap(void)
{
  if (!allocate_fastmem_arena())
    return;
  if (s_fastmem_arena_active)
    unmap_fastmem_views_mmap();

  for (u32 b = 0; b < 12u; b++)
  {
    const u32 base_address = s_fastmem_mirror_bases[b];

    if (cpu_virtual_to_physical_address(base_address) >= s_ram_mapped_size)
      continue;

    u8* slot = s_fastmem_arena_base + base_address;
    void* mapped = memmap_map_shared_memory(s_shmem_handle, MEMMAP_RAM_OFFSET,
                                            slot, s_ram_size,
                                            MEMMAP_PAGE_PROTECT_READ_WRITE);
    if (!mapped)
    {
      ERROR_LOG("Failed to map fastmem mirror %u (base=0x%08X) at %p", b, base_address, slot);
      continue;
    }
  }
  s_fastmem_arena_active = true;
}

void bus_remap_fastmem_views(void)
{
  switch (g_settings.cpu_fastmem_mode)
  {
    case CPU_FASTMEM_MODE_MMAP:
      map_fastmem_views_mmap();
      break;

    case CPU_FASTMEM_MODE_LUT:
      /* If we previously had MMap mode active, tear those views down. */
      unmap_fastmem_views_mmap();
      map_fastmem_views_lut();
      break;

    case CPU_FASTMEM_MODE_DISABLED:
    default:
      unmap_fastmem_views_mmap();
      break;
  }

  cpu_update_memory_pointers();
}

bool bus_can_use_fastmem_for_address(virtual_memory_address_t address)
{
  const physical_memory_address_t paddr = cpu_virtual_to_physical_address(address);
  return paddr < BUS_RAM_MIRROR_END;
}

void bus_set_ram_code_page(u32 index)
{
  if (index >= BUS_RAM_8MB_CODE_PAGE_COUNT || code_bit_get(index))
    return;
  code_bit_set(index);
  set_ram_page_writable(index, false);
}

void bus_clear_ram_code_page(u32 index)
{
  if (index >= BUS_RAM_8MB_CODE_PAGE_COUNT || !code_bit_get(index))
    return;
  code_bit_clr(index);
  set_ram_page_writable(index, true);
}

bool bus_is_ram_code_page(u32 index)
{
  if (index >= BUS_RAM_8MB_CODE_PAGE_COUNT)
    return false;
  return code_bit_get(index);
}

static void set_ram_page_writable(u32 page_index, bool writable)
{
  if (!s_ram)
    return;
  if (!memmap_mem_protect(&s_ram[page_index << HOST_PAGE_SHIFT], HOST_PAGE_SIZE,
                          writable ? MEMMAP_PAGE_PROTECT_READ_WRITE : MEMMAP_PAGE_PROTECT_READ_ONLY))
  {
    ERROR_LOG("Failed to set RAM host page %u (%p) to %s", page_index,
              (void*)&s_ram[page_index * HOST_PAGE_SIZE], writable ? "read-write" : "read-only");
  }
}

void bus_clear_ram_code_page_flags(void)
{
  memset(s_ram_code_bits, 0, sizeof(s_ram_code_bits));
  if (s_ram && !memmap_mem_protect(s_ram, BUS_RAM_8MB_SIZE, MEMMAP_PAGE_PROTECT_READ_WRITE))
    ERROR_LOG("Failed to restore RAM protection to read-write.");
}

bool bus_is_code_page_address(physical_memory_address_t address)
{
  if (!bus_is_ram_address(address))
    return false;
  return code_bit_get((address & s_ram_mask) >> HOST_PAGE_SHIFT);
}

bool bus_has_code_pages_in_range(physical_memory_address_t start_address, u32 size)
{
  if (!bus_is_ram_address(start_address))
    return false;

  start_address = (start_address & s_ram_mask);
  const u32 end_address = start_address + size;
  while (start_address < end_address)
  {
    const u32 code_page_index = start_address >> HOST_PAGE_SHIFT;
    if (code_bit_get(code_page_index))
      return true;
    start_address += HOST_PAGE_SIZE;
  }
  return false;
}

u8*  bus_get_ram_pointer(void)              { return s_ram; }
u8*  bus_get_unprotected_ram_pointer(void)  { return s_unprotected_ram; }
u32  bus_get_ram_size(void)                 { return s_ram_size; }
u32  bus_get_ram_mapped_size(void)          { return s_ram_mapped_size; }
u32  bus_get_ram_mask(void)                 { return s_ram_mask; }
u8*  bus_get_bios_pointer(void)             { return s_bios; }

const tick_count_t* bus_get_exp1_access_time(void)  { return s_exp1_access_time;  }
const tick_count_t* bus_get_exp2_access_time(void)  { return s_exp2_access_time;  }
const tick_count_t* bus_get_bios_access_time(void)  { return s_bios_access_time;  }
const tick_count_t* bus_get_cdrom_access_time(void) { return s_cdrom_access_time; }
const tick_count_t* bus_get_spu_access_time(void)   { return s_spu_access_time;   }

const tick_count_t* bus_get_memory_access_time_ptr(physical_memory_address_t address,
                                                   memory_access_size_t size)
{
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_MIRROR_SIZE))
    return &s_bios_access_time[(u32)size];
  if (address >= BUS_EXP1_BASE && address < (BUS_EXP1_BASE + BUS_EXP1_SIZE))
    return &s_exp1_access_time[(u32)size];
  return NULL;
}

bus_memory_region_t bus_get_memory_region_for_address(physical_memory_address_t address)
{
  if (address < BUS_RAM_2MB_SIZE)
    return BUS_MEMORY_REGION_RAM;
  if (address < BUS_RAM_MIRROR_END)
    return (bus_memory_region_t)((u32)BUS_MEMORY_REGION_RAM + (address / BUS_RAM_2MB_SIZE));
  if (address >= BUS_EXP1_BASE && address < (BUS_EXP1_BASE + BUS_EXP1_SIZE))
    return BUS_MEMORY_REGION_EXP1;
  if (address >= cpu_get_scratchpad_address() && address < (cpu_get_scratchpad_address() + cpu_get_scratchpad_size()))
    return BUS_MEMORY_REGION_SCRATCHPAD;
  if (address >= BUS_BIOS_BASE && address < (BUS_BIOS_BASE + BUS_BIOS_SIZE))
    return BUS_MEMORY_REGION_BIOS;
  return BUS_MEMORY_REGION_COUNT;
}

typedef struct {
  physical_memory_address_t start;
  physical_memory_address_t end;
  bool                      writable;
} code_region_range_t;

static const code_region_range_t s_code_region_ranges[BUS_MEMORY_REGION_COUNT] = {
  { 0,                          BUS_RAM_2MB_SIZE,                 true  },
  { BUS_RAM_2MB_SIZE,           BUS_RAM_2MB_SIZE * 2u,            true  },
  { BUS_RAM_2MB_SIZE * 2u,      BUS_RAM_2MB_SIZE * 3u,            true  },
  { BUS_RAM_2MB_SIZE * 3u,      BUS_RAM_MIRROR_END,               true  },
  { BUS_EXP1_BASE,              BUS_EXP1_BASE + BUS_EXP1_SIZE,    false },
  { 0,                          0,                                true  },
  { BUS_BIOS_BASE,              BUS_BIOS_BASE + BUS_BIOS_SIZE,    false },
};

physical_memory_address_t bus_get_memory_region_start(bus_memory_region_t region)
{
  if (region == BUS_MEMORY_REGION_SCRATCHPAD)
    return cpu_get_scratchpad_address();
  return s_code_region_ranges[(u32)region].start;
}

physical_memory_address_t bus_get_memory_region_end(bus_memory_region_t region)
{
  if (region == BUS_MEMORY_REGION_SCRATCHPAD)
    return cpu_get_scratchpad_address() + cpu_get_scratchpad_size();
  return s_code_region_ranges[(u32)region].end;
}

bool bus_is_memory_region_writable(bus_memory_region_t region)
{
  return s_code_region_ranges[(u32)region].writable;
}

u8* bus_get_memory_region_pointer(bus_memory_region_t region)
{
  switch (region)
  {
    case BUS_MEMORY_REGION_RAM:         return s_unprotected_ram;
    case BUS_MEMORY_REGION_RAM_MIRROR1: return s_unprotected_ram + (BUS_RAM_2MB_SIZE      & s_ram_mask);
    case BUS_MEMORY_REGION_RAM_MIRROR2: return s_unprotected_ram + ((BUS_RAM_2MB_SIZE*2u) & s_ram_mask);
    case BUS_MEMORY_REGION_RAM_MIRROR3: return s_unprotected_ram + ((BUS_RAM_2MB_SIZE*3u) & s_ram_mask);
    case BUS_MEMORY_REGION_EXP1:        return NULL;
    case BUS_MEMORY_REGION_SCRATCHPAD:  return cpu_get_scratchpad_pointer();
    case BUS_MEMORY_REGION_BIOS:        return s_bios;
    default:                            return NULL;
  }
}

static ALWAYS_INLINE_RELEASE bool masked_memory_compare(const u8* pattern, const u8* mask,
                                                        u32 pattern_length, const u8* mem)
{
  if (!mask)
    return memcmp(mem, pattern, pattern_length) == 0;
  for (u32 i = 0; i < pattern_length; i++)
  {
    if ((mem[i] & mask[i]) != (pattern[i] & mask[i]))
      return false;
  }
  return true;
}

physical_memory_address_t bus_search_memory(physical_memory_address_t start_address, const u8* pattern,
                                            const u8* mask, u32 pattern_length)
{
  bus_memory_region_t current_region = bus_get_memory_region_for_address(start_address);
  if (current_region == BUS_MEMORY_REGION_COUNT)
    return 0xFFFFFFFFu;

  physical_memory_address_t current_address = start_address;
  while (current_region != BUS_MEMORY_REGION_COUNT)
  {
    const u8* mem = bus_get_memory_region_pointer(current_region);
    const physical_memory_address_t region_start = bus_get_memory_region_start(current_region);
    const physical_memory_address_t region_end   = bus_get_memory_region_end(current_region);

    if (mem)
    {
      physical_memory_address_t region_offset   = current_address - region_start;
      physical_memory_address_t bytes_remaining = region_end       - current_address;
      while (bytes_remaining >= pattern_length)
      {
        if (masked_memory_compare(pattern, mask, pattern_length, mem + region_offset))
          return region_start + region_offset;
        region_offset++;
        bytes_remaining--;
      }
    }

    /* Skip the RAM mirrors after walking the canonical RAM region. */
    if (current_region == BUS_MEMORY_REGION_RAM)
      current_region = BUS_MEMORY_REGION_EXP1;
    else
      current_region = (bus_memory_region_t)((u32)current_region + 1u);

    if (current_region != BUS_MEMORY_REGION_COUNT)
      current_address = bus_get_memory_region_start(current_region);
  }
  return 0xFFFFFFFFu;
}

void bus_add_tty_character(char ch)
{
  if (ch == '\r')
    return;

  if (ch == '\n')
  {
    if (s_tty_line_buffer_len > 0)
    {
      s_tty_line_buffer[s_tty_line_buffer_len] = '\0';
      GENERIC_LOG(LOG_CHANNEL_TTY, LOG_LEVEL_INFO, LOG_COLOR_STRONG_BLUE, "%s", s_tty_line_buffer);
    }
    s_tty_line_buffer_len = 0;
    return;
  }

  if (ch == '\0')
    return;

  if (s_tty_line_buffer_len + 1u < TTY_LINE_BUFFER_CAPACITY)
    s_tty_line_buffer[s_tty_line_buffer_len++] = ch;
}

void bus_add_tty_string(const char* str, size_t len)
{
  for (size_t i = 0; i < len; i++)
    bus_add_tty_character(str[i]);
}

bool bus_inject_executable(const u8* buffer, size_t buffer_size, bool set_pc, Error* error)
{
  bios_psexe_header_t header;
  if (buffer_size < sizeof(header)) {
    Error_set_string(error, "Executable does not contain a header.");
    return false;
  }

  memcpy(&header, buffer, sizeof(header));
  if (!bios_is_valid_psexe_header(&header, buffer_size)) {
    Error_set_string(error, "Executable does not contain a valid header.");
    return false;
  }

  if (header.memfill_size > 0u &&
      !cpu_safe_zero_memory_bytes(header.memfill_start & ~UINT32_C(3),
                                  AlignDownPow2(header.memfill_size, 4u))) {
    Error_set_string_fmt(error, "Failed to zero %u bytes of memory at address 0x%08X.",
                         header.memfill_size, header.memfill_start);
    return false;
  }

  const u32 data_load_size =
    (u32)((buffer_size - sizeof(bios_psexe_header_t)) < header.file_size
          ? (buffer_size - sizeof(bios_psexe_header_t))
          : header.file_size);
  if (data_load_size > 0u) {
    if (!cpu_safe_write_memory_bytes(header.load_address,
                                     buffer + sizeof(bios_psexe_header_t),
                                     data_load_size)) {
      Error_set_string_fmt(error, "Failed to upload %u bytes to memory at address 0x%08X.",
                           data_load_size, header.load_address);
      return false;
    }
  }

  if (set_pc) {
    const u32 r_pc = header.initial_pc;
    const u32 r_gp = header.initial_gp;
    const u32 r_sp = header.initial_sp_base + header.initial_sp_offset;
    g_cpu_state.regs.named.gp = r_gp;
    if (r_sp != 0u) {
      g_cpu_state.regs.named.sp = r_sp;
      g_cpu_state.regs.named.fp = r_sp;
    }
    cpu_set_pc(r_pc);
  }

  return true;
}

static u32 unknown_read_byte(virtual_memory_address_t address)
{
  ERROR_LOG("Invalid byte read at address 0x%08X, pc 0x%08X", address, cpu_get_pc());
  return 0xFFFFFFFFu;
}
static u32 unknown_read_half(virtual_memory_address_t address)
{
  ERROR_LOG("Invalid halfword read at address 0x%08X, pc 0x%08X", address, cpu_get_pc());
  return 0xFFFFFFFFu;
}
static u32 unknown_read_word(virtual_memory_address_t address)
{
  ERROR_LOG("Invalid word read at address 0x%08X, pc 0x%08X", address, cpu_get_pc());
  return 0xFFFFFFFFu;
}

static void unknown_write_byte(virtual_memory_address_t address, u32 value)
{
  ERROR_LOG("Invalid byte write at address 0x%08X, value 0x%08X, pc 0x%08X",
            address, value, cpu_get_pc());
  cpu_set_bus_error(true);
}
static void unknown_write_half(virtual_memory_address_t address, u32 value)
{
  ERROR_LOG("Invalid halfword write at address 0x%08X, value 0x%08X, pc 0x%08X",
            address, value, cpu_get_pc());
  cpu_set_bus_error(true);
}
static void unknown_write_word(virtual_memory_address_t address, u32 value)
{
  ERROR_LOG("Invalid word write at address 0x%08X, value 0x%08X, pc 0x%08X",
            address, value, cpu_get_pc());
  cpu_set_bus_error(true);
}

static void ignore_write(virtual_memory_address_t address, u32 value)
{
  (void)address; (void)value;
}

static u32 unmapped_read_byte(virtual_memory_address_t address)
{
  cpu_set_bus_error(true);
  return unknown_read_byte(address);
}
static u32 unmapped_read_half(virtual_memory_address_t address)
{
  cpu_set_bus_error(true);
  return unknown_read_half(address);
}
static u32 unmapped_read_word(virtual_memory_address_t address)
{
  cpu_set_bus_error(true);
  return unknown_read_word(address);
}
static void unmapped_write_byte(virtual_memory_address_t address, u32 value)
{
  cpu_set_bus_error(true);
  unknown_write_byte(address, value);
}
static void unmapped_write_half(virtual_memory_address_t address, u32 value)
{
  cpu_set_bus_error(true);
  unknown_write_half(address, value);
}
static void unmapped_write_word(virtual_memory_address_t address, u32 value)
{
  cpu_set_bus_error(true);
  unknown_write_word(address, value);
}

static u32 ram_read_byte(virtual_memory_address_t address)
{
  BUS_CYCLES(BUS_RAM_READ_TICKS);
  return (u32)s_ram[address & s_ram_mask];
}
static u32 ram_read_half(virtual_memory_address_t address)
{
  BUS_CYCLES(BUS_RAM_READ_TICKS);
  u16 v;
  memcpy(&v, &s_ram[address & s_ram_mask], sizeof(u16));
  return (u32)v;
}
static u32 ram_read_word(virtual_memory_address_t address)
{
  BUS_CYCLES(BUS_RAM_READ_TICKS);
  u32 v;
  memcpy(&v, &s_ram[address & s_ram_mask], sizeof(u32));
  return v;
}
bus_ram_write_tap_fn g_bus_ram_write_tap = NULL;

/* Slow-path RAM stores must coexist with the SMC page-protection scheme.
 * `s_ram` is mapped READ-ONLY for any 4 KB host page that currently holds
 * compiled JIT blocks; fastmem JIT stores rely on a SEGV → backpatch flow
 * to handle that.  Slow-path stores from C have no such recovery path, so
 * before writing we invalidate any compiled blocks on the page (which also
 * flips the host-page protection back to READ-WRITE in s_ram). */
static ALWAYS_INLINE void ram_smc_invalidate_if_needed(u32 paddr)
{
  const u32 page_index = paddr >> HOST_PAGE_SHIFT;
  if (bus_is_ram_code_page(page_index))
    cpu_code_cache_invalidate_blocks_with_page_index(page_index);
}

static void ram_write_byte(virtual_memory_address_t address, u32 value)
{
  const u32 paddr = address & s_ram_mask;
  ram_smc_invalidate_if_needed(paddr);
  s_ram[paddr] = (u8)value;
  if (g_bus_ram_write_tap) g_bus_ram_write_tap(paddr, 1u, value & 0xFFu,
                                               g_cpu_state.current_instruction_pc);
}
static void ram_write_half(virtual_memory_address_t address, u32 value)
{
  const u32 paddr = address & s_ram_mask;
  ram_smc_invalidate_if_needed(paddr);
  const u16 v = (u16)value;
  memcpy(&s_ram[paddr], &v, sizeof(u16));
  if (g_bus_ram_write_tap) g_bus_ram_write_tap(paddr, 2u, value & 0xFFFFu,
                                               g_cpu_state.current_instruction_pc);
}
static void ram_write_word(virtual_memory_address_t address, u32 value)
{
  const u32 paddr = address & s_ram_mask;
  ram_smc_invalidate_if_needed(paddr);
  memcpy(&s_ram[paddr], &value, sizeof(u32));
  if (g_bus_ram_write_tap) g_bus_ram_write_tap(paddr, 4u, value,
                                               g_cpu_state.current_instruction_pc);
}

static u32 bios_read_byte(virtual_memory_address_t address)
{
  BUS_CYCLES(s_bios_access_time[0]);
  return (u32)s_bios[address & 0x7FFFFu];
}
static u32 bios_read_half(virtual_memory_address_t address)
{
  BUS_CYCLES(s_bios_access_time[1]);
  u16 v;
  memcpy(&v, &s_bios[address & 0x7FFFFu], sizeof(u16));
  return (u32)v;
}
static u32 bios_read_word(virtual_memory_address_t address)
{
  BUS_CYCLES(s_bios_access_time[2]);
  u32 v;
  memcpy(&v, &s_bios[address & 0x7FFFFu], sizeof(u32));
  return v;
}

static u32 scratchpad_read_byte(virtual_memory_address_t address)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
    return unknown_read_byte(address);
  return (u32)cpu_get_scratchpad_pointer()[off];
}
static u32 scratchpad_read_half(virtual_memory_address_t address)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
    return unknown_read_half(address);
  u16 v;
  memcpy(&v, &cpu_get_scratchpad_pointer()[off], sizeof(u16));
  return (u32)v;
}
static u32 scratchpad_read_word(virtual_memory_address_t address)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
    return unknown_read_word(address);
  u32 v;
  memcpy(&v, &cpu_get_scratchpad_pointer()[off], sizeof(u32));
  return v;
}
static void scratchpad_write_byte(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
  {
    unknown_write_byte(address, value);
    return;
  }
  cpu_get_scratchpad_pointer()[off] = (u8)value;
}
static void scratchpad_write_half(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
  {
    unknown_write_half(address, value);
    return;
  }
  const u16 v = (u16)value;
  memcpy(&cpu_get_scratchpad_pointer()[off], &v, sizeof(u16));
}
static void scratchpad_write_word(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_MEMORY_LUT_PAGE_MASK;
  if (off >= cpu_get_scratchpad_size())
  {
    unknown_write_word(address, value);
    return;
  }
  memcpy(&cpu_get_scratchpad_pointer()[off], &value, sizeof(u32));
}

static u32 cache_control_read_any(virtual_memory_address_t address)
{
  if (address != 0xFFFE0130u)
    return unknown_read_word(address);
  return cpu_get_cache_control_bits();
}
static void cache_control_write_any(virtual_memory_address_t address, u32 value)
{
  if (address != 0xFFFE0130u)
  {
    unknown_write_word(address, value);
    return;
  }
  DEV_LOG("Cache control <- 0x%08X", value);
  cpu_set_cache_control_bits(value);
}

static void icache_write_common(virtual_memory_address_t address, u32 value, size_t bytes)
{
  const u32 line   = cpu_get_icache_line(address);
  u32* line_data   = &cpu_get_icache_data()[line * CPU_ICACHE_WORDS_PER_LINE];
  const u32 offset = cpu_get_icache_line_offset(address);
  cpu_get_icache_tags()[line] = cpu_get_icache_tag_for_address(address) | CPU_ICACHE_INVALID_BITS;
  memcpy((u8*)line_data + offset, &value, bytes);
}
static u32 icache_read(virtual_memory_address_t address)
{
  const u32 line   = cpu_get_icache_line(address);
  const u32* line_data = &cpu_get_icache_data()[line * CPU_ICACHE_WORDS_PER_LINE];
  const u32 offset = cpu_get_icache_line_offset(address);
  u32 result;
  memcpy(&result, (const u8*)line_data + offset, sizeof(result));
  return result;
}
static u32  icache_read_byte(virtual_memory_address_t a) { return icache_read(a); }
static u32  icache_read_half(virtual_memory_address_t a) { return icache_read(a); }
static u32  icache_read_word(virtual_memory_address_t a) { return icache_read(a); }
static void icache_write_byte(virtual_memory_address_t a, u32 v) { icache_write_common(a, v, 1); }
static void icache_write_half(virtual_memory_address_t a, u32 v) { icache_write_common(a, v, 2); }
static void icache_write_word(virtual_memory_address_t a, u32 v) { icache_write_common(a, v, 4); }

static u32 exp1_read_byte(virtual_memory_address_t address)
{
  BUS_CYCLES(s_exp1_access_time[0]);
  return (u32)pio_device_read(address & BUS_EXP1_MASK);
}
static u32 exp1_read_half(virtual_memory_address_t address)
{
  BUS_CYCLES(s_exp1_access_time[1]);
  const u32 off = address & BUS_EXP1_MASK;
  u32 v = pio_device_read(off);
  v |= (u32)pio_device_read(off + 1u) << 8;
  return v;
}
static u32 exp1_read_word(virtual_memory_address_t address)
{
  BUS_CYCLES(s_exp1_access_time[2]);
  const u32 off = address & BUS_EXP1_MASK;
  u32 v = pio_device_read(off);
  v |= (u32)pio_device_read(off + 1u) << 8;
  v |= (u32)pio_device_read(off + 2u) << 16;
  v |= (u32)pio_device_read(off + 3u) << 24;
  return v;
}
static void exp1_write_byte(virtual_memory_address_t address, u32 value)
{
  pio_device_write(address & BUS_EXP1_MASK, (u8)value);
}
static void exp1_write_half(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_EXP1_MASK;
  pio_device_write(off,        (u8)value);
  pio_device_write(off + 1u,   (u8)(value >> 8));
}
static void exp1_write_word(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_EXP1_MASK;
  pio_device_write(off,        (u8)value);
  pio_device_write(off + 1u,   (u8)(value >> 8));
  pio_device_write(off + 2u,   (u8)(value >> 16));
  pio_device_write(off + 3u,   (u8)(value >> 24));
}

static u32 exp2_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  BUS_CYCLES(s_exp2_access_time[(u32)size]);
  const u32 off = address & BUS_EXP2_MASK;
  if (off == 0x21u)
    return 0x04u | 0x08u;                /* rx/tx buffer empty */
  if (off >= 0x60u && off <= 0x67u)
    return 0xFFFFFFFFu;                  /* nocash expansion area */
  if (off == 0x80u)
    return 0xFFFFFFFFu;                  /* pcsx_present() */
  WARNING_LOG("EXP2 read: 0x%08X", address);
  return 0xFFFFFFFFu;
}
static u32 exp2_read_byte(virtual_memory_address_t a) { return exp2_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 exp2_read_half(virtual_memory_address_t a) { return exp2_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 exp2_read_word(virtual_memory_address_t a) { return exp2_read_common(a, MEMORY_ACCESS_SIZE_WORD); }

static void kernel_initialized_hook(void)
{
  if (s_kernel_initialize_hook_run)
    return;
  INFO_LOG("Kernel initialized.");
  s_kernel_initialize_hook_run = true;
  system_handle_kernel_initialized();
}

static void exp2_write_common(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_EXP2_MASK;
  if (off == 0x23u || off == 0x80u)
  {
    bus_add_tty_character((char)value);
    return;
  }
  if (off == 0x41u || off == 0x42u)
  {
    const u32 post_code = value & 0x0Fu;
    DEV_LOG("BIOS POST status: %02X", post_code);
    if (post_code == 0x07u)
      kernel_initialized_hook();
    return;
  }
  if (off == 0x70u)
  {
    DEV_LOG("BIOS POST2 status: %02X", value & 0x0Fu);
    return;
  }
  WARNING_LOG("EXP2 write: 0x%08X <- 0x%08X", address, value);
}
static void exp2_write_byte(virtual_memory_address_t a, u32 v) { exp2_write_common(a, v); }
static void exp2_write_half(virtual_memory_address_t a, u32 v) { exp2_write_common(a, v); }
static void exp2_write_word(virtual_memory_address_t a, u32 v) { exp2_write_common(a, v); }

static u32 exp3_read_byte(virtual_memory_address_t address)
{
  WARNING_LOG("EXP3 read: 0x%08X", address);
  return 0xFFFFFFFFu;
}
static u32 exp3_read_half(virtual_memory_address_t a) { return exp3_read_byte(a); }
static u32 exp3_read_word(virtual_memory_address_t a) { return exp3_read_byte(a); }

static void exp3_write_common(virtual_memory_address_t address, u32 value)
{
  const u32 off = address & BUS_EXP3_MASK;
  if (off == 0u)
  {
    const u32 post_code = value & 0x0Fu;
    WARNING_LOG("BIOS POST3 status: %02X", post_code);
    if (post_code == 0x07u)
      kernel_initialized_hook();
  }
}
static void exp3_write_byte(virtual_memory_address_t a, u32 v) { exp3_write_common(a, v); }
static void exp3_write_half(virtual_memory_address_t a, u32 v) { exp3_write_common(a, v); }
static void exp3_write_word(virtual_memory_address_t a, u32 v) { exp3_write_common(a, v); }

static u32 sio2_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  if (system_is_using_known_ps1_bios())
  {
    cpu_set_bus_error(true);
    if (size == MEMORY_ACCESS_SIZE_BYTE) return unknown_read_byte(address);
    if (size == MEMORY_ACCESS_SIZE_HALFWORD) return unknown_read_half(address);
    return unknown_read_word(address);
  }
  WARNING_LOG("SIO2 read: 0x%08X", address);
  return 0;
}
static u32 sio2_read_byte(virtual_memory_address_t a) { return sio2_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 sio2_read_half(virtual_memory_address_t a) { return sio2_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 sio2_read_word(virtual_memory_address_t a) { return sio2_read_common(a, MEMORY_ACCESS_SIZE_WORD); }

static void sio2_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  if (system_is_using_known_ps1_bios())
  {
    cpu_set_bus_error(true);
    if (size == MEMORY_ACCESS_SIZE_BYTE) { unknown_write_byte(address, value); return; }
    if (size == MEMORY_ACCESS_SIZE_HALFWORD) { unknown_write_half(address, value); return; }
    unknown_write_word(address, value); return;
  }
  WARNING_LOG("SIO2 write: 0x%08X <- 0x%08X", address, value);
}
static void sio2_write_byte(virtual_memory_address_t a, u32 v) { sio2_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void sio2_write_half(virtual_memory_address_t a, u32 v) { sio2_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void sio2_write_word(virtual_memory_address_t a, u32 v) { sio2_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* MEMCTRL */
static u32 memctrl_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_MEMCTRL_MASK;
  const u32 index  = FIXUP_WORD_OFFSET(size, offset) / 4u;
  if (index >= BUS_MEMCTRL_REG_COUNT)
    return 0;
  u32 value = s_MEMCTRL.regs[index];
  value     = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void memctrl_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_MEMCTRL_MASK;
  const u32 index  = FIXUP_WORD_OFFSET(size, offset) / 4u;
  if (index >= BUS_MEMCTRL_REG_COUNT)
    return;
  value = FIXUP_WORD_WRITE_VALUE(size, offset, value);
  const u32 write_mask = (index == 8u) ? COMDELAY_WRITE_MASK : MEMDELAY_WRITE_MASK;
  const u32 new_value  = (s_MEMCTRL.regs[index] & ~write_mask) | (value & write_mask);
  if (s_MEMCTRL.regs[index] != new_value)
  {
    s_MEMCTRL.regs[index] = new_value;
    recalculate_memory_timings();
  }
}
static u32 memctrl_read_byte(virtual_memory_address_t a) { return memctrl_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 memctrl_read_half(virtual_memory_address_t a) { return memctrl_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 memctrl_read_word(virtual_memory_address_t a) { return memctrl_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void memctrl_write_byte(virtual_memory_address_t a, u32 v) { memctrl_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void memctrl_write_half(virtual_memory_address_t a, u32 v) { memctrl_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void memctrl_write_word(virtual_memory_address_t a, u32 v) { memctrl_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* MEMCTRL2 (RAM_SIZE register). */
static u32 memctrl2_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_MEMCTRL2_MASK;
  if (offset != 0x00u)
  {
    if (size == MEMORY_ACCESS_SIZE_BYTE) return unknown_read_byte(address);
    if (size == MEMORY_ACCESS_SIZE_HALFWORD) return unknown_read_half(address);
    return unknown_read_word(address);
  }
  BUS_CYCLES(2);
  return s_RAM_SIZE.bits;
}
static void memctrl2_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_MEMCTRL2_MASK;
  if (offset != 0x00u)
  {
    if (size == MEMORY_ACCESS_SIZE_BYTE) { unknown_write_byte(address, value); return; }
    if (size == MEMORY_ACCESS_SIZE_HALFWORD) { unknown_write_half(address, value); return; }
    unknown_write_word(address, value); return;
  }
  if (s_RAM_SIZE.bits != value)
  {
    DEV_LOG("RAM size register set to 0x%08X", value);
    const ram_size_reg_t old = s_RAM_SIZE;
    s_RAM_SIZE.bits = value;
    if (s_RAM_SIZE.b.memory_window != old.b.memory_window)
      update_mapped_ram_size();
  }
}
static u32 memctrl2_read_byte(virtual_memory_address_t a) { return memctrl2_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 memctrl2_read_half(virtual_memory_address_t a) { return memctrl2_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 memctrl2_read_word(virtual_memory_address_t a) { return memctrl2_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void memctrl2_write_byte(virtual_memory_address_t a, u32 v) { memctrl2_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void memctrl2_write_half(virtual_memory_address_t a, u32 v) { memctrl2_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void memctrl2_write_word(virtual_memory_address_t a, u32 v) { memctrl2_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* PAD */
static u32 pad_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_PAD_MASK;
  u32 value = pad_read_register(FIXUP_HALFWORD_OFFSET(size, offset));
  value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void pad_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_PAD_MASK;
  pad_write_register(FIXUP_HALFWORD_OFFSET(size, offset),
                     FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
}
static u32 pad_read_byte(virtual_memory_address_t a) { return pad_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 pad_read_half(virtual_memory_address_t a) { return pad_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 pad_read_word(virtual_memory_address_t a) { return pad_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void pad_write_byte(virtual_memory_address_t a, u32 v) { pad_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void pad_write_half(virtual_memory_address_t a, u32 v) { pad_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void pad_write_word(virtual_memory_address_t a, u32 v) { pad_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* SIO */
static u32 sio_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_SIO_MASK;
  u32 value = sio_read_register(FIXUP_HALFWORD_OFFSET(size, offset));
  value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void sio_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_SIO_MASK;
  sio_write_register(FIXUP_HALFWORD_OFFSET(size, offset),
                     FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
}
static u32 sio_read_byte(virtual_memory_address_t a) { return sio_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 sio_read_half(virtual_memory_address_t a) { return sio_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 sio_read_word(virtual_memory_address_t a) { return sio_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void sio_write_byte(virtual_memory_address_t a, u32 v) { sio_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void sio_write_half(virtual_memory_address_t a, u32 v) { sio_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void sio_write_word(virtual_memory_address_t a, u32 v) { sio_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* CDROM (byte-wide registers; halfword/word reads compose them). */
static u32 cdrom_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_CDROM_MASK;
  u32 value;
  if (size == MEMORY_ACCESS_SIZE_WORD)
  {
    const u32 b0 = (u32)cdrom_read_register(offset);
    const u32 b1 = (u32)cdrom_read_register(offset + 1u);
    const u32 b2 = (u32)cdrom_read_register(offset + 2u);
    const u32 b3 = (u32)cdrom_read_register(offset + 3u);
    value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
  }
  else if (size == MEMORY_ACCESS_SIZE_HALFWORD)
  {
    const u32 lsb = (u32)cdrom_read_register(offset);
    const u32 msb = (u32)cdrom_read_register(offset + 1u);
    value = lsb | (msb << 8);
  }
  else
  {
    value = (u32)cdrom_read_register(offset);
  }
  BUS_CYCLES(s_cdrom_access_time[(u32)size]);
  return value;
}
static void cdrom_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_CDROM_MASK;
  if (size == MEMORY_ACCESS_SIZE_WORD)
  {
    cdrom_write_register(offset,        (u8)(value & 0xFFu));
    cdrom_write_register(offset + 1u,   (u8)((value >> 8)  & 0xFFu));
    cdrom_write_register(offset + 2u,   (u8)((value >> 16) & 0xFFu));
    cdrom_write_register(offset + 3u,   (u8)((value >> 24) & 0xFFu));
  }
  else if (size == MEMORY_ACCESS_SIZE_HALFWORD)
  {
    cdrom_write_register(offset,        (u8)(value & 0xFFu));
    cdrom_write_register(offset + 1u,   (u8)((value >> 8) & 0xFFu));
  }
  else
  {
    cdrom_write_register(offset, (u8)value);
  }
}
static u32 cdrom_read_byte_h(virtual_memory_address_t a) { return cdrom_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 cdrom_read_half_h(virtual_memory_address_t a) { return cdrom_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 cdrom_read_word_h(virtual_memory_address_t a) { return cdrom_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void cdrom_write_byte_h(virtual_memory_address_t a, u32 v) { cdrom_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void cdrom_write_half_h(virtual_memory_address_t a, u32 v) { cdrom_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void cdrom_write_word_h(virtual_memory_address_t a, u32 v) { cdrom_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* GPU */
static u32 gpu_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_GPU_MASK;
  u32 value = gpu_read_register(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void gpu_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_GPU_MASK;
  gpu_write_register(FIXUP_WORD_OFFSET(size, offset),
                     FIXUP_WORD_WRITE_VALUE(size, offset, value));
}
static u32 gpu_read_byte_h(virtual_memory_address_t a) { return gpu_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 gpu_read_half_h(virtual_memory_address_t a) { return gpu_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 gpu_read_word_h(virtual_memory_address_t a) { return gpu_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void gpu_write_byte_h(virtual_memory_address_t a, u32 v) { gpu_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void gpu_write_half_h(virtual_memory_address_t a, u32 v) { gpu_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void gpu_write_word_h(virtual_memory_address_t a, u32 v) { gpu_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* MDEC */
static u32 mdec_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_MDEC_MASK;
  u32 value = mdec_read_register(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void mdec_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_MDEC_MASK;
  mdec_write_register(FIXUP_WORD_OFFSET(size, offset),
                      FIXUP_WORD_WRITE_VALUE(size, offset, value));
}
static u32 mdec_read_byte_h(virtual_memory_address_t a) { return mdec_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 mdec_read_half_h(virtual_memory_address_t a) { return mdec_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 mdec_read_word_h(virtual_memory_address_t a) { return mdec_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void mdec_write_byte_h(virtual_memory_address_t a, u32 v) { mdec_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void mdec_write_half_h(virtual_memory_address_t a, u32 v) { mdec_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void mdec_write_word_h(virtual_memory_address_t a, u32 v) { mdec_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* INTC */
static u32 intc_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_INTERRUPT_CONTROLLER_MASK;
  u32 value = interrupt_controller_read_register(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void intc_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_INTERRUPT_CONTROLLER_MASK;
  interrupt_controller_write_register(FIXUP_WORD_OFFSET(size, offset),
                                      FIXUP_WORD_WRITE_VALUE(size, offset, value));
}
static u32 intc_read_byte(virtual_memory_address_t a) { return intc_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 intc_read_half(virtual_memory_address_t a) { return intc_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 intc_read_word(virtual_memory_address_t a) { return intc_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void intc_write_byte(virtual_memory_address_t a, u32 v) { intc_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void intc_write_half(virtual_memory_address_t a, u32 v) { intc_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void intc_write_word(virtual_memory_address_t a, u32 v) { intc_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* TIMERS */
static u32 timers_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_TIMERS_MASK;
  u32 value = timers_read_register(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void timers_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_TIMERS_MASK;
  timers_write_register(FIXUP_WORD_OFFSET(size, offset),
                        FIXUP_WORD_WRITE_VALUE(size, offset, value));
}
static u32 timers_read_byte(virtual_memory_address_t a) { return timers_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 timers_read_half(virtual_memory_address_t a) { return timers_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 timers_read_word(virtual_memory_address_t a) { return timers_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void timers_write_byte(virtual_memory_address_t a, u32 v) { timers_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void timers_write_half(virtual_memory_address_t a, u32 v) { timers_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void timers_write_word(virtual_memory_address_t a, u32 v) { timers_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

/* DMA */
static u32 dma_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_DMA_MASK;
  u32 value = dma_read_register(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}
static void dma_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_DMA_MASK;
  dma_write_register(FIXUP_WORD_OFFSET(size, offset),
                     FIXUP_WORD_WRITE_VALUE(size, offset, value));
}
static u32 dma_read_byte_h(virtual_memory_address_t a) { return dma_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 dma_read_half_h(virtual_memory_address_t a) { return dma_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 dma_read_word_h(virtual_memory_address_t a) { return dma_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void dma_write_byte_h(virtual_memory_address_t a, u32 v) { dma_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void dma_write_half_h(virtual_memory_address_t a, u32 v) { dma_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void dma_write_word_h(virtual_memory_address_t a, u32 v) { dma_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

static u32 spu_read_common(virtual_memory_address_t address, memory_access_size_t size)
{
  const u32 offset = address & BUS_SPU_MASK;
  u32 value;
  if (size == MEMORY_ACCESS_SIZE_WORD)
  {
    const u16 lsb = spu_read_register(offset);
    const u16 msb = spu_read_register(offset + 2u);
    value = (u32)lsb | ((u32)msb << 16);
  }
  else if (size == MEMORY_ACCESS_SIZE_HALFWORD)
  {
    value = (u32)spu_read_register(offset);
  }
  else
  {
    const u16 v16 = spu_read_register(FIXUP_HALFWORD_OFFSET(size, offset));
    value = FIXUP_HALFWORD_READ_VALUE(size, offset, (u32)v16);
  }
  BUS_CYCLES(s_spu_access_time[(u32)size]);
  return value;
}
static void spu_write_common(virtual_memory_address_t address, u32 value, memory_access_size_t size)
{
  const u32 offset = address & BUS_SPU_MASK;
  if (size == MEMORY_ACCESS_SIZE_WORD)
  {
    spu_write_register(offset,       (u16)value);
    spu_write_register(offset + 2u,  (u16)(value >> 16));
    return;
  }
  if (size == MEMORY_ACCESS_SIZE_HALFWORD)
  {
    spu_write_register(offset, (u16)value);
    return;
  }
  /* byte write */
  if (address & 1u)
    return;
  spu_write_register(offset, (u16)FIXUP_HALFWORD_READ_VALUE(size, offset, value));
}
static u32 spu_read_byte_h(virtual_memory_address_t a) { return spu_read_common(a, MEMORY_ACCESS_SIZE_BYTE); }
static u32 spu_read_half_h(virtual_memory_address_t a) { return spu_read_common(a, MEMORY_ACCESS_SIZE_HALFWORD); }
static u32 spu_read_word_h(virtual_memory_address_t a) { return spu_read_common(a, MEMORY_ACCESS_SIZE_WORD); }
static void spu_write_byte_h(virtual_memory_address_t a, u32 v) { spu_write_common(a, v, MEMORY_ACCESS_SIZE_BYTE); }
static void spu_write_half_h(virtual_memory_address_t a, u32 v) { spu_write_common(a, v, MEMORY_ACCESS_SIZE_HALFWORD); }
static void spu_write_word_h(virtual_memory_address_t a, u32 v) { spu_write_common(a, v, MEMORY_ACCESS_SIZE_WORD); }

typedef struct {
  bus_memory_read_handler_t  read_byte[256];
  bus_memory_read_handler_t  read_half[256];
  bus_memory_read_handler_t  read_word[256];
  bus_memory_write_handler_t write_byte[256];
  bus_memory_write_handler_t write_half[256];
  bus_memory_write_handler_t write_word[256];
} hw_dispatch_t;

static hw_dispatch_t s_hw_dispatch;
static bool          s_hw_dispatch_built = false;

static void hw_dispatch_set_range(u32 raddr, u32 rsize,
                                   bus_memory_read_handler_t  rb,
                                  bus_memory_read_handler_t  rh,
                                  bus_memory_read_handler_t  rw,
                                  bus_memory_write_handler_t wb,
                                  bus_memory_write_handler_t wh,
                                  bus_memory_write_handler_t ww) 
{
  for (u32 taddr = raddr; taddr < (raddr + rsize); taddr += 16u)
  {
    const u32 i = (taddr >> 4) & 0xFFu;
    s_hw_dispatch.read_byte [i] = rb;
    s_hw_dispatch.read_half [i] = rh;
    s_hw_dispatch.read_word [i] = rw;
    s_hw_dispatch.write_byte[i] = wb;
    s_hw_dispatch.write_half[i] = wh;
    s_hw_dispatch.write_word[i] = ww;
  }
}

static void build_hw_dispatch(void)
{
  for (u32 i = 0; i < 256u; i++)
  {
    s_hw_dispatch.read_byte [i] = unmapped_read_byte;
    s_hw_dispatch.read_half [i] = unmapped_read_half;
    s_hw_dispatch.read_word [i] = unmapped_read_word;
    s_hw_dispatch.write_byte[i] = unmapped_write_byte;
    s_hw_dispatch.write_half[i] = unmapped_write_half;
    s_hw_dispatch.write_word[i] = unmapped_write_word;
  }

  hw_dispatch_set_range(BUS_MEMCTRL_BASE,  BUS_MEMCTRL_SIZE,
                        memctrl_read_byte,  memctrl_read_half,  memctrl_read_word,
                        memctrl_write_byte, memctrl_write_half, memctrl_write_word);
  hw_dispatch_set_range(BUS_PAD_BASE,      BUS_PAD_SIZE,
                        pad_read_byte,     pad_read_half,     pad_read_word,
                        pad_write_byte,    pad_write_half,    pad_write_word);
  hw_dispatch_set_range(BUS_SIO_BASE,      BUS_SIO_SIZE,
                        sio_read_byte,     sio_read_half,     sio_read_word,
                        sio_write_byte,    sio_write_half,    sio_write_word);
  hw_dispatch_set_range(BUS_MEMCTRL2_BASE, BUS_MEMCTRL2_SIZE,
                        memctrl2_read_byte, memctrl2_read_half, memctrl2_read_word,
                        memctrl2_write_byte, memctrl2_write_half, memctrl2_write_word);
  hw_dispatch_set_range(BUS_INTC_BASE,     BUS_INTC_SIZE,
                        intc_read_byte,    intc_read_half,    intc_read_word,
                        intc_write_byte,   intc_write_half,   intc_write_word);
  hw_dispatch_set_range(BUS_DMA_BASE,      BUS_DMA_SIZE,
                        dma_read_byte_h,   dma_read_half_h,   dma_read_word_h,
                        dma_write_byte_h,  dma_write_half_h,  dma_write_word_h);
  hw_dispatch_set_range(BUS_TIMERS_BASE,   BUS_TIMERS_SIZE,
                        timers_read_byte,  timers_read_half,  timers_read_word,
                        timers_write_byte, timers_write_half, timers_write_word);
  hw_dispatch_set_range(BUS_CDROM_BASE,    BUS_CDROM_SIZE,
                        cdrom_read_byte_h, cdrom_read_half_h, cdrom_read_word_h,
                        cdrom_write_byte_h,cdrom_write_half_h,cdrom_write_word_h);
  hw_dispatch_set_range(BUS_GPU_BASE,      BUS_GPU_SIZE,
                        gpu_read_byte_h,   gpu_read_half_h,   gpu_read_word_h,
                        gpu_write_byte_h,  gpu_write_half_h,  gpu_write_word_h);
  hw_dispatch_set_range(BUS_MDEC_BASE,     BUS_MDEC_SIZE,
                        mdec_read_byte_h,  mdec_read_half_h,  mdec_read_word_h,
                        mdec_write_byte_h, mdec_write_half_h, mdec_write_word_h);
  hw_dispatch_set_range(BUS_SPU_BASE,      BUS_SPU_SIZE,
                        spu_read_byte_h,   spu_read_half_h,   spu_read_word_h,
                        spu_write_byte_h,  spu_write_half_h,  spu_write_word_h);

  s_hw_dispatch_built = true;
}

static u32 hardware_read_byte(virtual_memory_address_t address)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  return s_hw_dispatch.read_byte[(address >> 4) & 0xFFu](address);
}
static u32 hardware_read_half(virtual_memory_address_t address)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  return s_hw_dispatch.read_half[(address >> 4) & 0xFFu](address);
}
static u32 hardware_read_word(virtual_memory_address_t address)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  return s_hw_dispatch.read_word[(address >> 4) & 0xFFu](address);
}
static void hardware_write_byte(virtual_memory_address_t address, u32 value)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  s_hw_dispatch.write_byte[(address >> 4) & 0xFFu](address, value);
}
static void hardware_write_half(virtual_memory_address_t address, u32 value)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  s_hw_dispatch.write_half[(address >> 4) & 0xFFu](address, value);
}
static void hardware_write_word(virtual_memory_address_t address, u32 value)
{
  if (!s_hw_dispatch_built) build_hw_dispatch();
  s_hw_dispatch.write_word[(address >> 4) & 0xFFu](address, value);
}

#define KUSEG 0x00000000u
#define KSEG0 0x80000000u
#define KSEG1 0xA0000000u
#define KSEG2 0xC0000000u

static void set_handlers(void)
{
  clear_handlers(s_memory_handlers);
  clear_handlers(s_memory_handlers_isc);

  const u32 scratchpad_addr = cpu_get_scratchpad_address();

#define SET(table, start, sz, rb, rh, rw, wb, wh, ww)                                                                  \
  set_handler_for_region((table), (start), (sz), (rb), (rh), (rw), (wb), (wh), (ww))

  /* KUSEG / KSEG0 share the cached memory map; KSEG1 is the uncached mirror.
   * Cache-isolated mode (icache shadow) is set up alongside via _isc tables. */

  /* KUSEG cached */
  SET(s_memory_handlers, KUSEG | BUS_RAM_BASE, BUS_RAM_MIRROR_SIZE,
      ram_read_byte, ram_read_half, ram_read_word, ram_write_byte, ram_write_half, ram_write_word);
  SET(s_memory_handlers, KUSEG | scratchpad_addr, 0x1000u,
      scratchpad_read_byte, scratchpad_read_half, scratchpad_read_word,
      scratchpad_write_byte, scratchpad_write_half, scratchpad_write_word);
  SET(s_memory_handlers, KUSEG | BUS_BIOS_BASE, BUS_BIOS_MIRROR_SIZE,
      bios_read_byte, bios_read_half, bios_read_word, ignore_write, ignore_write, ignore_write);
  SET(s_memory_handlers, KUSEG | BUS_EXP1_BASE, BUS_EXP1_SIZE,
      exp1_read_byte, exp1_read_half, exp1_read_word,
      exp1_write_byte, exp1_write_half, exp1_write_word);
  SET(s_memory_handlers, KUSEG | BUS_HW_BASE, BUS_HW_SIZE,
      hardware_read_byte, hardware_read_half, hardware_read_word,
      hardware_write_byte, hardware_write_half, hardware_write_word);
  SET(s_memory_handlers, KUSEG | BUS_EXP2_BASE, BUS_EXP2_SIZE,
      exp2_read_byte, exp2_read_half, exp2_read_word,
      exp2_write_byte, exp2_write_half, exp2_write_word);
  SET(s_memory_handlers, KUSEG | BUS_EXP3_BASE, BUS_EXP3_SIZE,
      exp3_read_byte, exp3_read_half, exp3_read_word,
      exp3_write_byte, exp3_write_half, exp3_write_word);
  SET(s_memory_handlers, KUSEG | BUS_SIO2_BASE, BUS_SIO2_SIZE,
      sio2_read_byte, sio2_read_half, sio2_read_word,
      sio2_write_byte, sio2_write_half, sio2_write_word);
  SET(s_memory_handlers_isc, KUSEG, 0x80000000u,
      icache_read_byte, icache_read_half, icache_read_word,
      icache_write_byte, icache_write_half, icache_write_word);

  /* KSEG0 cached */
  SET(s_memory_handlers, KSEG0 | BUS_RAM_BASE, BUS_RAM_MIRROR_SIZE,
      ram_read_byte, ram_read_half, ram_read_word, ram_write_byte, ram_write_half, ram_write_word);
  SET(s_memory_handlers, KSEG0 | scratchpad_addr, 0x1000u,
      scratchpad_read_byte, scratchpad_read_half, scratchpad_read_word,
      scratchpad_write_byte, scratchpad_write_half, scratchpad_write_word);
  SET(s_memory_handlers, KSEG0 | BUS_BIOS_BASE, BUS_BIOS_MIRROR_SIZE,
      bios_read_byte, bios_read_half, bios_read_word, ignore_write, ignore_write, ignore_write);
  SET(s_memory_handlers, KSEG0 | BUS_EXP1_BASE, BUS_EXP1_SIZE,
      exp1_read_byte, exp1_read_half, exp1_read_word,
      exp1_write_byte, exp1_write_half, exp1_write_word);
  SET(s_memory_handlers, KSEG0 | BUS_HW_BASE, BUS_HW_SIZE,
      hardware_read_byte, hardware_read_half, hardware_read_word,
      hardware_write_byte, hardware_write_half, hardware_write_word);
  SET(s_memory_handlers, KSEG0 | BUS_EXP2_BASE, BUS_EXP2_SIZE,
      exp2_read_byte, exp2_read_half, exp2_read_word,
      exp2_write_byte, exp2_write_half, exp2_write_word);
  SET(s_memory_handlers, KSEG0 | BUS_EXP3_BASE, BUS_EXP3_SIZE,
      exp3_read_byte, exp3_read_half, exp3_read_word,
      exp3_write_byte, exp3_write_half, exp3_write_word);
  SET(s_memory_handlers, KSEG0 | BUS_SIO2_BASE, BUS_SIO2_SIZE,
      sio2_read_byte, sio2_read_half, sio2_read_word,
      sio2_write_byte, sio2_write_half, sio2_write_word);
  SET(s_memory_handlers_isc, KSEG0, 0x20000000u,
      icache_read_byte, icache_read_half, icache_read_word,
      icache_write_byte, icache_write_half, icache_write_word);

#define SETUC(start, sz, rb, rh, rw, wb, wh, ww)                                                                       \
  do {                                                                                                                 \
    SET(s_memory_handlers,     (start), (sz), (rb), (rh), (rw), (wb), (wh), (ww));                                     \
    SET(s_memory_handlers_isc, (start), (sz), (rb), (rh), (rw), (wb), (wh), (ww));                                     \
  } while (0)

  SETUC(KSEG1 | BUS_RAM_BASE,  BUS_RAM_MIRROR_SIZE,
        ram_read_byte, ram_read_half, ram_read_word, ram_write_byte, ram_write_half, ram_write_word);
  SETUC(KSEG1 | BUS_BIOS_BASE, BUS_BIOS_MIRROR_SIZE,
        bios_read_byte, bios_read_half, bios_read_word, ignore_write, ignore_write, ignore_write);
  SETUC(KSEG1 | BUS_EXP1_BASE, BUS_EXP1_SIZE,
        exp1_read_byte, exp1_read_half, exp1_read_word,
        exp1_write_byte, exp1_write_half, exp1_write_word);
  SETUC(KSEG1 | BUS_HW_BASE,   BUS_HW_SIZE,
        hardware_read_byte, hardware_read_half, hardware_read_word,
        hardware_write_byte, hardware_write_half, hardware_write_word);
  SETUC(KSEG1 | BUS_EXP2_BASE, BUS_EXP2_SIZE,
        exp2_read_byte, exp2_read_half, exp2_read_word,
        exp2_write_byte, exp2_write_half, exp2_write_word);
  SETUC(KSEG1 | BUS_EXP3_BASE, BUS_EXP3_SIZE,
        exp3_read_byte, exp3_read_half, exp3_read_word,
        exp3_write_byte, exp3_write_half, exp3_write_word);
  SETUC(KSEG1 | BUS_SIO2_BASE, BUS_SIO2_SIZE,
        sio2_read_byte, sio2_read_half, sio2_read_word,
        sio2_write_byte, sio2_write_half, sio2_write_word);

  SETUC(KSEG2 | 0xFFFE0000u, 0x1000u,
        cache_control_read_any, cache_control_read_any, cache_control_read_any,
        cache_control_write_any, cache_control_write_any, cache_control_write_any);

#undef SETUC
#undef SET
}

static void update_mapped_ram_size(void)
{
  const u32 prev_mapped_size = s_ram_mapped_size;

#define SET(table, start, sz, rb, rh, rw, wb, wh, ww)                                                                  \
  set_handler_for_region((table), (start), (sz), (rb), (rh), (rw), (wb), (wh), (ww))

  switch (s_RAM_SIZE.b.memory_window)
  {
    case 4: /* 2MB memory + 6MB unmapped (Rock-Climbing JP) */
    {
      const u32 mapped_size    = BUS_RAM_2MB_SIZE;
      const u32 unmapped_start = BUS_RAM_BASE + mapped_size;
      const u32 unmapped_size  = BUS_RAM_MIRROR_SIZE - mapped_size;
      SET(s_memory_handlers, KUSEG | unmapped_start, unmapped_size,
          unmapped_read_byte, unmapped_read_half, unmapped_read_word,
          unmapped_write_byte, unmapped_write_half, unmapped_write_word);
      SET(s_memory_handlers, KSEG0 | unmapped_start, unmapped_size,
          unmapped_read_byte, unmapped_read_half, unmapped_read_word,
          unmapped_write_byte, unmapped_write_half, unmapped_write_word);
      SET(s_memory_handlers, KSEG1 | unmapped_start, unmapped_size,
          unmapped_read_byte, unmapped_read_half, unmapped_read_word,
          unmapped_write_byte, unmapped_write_half, unmapped_write_word);
      s_ram_mapped_size = mapped_size;
    }
    break;

    case 0: case 1: case 2: case 3: case 6: case 7:
      WARNING_LOG("Unhandled memory window 0x%X (register 0x%08X). Please report.",
                  s_RAM_SIZE.b.memory_window, s_RAM_SIZE.bits);
      /* fall through */
    case 5: /* default 8MB */
    {
      const u32 remap_start = BUS_RAM_BASE + BUS_RAM_2MB_SIZE;
      const u32 remap_size  = BUS_RAM_MIRROR_SIZE - BUS_RAM_2MB_SIZE;
      SET(s_memory_handlers, KUSEG | remap_start, remap_size,
          ram_read_byte, ram_read_half, ram_read_word,
          ram_write_byte, ram_write_half, ram_write_word);
      SET(s_memory_handlers, KSEG0 | remap_start, remap_size,
          ram_read_byte, ram_read_half, ram_read_word,
          ram_write_byte, ram_write_half, ram_write_word);
      SET(s_memory_handlers, KSEG1 | remap_start, remap_size,
          ram_read_byte, ram_read_half, ram_read_word,
          ram_write_byte, ram_write_half, ram_write_word);
      s_ram_mapped_size = BUS_RAM_8MB_SIZE;
    }
    break;
  }

#undef SET

  if (prev_mapped_size != s_ram_mapped_size)
    bus_remap_fastmem_views();
}

static void clear_handlers(void** handlers)
{
  if (!handlers)
    return;
  for (u32 size = 0; size < 3u; size++)
  {
    bus_memory_read_handler_t* read_handlers =
      bus_offset_read_handler_array(handlers, (memory_access_size_t)size);
    const bus_memory_read_handler_t read_handler =
      (size == 0) ? unmapped_read_byte : ((size == 1) ? unmapped_read_half : unmapped_read_word);
    MemsetPtrs((void**)read_handlers, (void*)(uintptr_t)read_handler, BUS_MEMORY_LUT_SIZE);

    bus_memory_write_handler_t* write_handlers =
      bus_offset_write_handler_array(handlers, (memory_access_size_t)size);
    const bus_memory_write_handler_t write_handler =
      (size == 0) ? unmapped_write_byte : ((size == 1) ? unmapped_write_half : unmapped_write_word);
    MemsetPtrs((void**)write_handlers, (void*)(uintptr_t)write_handler, BUS_MEMORY_LUT_SIZE);
  }
}

static void set_handler_for_region(void** handlers, virtual_memory_address_t address, u32 size,
                                    bus_memory_read_handler_t  read_byte,
                                   bus_memory_read_handler_t  read_half,
                                   bus_memory_read_handler_t  read_word,
                                   bus_memory_write_handler_t write_byte,
                                   bus_memory_write_handler_t write_half,
                                   bus_memory_write_handler_t write_word) 
{
  DebugAssert(IsAlignedPow2(size, BUS_MEMORY_LUT_PAGE_SIZE));
  const u32 start_page = address / BUS_MEMORY_LUT_PAGE_SIZE;
  const u32 num_pages  = (size + (BUS_MEMORY_LUT_PAGE_SIZE - 1u)) / BUS_MEMORY_LUT_PAGE_SIZE;

  for (u32 acc_size = 0; acc_size < 3u; acc_size++)
  {
    bus_memory_read_handler_t* read_h =
      bus_offset_read_handler_array(handlers, (memory_access_size_t)acc_size) + start_page;
    const bus_memory_read_handler_t r =
      (acc_size == 0) ? read_byte : ((acc_size == 1) ? read_half : read_word);
    MemsetPtrs((void**)read_h, (void*)(uintptr_t)r, num_pages);

    bus_memory_write_handler_t* write_h =
      bus_offset_write_handler_array(handlers, (memory_access_size_t)acc_size) + start_page;
    const bus_memory_write_handler_t w =
      (acc_size == 0) ? write_byte : ((acc_size == 1) ? write_half : write_word);
    MemsetPtrs((void**)write_h, (void*)(uintptr_t)w, num_pages);
  }
}

void** bus_get_memory_handlers(bool isolate_cache, bool swap_caches)
{
  if (!isolate_cache)
    return s_memory_handlers;
  (void)swap_caches;
  return s_memory_handlers_isc;
}
