/*
 * Helpers exposed to other core modules (bus, gte, system, dma, ...) without
 * being part of the public CPU API.  Stays in the header rather than in .c
 * because callers want them inlined.  Pure interpreter - no recompiler thunks.
 */

#ifndef CUPID_CORE_CPU_CORE_PRIVATE_H
#define CUPID_CORE_CPU_CORE_PRIVATE_H

#include "bus.h"
#include "cpu_core.h"
#include "cpu_types.h"

#include "common/types.h"

/* Mask used to strip cached/uncached/user-mode segment bits before fetching
 * or load/store.  KUSEG covers the first 2GB, the others only 512MB. */
enum {
  CPU_KSEG_MASK  = 0x1FFFFFFFu,
  CPU_KUSEG_MASK = 0x7FFFFFFFu,
};

void cpu_set_pc(u32 new_pc);

/* Exception entry points.  RaiseException(excode) builds CAUSE for the
 * current instruction; the (CAUSE_bits, EPC) overload is used by RFE-like
 * paths that supply both directly. */
void cpu_raise_exception(cpu_exception_t excode);
void cpu_raise_exception_bits(u32 cause_bits, u32 epc);
void cpu_raise_break_exception(u32 cause_bits, u32 epc, u32 instruction_bits);

/* COP0 IM/IP intersection: an interrupt is pending when SR.IEc is set and
 * any of the eight (Im & Ip) bits in CAUSE is set. */
ALWAYS_INLINE bool cpu_has_pending_interrupt(void)
{
  return g_cpu_state.cop0_regs.sr.IEc &&
         (((g_cpu_state.cop0_regs.cause.bits & g_cpu_state.cop0_regs.sr.bits) & (0xFFu << 8)) != 0u);
}

/* Kicks the dispatcher out of its tight loop the next time it checks
 * downcount.  Used by writes to SR/CAUSE which may newly enable an IRQ. */
ALWAYS_INLINE void cpu_check_for_pending_interrupt(void)
{
  if (cpu_has_pending_interrupt())
    g_cpu_state.downcount = 0u;
}

void cpu_dispatch_interrupt(void);

/* KUSEG / KSEG0 are cached; KSEG1 and KSEG2 are not. */
ALWAYS_INLINE bool cpu_is_cached_address(virtual_memory_address_t address)
{
  return (address >> 29) <= 4u;
}

ALWAYS_INLINE u32 cpu_get_icache_line(virtual_memory_address_t address)
{
  return (address >> 4) & 0xFFu;
}
ALWAYS_INLINE u32 cpu_get_icache_line_offset(virtual_memory_address_t address)
{
  return address & (CPU_ICACHE_LINE_SIZE - 1u);
}
ALWAYS_INLINE u32 cpu_get_icache_line_word_offset(virtual_memory_address_t address)
{
  return (address >> 2) & 0x03u;
}
ALWAYS_INLINE u32 cpu_get_icache_tag_for_address(virtual_memory_address_t address)
{
  return address & CPU_ICACHE_TAG_ADDRESS_MASK;
}

/* The bottom 4 bits of each icache tag double as a per-word valid bitmap.
 * On a fill the tag is OR'd with the inverse-of-bits-already-loaded mask
 * for the word offset where the fill *started*, so a partial-line miss
 * doesn't pretend the trailing words are valid. */
ALWAYS_INLINE u32 cpu_get_icache_fill_tag_for_address(virtual_memory_address_t address)
{
  static const u32 invalid_bits[4] = {0u, 1u, 3u, 7u};
  return cpu_get_icache_tag_for_address(address) | invalid_bits[(address >> 2) & 0x03u];
}
ALWAYS_INLINE u32 cpu_get_icache_tag_mask_for_address(virtual_memory_address_t address)
{
  static const u32 mask[4] = {CPU_ICACHE_TAG_ADDRESS_MASK | 1u, CPU_ICACHE_TAG_ADDRESS_MASK | 2u,
                              CPU_ICACHE_TAG_ADDRESS_MASK | 4u, CPU_ICACHE_TAG_ADDRESS_MASK | 8u};
  return mask[(address >> 2) & 0x03u];
}

ALWAYS_INLINE bool cpu_compare_icache_tag(virtual_memory_address_t address)
{
  const u32 line = cpu_get_icache_line(address);
  return (g_cpu_state.icache_tags[line] & cpu_get_icache_tag_mask_for_address(address)) ==
         cpu_get_icache_tag_for_address(address);
}

tick_count_t cpu_get_instruction_read_ticks(virtual_memory_address_t address);
tick_count_t cpu_get_icache_fill_ticks(virtual_memory_address_t address);
u32  cpu_fill_icache(virtual_memory_address_t address);
void cpu_check_and_update_icache_tags(u32 line_count);

ALWAYS_INLINE cpu_segment_t cpu_get_segment_for_address(virtual_memory_address_t address)
{
  switch (address >> 29) {
    case 0x00: case 0x01: case 0x02: case 0x03:
      return CPU_SEGMENT_KUSEG;
    case 0x04:
      return CPU_SEGMENT_KSEG0;
    case 0x05:
      return CPU_SEGMENT_KSEG1;
    case 0x06: case 0x07: default:
      return CPU_SEGMENT_KSEG2;
  }
}

ALWAYS_INLINE physical_memory_address_t cpu_virtual_to_physical(virtual_memory_address_t address)
{
  /* KUSEG: bottom 2GB; everything else: bottom 512MB. */
  return address & ((address & 0x80000000u) ? CPU_KSEG_MASK : CPU_KUSEG_MASK);
}

ALWAYS_INLINE virtual_memory_address_t cpu_physical_to_virtual(physical_memory_address_t address,
                                                               cpu_segment_t segment)
{
  static const virtual_memory_address_t bases[4] = {0x00000000u, 0x80000000u, 0xA0000000u, 0xE0000000u};
  return bases[(u32)segment] | address;
}

bus_memory_read_handler_t  cpu_get_memory_read_handler (virtual_memory_address_t address, memory_access_size_t size);
bus_memory_write_handler_t cpu_get_memory_write_handler(virtual_memory_address_t address, memory_access_size_t size);

bool  cpu_safe_read_instruction(virtual_memory_address_t addr, u32* value);
void* cpu_get_direct_read_memory_pointer (virtual_memory_address_t address, memory_access_size_t size,
                                          tick_count_t* read_ticks);
void* cpu_get_direct_write_memory_pointer(virtual_memory_address_t address, memory_access_size_t size);

/* GTE pipeline stall.  COP2 instructions take a variable number of cycles;
 * a cfc/mfc that reads back from the GTE before completion has to wait. */
ALWAYS_INLINE void cpu_add_gte_ticks(tick_count_t ticks)
{
  g_cpu_state.gte_completion_tick = g_cpu_state.pending_ticks + (u32)ticks + 1u;
}

ALWAYS_INLINE void cpu_stall_until_gte_complete(void)
{
  g_cpu_state.pending_ticks = (g_cpu_state.gte_completion_tick > g_cpu_state.pending_ticks) ?
                                g_cpu_state.gte_completion_tick : 
                                g_cpu_state.pending_ticks;
}

ALWAYS_INLINE void cpu_add_muldiv_ticks(tick_count_t ticks)
{
  g_cpu_state.muldiv_completion_tick = g_cpu_state.pending_ticks + (u32)ticks;
}

ALWAYS_INLINE void cpu_stall_until_muldiv_complete(void)
{
  g_cpu_state.pending_ticks = (g_cpu_state.muldiv_completion_tick > g_cpu_state.pending_ticks) ?
                                g_cpu_state.muldiv_completion_tick : 
                                g_cpu_state.pending_ticks;
}

/* MULT timing: number of "leading sign bits" buckets the multiplier hardware
 * uses.  Subtract one because the cycle count includes the instruction
 * fetch tick we already accounted for. */
ALWAYS_INLINE tick_count_t cpu_get_mult_ticks_signed(s32 rs)
{
  if (rs < 0)
    return (rs >= -2048) ? (6 - 1) : ((rs >= -1048576) ? (9 - 1) : (13 - 1));
  return (rs < 0x800) ? (6 - 1) : (rs < 0x100000 ? (9 - 1) : (13 - 1));
}

ALWAYS_INLINE tick_count_t cpu_get_mult_ticks_unsigned(u32 rs)
{
  return (rs < 0x800u) ? (6 - 1) : ((rs < 0x100000u) ? (9 - 1) : (13 - 1));
}

ALWAYS_INLINE tick_count_t cpu_get_div_ticks(void)
{
  return 36 - 1;
}

/* TTY trampolines: BIOS A0/B0 syscalls 0x03/0x09/0x3c/0x35/etc carry
 * printable strings that we capture into the TTY log. */
void cpu_handle_a0_syscall(void);
void cpu_handle_b0_syscall(void);

#endif /* CUPID_CORE_CPU_CORE_PRIVATE_H */
