/*
 * 7-channel DMA controller.  The original used per-channel C++ templates so
 * each channel got a fully-inlined transfer function; in C we dispatch on
 * dma_channel_t inside one function and let the optimiser hoist the
 * branches.  Behaviour is identical.
 *
 * Linked-list mode (GPU only on real hardware) walks header words in RAM
 * until the terminator (low 24 bits == 0xFFFFFF).  We have to test the
 * terminator on the *next* address, not the read header, since MADR is 24
 * bits wide and the terminator IS the address.
 *
 * External (parallel-agent) dependencies (resolved via includes):
 *   bus.h         ; bus_get_ram_pointer/_mask/_mapped_size,
 *                     bus_get_dma_ram_tick_count, bus_is_ram_code_page
 *   cpu_core.h    ; cpu_add_pending_ticks
 *   spu/mdec/cdrom/pad/gpu (h's)
 *
 * The few APIs not yet ported (gpu_begin_dma_write, GPU dump recorder,
 * cpu_code_cache_invalidate_blocks_with_page_index, settings_get_dma_*) are
 * declared weak so we link cleanly until they land.  When NULL the
 * functions either no-op or fall back to baked-in defaults.
 */

#include "core/bus.h"
#include "core/cdrom.h"
#include "core/cpu_core.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/mdec.h"
#include "core/pad.h"
#include "core/spu.h"
#include "core/timing_event.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(DMA);

extern void gpu_begin_dma_write_pre(void) __attribute__((weak));   /* unused alias */

extern bool gpu_begin_dma_write(void) __attribute__((weak));
extern void gpu_end_dma_write(void)   __attribute__((weak));
extern void gpu_dma_write(u32 address, u32 value) __attribute__((weak));
extern void gpu_dma_read (u32* dest, u32 word_count) __attribute__((weak));

extern void cpu_code_cache_invalidate_blocks_with_page_index(u32 page) __attribute__((weak));

extern tick_count_t settings_get_dma_halt_ticks      (void) __attribute__((weak));
extern tick_count_t settings_get_dma_max_slice_ticks (void) __attribute__((weak));
extern int          settings_get_cdrom_read_speedup  (void) __attribute__((weak));

static tick_count_t default_halt_ticks(void)      { return 100; }
static tick_count_t default_max_slice_ticks(void) { return 1000; }
static int          default_cdrom_read_speedup(void) { return 1; }

#define BASE_ADDRESS_MASK     UINT32_C(0x00FFFFFF)
#define TRANSFER_ADDRESS_MASK UINT32_C(0x00FFFFFC)
#define LINKED_LIST_TERMINATOR UINT32_C(0x00FFFFFF)

#define LINKED_LIST_HEADER_READ_TICKS   8
#define LINKED_LIST_BLOCK_SETUP_TICKS   5
#define SLICE_SIZE_WHEN_TRANSMITTING_PAD 10
#define SLICE_SIZE_WHEN_DECODING_MDEC    100

typedef enum {
  SYNC_MODE_MANUAL      = 0,
  SYNC_MODE_REQUEST     = 1,
  SYNC_MODE_LINKED_LIST = 2,
  SYNC_MODE_RESERVED    = 3,
} dma_sync_mode_t;

typedef union {
  u32 bits;
  /* Manual mode view */
  struct {
    u32 word_count : 16;
    u32 _pad0      : 16;
  } manual;
  /* Request (block) mode view */
  struct {
    u32 block_size  : 16;
    u32 block_count : 16;
  } request;
} dma_block_control_t;

#define CHANNEL_CONTROL_WRITE_MASK UINT32_C(0x71770703)

typedef union {
  u32 bits;
  struct {
    u32 copy_to_device           : 1; /* bit 0 */
    u32 address_step_reverse     : 1; /* bit 1 */
    u32 _pad0                    : 6;
    u32 chopping_enable          : 1; /* bit 8 */
    u32 sync_mode                : 2; /* bit 9-10 */
    u32 _pad1                    : 5;
    u32 chopping_dma_window_size : 3; /* bit 16-18 */
    u32 _pad2                    : 1;
    u32 chopping_cpu_window_size : 3; /* bit 20-22 */
    u32 _pad3                    : 1;
    u32 enable_busy              : 1; /* bit 24 */
    u32 _pad4                    : 3;
    u32 start_trigger            : 1; /* bit 28 */
    u32 _pad5                    : 3;
  };
} dma_channel_control_t;

typedef struct {
  u32                   base_address;
  dma_block_control_t   block_control;
  dma_channel_control_t channel_control;
  bool                  request;
} dma_channel_state_t;

typedef union {
  u32 bits;
  struct {
    u32 MDECin_priority         : 3;
    u32 MDECin_master_enable    : 1;
    u32 MDECout_priority        : 3;
    u32 MDECout_master_enable   : 1;
    u32 GPU_priority            : 3;
    u32 GPU_master_enable       : 1;
    u32 CDROM_priority          : 3;
    u32 CDROM_master_enable     : 1;
    u32 SPU_priority            : 3;
    u32 SPU_master_enable       : 1;
    u32 PIO_priority            : 3;
    u32 PIO_master_enable       : 1;
    u32 OTC_priority            : 3;
    u32 OTC_master_enable       : 1;
    u32 priority_offset         : 3;
    u32 unused                  : 1;
  };
} dpcr_register_t;

static u8 dpcr_get_priority(dpcr_register_t r, dma_channel_t ch)
{
  return (u8)((r.bits >> ((u8)ch * 4u)) & 3u);
}
static bool dpcr_get_master_enable(dpcr_register_t r, dma_channel_t ch)
{
  return ((r.bits >> ((u8)ch * 4u + 3u)) & 1u) != 0;
}

#define DICR_WRITE_MASK UINT32_C(0x00FF803F)
#define DICR_RESET_MASK UINT32_C(0x7F000000)

typedef union {
  u32 bits;
  struct {
    u32 _pad0              : 15;
    u32 bus_error          : 1; /* bit 15 */
    u32 MDECin_irq_enable  : 1; /* bit 16 */
    u32 MDECout_irq_enable : 1;
    u32 GPU_irq_enable     : 1;
    u32 CDROM_irq_enable   : 1;
    u32 SPU_irq_enable     : 1;
    u32 PIO_irq_enable     : 1;
    u32 OTC_irq_enable     : 1; /* bit 22 */
    u32 master_enable      : 1; /* bit 23 */
    u32 MDECin_irq_flag    : 1; /* bit 24 */
    u32 MDECout_irq_flag   : 1;
    u32 GPU_irq_flag       : 1;
    u32 CDROM_irq_flag     : 1;
    u32 SPU_irq_flag       : 1;
    u32 PIO_irq_flag       : 1;
    u32 OTC_irq_flag       : 1; /* bit 30 */
    u32 master_flag        : 1; /* bit 31 */
  };
} dicr_register_t;

static bool dicr_get_irq_enabled(dicr_register_t r, dma_channel_t ch)
{
  return ((r.bits >> ((u8)ch + 16u)) & 1u) != 0;
}
static bool dicr_get_irq_flag(dicr_register_t r, dma_channel_t ch)
{
  return ((r.bits >> ((u8)ch + 24u)) & 1u) != 0;
}
static void dicr_set_irq_flag(dicr_register_t* r, dma_channel_t ch)
{
  r->bits |= (1u << ((u8)ch + 24u));
}
static bool dicr_should_set_irq_flag(dicr_register_t r, dma_channel_t ch)
{
  /* Bus errors set IRQ unconditionally; channel completion needs the master
   * flag enabled. */
  return ((r.bits >> ((u8)ch + 16u)) & ((r.bits >> 23u) & 1u)) != 0;
}
static void dicr_update_master_flag(dicr_register_t* r)
{
   r->master_flag = (((r->bits & (1u << 15)) != 0u) ||
                    (((r->bits & (1u << 23)) != 0u) && (r->bits & (0x7Fu << 24)) != 0u)) 
                     ? 1u : 0u;
}

typedef struct {
  u32* transfer_buffer;
  u32  transfer_buffer_cap; /* in words */
  timing_event_t  unhalt_event;
  tick_count_t    halt_ticks_remaining;

  dma_channel_state_t channels[DMA_NUM_CHANNELS];
  dpcr_register_t     DPCR;
  dicr_register_t     DICR;
} dma_state_t;

static const char s_unhalt_event_name[] = "DMA Transfer Unhalt";

ALIGN_TO_CACHE_LINE static dma_state_t s_state;
static u32 s_dbg_transfer_counts[DMA_NUM_CHANNELS];

__attribute__((unused)) static const char* const s_channel_names[DMA_NUM_CHANNELS] = {
  "MDECin", "MDECout", "GPU", "CDROM", "SPU", "PIO", "OTC"
};

static void         dma_clear_state(void);
static bool         dma_can_transfer_channel(dma_channel_t channel, bool ignore_halt);
static bool         dma_is_transfer_halted(void);
static void         dma_update_irq(void);
static void         dma_halt_transfer(tick_count_t duration);
static void         dma_unhalt_transfer(void* user, tick_count_t ticks, tick_count_t ticks_late);
static bool         dma_is_linked_list_terminator(u32 address);
static bool         dma_check_for_bus_error(dma_channel_t channel, dma_channel_state_t* cs,
                                            u32 address, u32 size);
static void         dma_complete_transfer(dma_channel_t channel, dma_channel_state_t* cs);

static bool         dma_transfer_channel(dma_channel_t channel);
static tick_count_t dma_transfer_memory_to_device(dma_channel_t channel, u32 address, u32 increment, u32 word_count);
static tick_count_t dma_transfer_device_to_memory(dma_channel_t channel, u32 address, u32 increment, u32 word_count);
static tick_count_t dma_get_max_slice_ticks(dma_channel_t channel, tick_count_t max_slice_size);

static tick_count_t dma_settings_halt_ticks(void)
{
  return settings_get_dma_halt_ticks ? settings_get_dma_halt_ticks() : default_halt_ticks();
}
static tick_count_t dma_settings_max_slice_ticks(void)
{
  return settings_get_dma_max_slice_ticks ? settings_get_dma_max_slice_ticks() : default_max_slice_ticks();
}

void dma_initialize(void)
{
  timing_event_init(&s_state.unhalt_event,
                    s_unhalt_event_name, (u32)(sizeof(s_unhalt_event_name) - 1),
                    1, 1, &dma_unhalt_transfer, NULL);
  timing_event_set_interval(&s_state.unhalt_event, dma_settings_halt_ticks());
  dma_reset();
}

void dma_shutdown(void)
{
  dma_clear_state();
  timing_event_deactivate(&s_state.unhalt_event);
  timing_event_destroy(&s_state.unhalt_event);
  free(s_state.transfer_buffer);
  s_state.transfer_buffer = NULL;
  s_state.transfer_buffer_cap = 0;
}

void dma_reset(void)
{
  dma_clear_state();
  timing_event_deactivate(&s_state.unhalt_event);
}

static void dma_clear_state(void)
{
  for (u32 i = 0; i < DMA_NUM_CHANNELS; i++)
  {
    dma_channel_state_t* cs = &s_state.channels[i];
    cs->base_address = 0;
    cs->block_control.bits = 0;
    cs->channel_control.bits = 0;
    cs->request = false;
  }

  s_state.DPCR.bits = 0x07654321u;
  s_state.DICR.bits = 0;
  s_state.halt_ticks_remaining = 0;
}

bool dma_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_s32(sw, &s_state.halt_ticks_remaining);

  for (u32 i = 0; i < DMA_NUM_CHANNELS; i++)
  {
    dma_channel_state_t* cs = &s_state.channels[i];
    state_wrapper_do_u32 (sw, &cs->base_address);
    state_wrapper_do_u32 (sw, &cs->block_control.bits);
    state_wrapper_do_u32 (sw, &cs->channel_control.bits);
    state_wrapper_do_bool(sw, &cs->request);
  }

  state_wrapper_do_u32(sw, &s_state.DPCR.bits);
  state_wrapper_do_u32(sw, &s_state.DICR.bits);

  if (state_wrapper_is_reading(sw))
  {
    if (s_state.halt_ticks_remaining > 0)
      timing_event_set_interval_and_schedule(&s_state.unhalt_event, s_state.halt_ticks_remaining);
    else
      timing_event_deactivate(&s_state.unhalt_event);
  }

  return !state_wrapper_has_error(sw);
}

u32 dma_read_register(u32 offset)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    switch (offset & 0x0Fu)
    {
      case 0x00:
        TRACE_LOG("DMA[%u] base address -> 0x%08X", channel_index,
                  s_state.channels[channel_index].base_address);
        return s_state.channels[channel_index].base_address;

      case 0x04:
        TRACE_LOG("DMA[%u] block control -> 0x%08X", channel_index,
                  s_state.channels[channel_index].block_control.bits);
        return s_state.channels[channel_index].block_control.bits;

      case 0x08:
        TRACE_LOG("DMA[%u] channel control -> 0x%08X", channel_index,
                  s_state.channels[channel_index].channel_control.bits);
        return s_state.channels[channel_index].channel_control.bits;

      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
    {
      TRACE_LOG("DPCR -> 0x%08X", s_state.DPCR.bits);
      return s_state.DPCR.bits;
    }
    if (offset == 0x74)
    {
      TRACE_LOG("DICR -> 0x%08X", s_state.DICR.bits);
      return s_state.DICR.bits;
    }
  }

  ERROR_LOG("Unhandled register read: %02X", offset);
  return UINT32_C(0xFFFFFFFF);
}

void dma_write_register(u32 offset, u32 value)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    dma_channel_state_t* state = &s_state.channels[channel_index];
    switch (offset & 0x0Fu)
    {
      case 0x00:
        state->base_address = value & BASE_ADDRESS_MASK;
        TRACE_LOG("DMA channel %u base address <- 0x%08X", channel_index, state->base_address);
        return;

      case 0x04:
        TRACE_LOG("DMA channel %u block control <- 0x%08X", channel_index, value);
        state->block_control.bits = value;
        return;

      case 0x08:
      {
        /* HACK: DMA runs in slices, so we can't wait for the current halt
         * time to finish before kicking the first block of a new channel.
         * FF8 etc. queue an SPU transfer mid-GPU transfer; if the SPU one
         * waits for the GPU unhalt to complete, the SPU IRQ is missed. */
        const bool ignore_halt = !state->channel_control.enable_busy && (value & (1u << 24));

         state->channel_control.bits =
          (state->channel_control.bits & ~CHANNEL_CONTROL_WRITE_MASK) | 
          (value & CHANNEL_CONTROL_WRITE_MASK);
        TRACE_LOG("DMA channel %u channel control <- 0x%08X", channel_index, state->channel_control.bits);

        /* OTC needs the start_trigger bit to actually request a transfer. */
        if ((dma_channel_t)channel_index == DMA_CHANNEL_OTC)
          dma_set_request(DMA_CHANNEL_OTC, state->channel_control.start_trigger != 0);

        if (dma_can_transfer_channel((dma_channel_t)channel_index, ignore_halt))
        {
          if ((dma_channel_t)channel_index != DMA_CHANNEL_OTC &&
               state->channel_control.sync_mode == SYNC_MODE_MANUAL &&
              state->channel_control.chopping_enable) 
          {
            /* Estimate roughly how many CPU cycles the transfer will take
             * and delay it.  Required for Lagnacure Legend, which sets
             * DICR after CHCR to kick off the transfer.  Capped at 500
             * cycles; larger caps break Namco Museum Vol. 4.  Tiny
             * blocks bypass entirely (Dotchi Mecha! sets up a 3-word
             * sector header and expects it immediately). */
            const u32 cpu_cycles_per_block = (1u << state->channel_control.chopping_cpu_window_size);
            const u32 blocks =
              state->block_control.manual.word_count >> state->channel_control.chopping_dma_window_size;
            tick_count_t delay_cycles = (tick_count_t)(cpu_cycles_per_block * blocks);
            if (delay_cycles > 500) delay_cycles = 500;
            if (state->block_control.manual.word_count > 4 && delay_cycles > 1)
            {
              DEV_LOG("Delaying channel %u transfer by %d cycles due to chopping",
                      channel_index, delay_cycles);
              dma_halt_transfer(delay_cycles);
            }
            else
            {
              dma_transfer_channel((dma_channel_t)channel_index);
            }
          }
          else
          {
            dma_transfer_channel((dma_channel_t)channel_index);
          }
        }
        return;
      }

      default:
        break;
    }
  }
  else
  {
    switch (offset)
    {
      case 0x70:
        TRACE_LOG("DPCR <- 0x%08X", value);
        s_state.DPCR.bits = value;
        for (u32 i = 0; i < DMA_NUM_CHANNELS; i++)
        {
          if (dma_can_transfer_channel((dma_channel_t)i, false))
          {
            if (!dma_transfer_channel((dma_channel_t)i))
              break;
          }
        }
        return;

      case 0x74:
        TRACE_LOG("DICR <- 0x%08X", value);
        s_state.DICR.bits = (s_state.DICR.bits & ~DICR_WRITE_MASK) | (value & DICR_WRITE_MASK);
        s_state.DICR.bits = s_state.DICR.bits & ~(value & DICR_RESET_MASK);
        dma_update_irq();
        return;

      default: break;
    }
  }

  ERROR_LOG("Unhandled register write: %02X <- %08X", offset, value);
}

void dma_set_request(dma_channel_t channel, bool request)
{
  dma_channel_state_t* cs = &s_state.channels[(u32)channel];
  if (cs->request == request)
    return;

  cs->request = request;
  if (dma_can_transfer_channel(channel, false))
    dma_transfer_channel(channel);
}

const u32* dma_dbg_transfer_counts(void)
{
  return s_dbg_transfer_counts;
}

u32 dma_dbg_base_address(u32 channel)
{
  return (channel < DMA_NUM_CHANNELS) ? s_state.channels[channel].base_address : 0;
}

u32 dma_dbg_block_control(u32 channel)
{
  return (channel < DMA_NUM_CHANNELS) ? s_state.channels[channel].block_control.bits : 0;
}

u32 dma_dbg_channel_control(u32 channel)
{
  return (channel < DMA_NUM_CHANNELS) ? s_state.channels[channel].channel_control.bits : 0;
}

u32 dma_dbg_request(u32 channel)
{
  return (channel < DMA_NUM_CHANNELS && s_state.channels[channel].request) ? 1u : 0u;
}

u32 dma_dbg_dicr(void)
{
  return s_state.DICR.bits;
}

static bool dma_can_transfer_channel(dma_channel_t channel, bool ignore_halt)
{
  if (!dpcr_get_master_enable(s_state.DPCR, channel))
    return false;

  const dma_channel_state_t* cs = &s_state.channels[(u32)channel];
  if (!cs->channel_control.enable_busy)
    return false;

  if (cs->channel_control.sync_mode != SYNC_MODE_MANUAL &&
      (dma_is_transfer_halted() && !ignore_halt))
    return false;

  return cs->request;
}

static bool dma_is_transfer_halted(void)
{
  return timing_event_is_active(&s_state.unhalt_event);
}

static void dma_update_irq(void)
{
  const dicr_register_t old_dicr = s_state.DICR;
  dicr_update_master_flag(&s_state.DICR);
  if (!old_dicr.master_flag && s_state.DICR.master_flag)
    TRACE_LOG("Firing DMA master interrupt");
  interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_DMA, s_state.DICR.master_flag != 0);
}

static bool dma_is_linked_list_terminator(u32 address)
{
  /* Terminator and MADR are both 24 bits wide, so the actual sentinel that
   * appears in the next-pointer field is 0xFFFFFF. */
  return ((address & LINKED_LIST_TERMINATOR) == LINKED_LIST_TERMINATOR);
}

static bool dma_check_for_bus_error(dma_channel_t channel, dma_channel_state_t* cs, u32 address, u32 size)
{
  /* Relying on a transfer partially happening at the end of RAM, then
   * hitting a bus error, would be silly. */
  if ((address + size) >= bus_get_ram_mapped_size())
  {
    DEBUG_LOG("DMA bus error on channel %u at address 0x%08X size %u", (u32)channel, address, size);
    cs->channel_control.enable_busy = 0;
    s_state.DICR.bus_error = 1;
    dicr_set_irq_flag(&s_state.DICR, channel);
    dma_update_irq();
    return true;
  }
  return false;
}

static void dma_complete_transfer(dma_channel_t channel, dma_channel_state_t* cs)
{
  /* start/busy bit is cleared on end of transfer */
  s_dbg_transfer_counts[(u32)channel]++;
  DEBUG_LOG("DMA transfer for channel %u complete", (u32)channel);
  cs->channel_control.enable_busy = 0;
  if (dicr_should_set_irq_flag(s_state.DICR, channel))
  {
    DEBUG_LOG("Setting DMA interrupt for channel %u", (u32)channel);
    dicr_set_irq_flag(&s_state.DICR, channel);
    dma_update_irq();
  }
}

static tick_count_t dma_get_max_slice_ticks(dma_channel_t channel, tick_count_t max_slice_size)
{
  tick_count_t max = pad_is_transmitting() ? SLICE_SIZE_WHEN_TRANSMITTING_PAD : max_slice_size;

  /* Cap MDEC slices: our internal FIFO is larger than the real one, so
   * left unbounded a single MDEC slice can queue kilobytes of data,
   * stealing CPU cycles and delaying interrupts. */
  if ((channel == DMA_CHANNEL_MDECIN || channel == DMA_CHANNEL_MDECOUT) &&
      mdec_is_decoding_macroblock())
  {
    max = SLICE_SIZE_WHEN_DECODING_MDEC;
  }

  if (!timing_events_is_running_events())
    return max;

  const tick_count_t remaining_in_event_loop =
    (tick_count_t)(timing_events_get_event_run_tick_counter() - timing_events_get_global_tick_counter());
  const tick_count_t res = max - remaining_in_event_loop;
  return (res < 1) ? 1 : res;
}

static bool dma_transfer_channel(dma_channel_t channel)
{
  dma_channel_state_t* cs = &s_state.channels[(u32)channel];

  const bool copy_to_device = cs->channel_control.copy_to_device;
  cs->channel_control.start_trigger = 0;

  u32 current_address = cs->base_address;
  const u32 increment = cs->channel_control.address_step_reverse ? (u32)(-4) : 4u;

  switch ((dma_sync_mode_t)cs->channel_control.sync_mode)
  {
    case SYNC_MODE_MANUAL:
    {
      const u32 word_count = cs->block_control.manual.word_count == 0 ? 0x10000u
                                                                       : cs->block_control.manual.word_count;
      DEBUG_LOG("DMA[%u]: Copying %u words %s 0x%08X", (u32)channel, word_count,
                copy_to_device ? "from" : "to", current_address);

      const u32 transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
      if (dma_check_for_bus_error(channel, cs, transfer_addr, (word_count - 1) * increment))
        return true;

      tick_count_t used_ticks = copy_to_device
        ? dma_transfer_memory_to_device(channel, transfer_addr, increment, word_count) 
        : dma_transfer_device_to_memory(channel, transfer_addr, increment, word_count);

      cpu_add_pending_ticks(used_ticks);
      dma_complete_transfer(channel, cs);
      return true;
    }

    case SYNC_MODE_LINKED_LIST:
    {
      if (!copy_to_device)
      {
        Panic("Linked list not implemented for DMA reads");
        return true;
      }

      DEBUG_LOG("DMA[%u]: Copying linked list starting at 0x%08X to device", (u32)channel, current_address);

      const u8* const ram_ptr = bus_get_ram_pointer();
      const u32 mask = bus_get_ram_mask();

      const tick_count_t slice_ticks = dma_get_max_slice_ticks(channel, dma_settings_max_slice_ticks());
      tick_count_t remaining_ticks = slice_ticks;
      while (cs->request && remaining_ticks > 0)
      {
        u32 header;
        const u32 transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
        if (dma_check_for_bus_error(channel, cs, transfer_addr, sizeof(header)))
        {
          cs->base_address = current_address;
          return true;
        }

        memcpy(&header, &ram_ptr[transfer_addr & mask], sizeof(header));
        const u32 word_count = header >> 24;
        const u32 next_address = header & 0x00FFFFFFu;
        TRACE_LOG(" .. linked list entry at 0x%08X size=%u(%u words) next=0x%08X",
                  current_address, word_count * 4u, word_count, next_address);

        const tick_count_t setup_ticks = (word_count > 0)
          ? (LINKED_LIST_HEADER_READ_TICKS + LINKED_LIST_BLOCK_SETUP_TICKS)
          :  LINKED_LIST_HEADER_READ_TICKS;
        cpu_add_pending_ticks(setup_ticks);
        remaining_ticks -= setup_ticks;

        if (word_count > 0)
        {
          if (dma_check_for_bus_error(channel, cs, transfer_addr, (word_count - 1) * increment))
          {
            cs->base_address = current_address;
            return true;
          }

          const tick_count_t block_ticks =
            dma_transfer_memory_to_device(channel, transfer_addr + sizeof(header), 4u, word_count);
          cpu_add_pending_ticks(block_ticks);
          remaining_ticks -= block_ticks;
        }

        current_address = next_address;
        if (dma_is_linked_list_terminator(current_address))
        {
          /* Terminator IS the address (24 bits, all set). */
          cs->base_address = LINKED_LIST_TERMINATOR;
          dma_complete_transfer(channel, cs);
          return true;
        }
      }

      cs->base_address = current_address;
      if (cs->request)
      {
        /* Stall the transfer if we ran for too long this slice. */
        dma_halt_transfer(dma_settings_halt_ticks());
        return false;
      }
      /* linked list not yet complete */
      return true;
    }

    case SYNC_MODE_REQUEST:
    {
      const u32 block_size_raw = cs->block_control.request.block_size;
      const u32 block_size = block_size_raw == 0 ? 0x10000u : block_size_raw;
      u32 blocks_remaining = cs->block_control.request.block_count == 0 ? 0x10000u
                                                                         : cs->block_control.request.block_count;
      tick_count_t ticks_remaining = dma_get_max_slice_ticks(channel, dma_settings_max_slice_ticks());

      DEBUG_LOG("DMA[%u]: Copying %u blocks of size %u (%u total words) %s 0x%08X",
                (u32)channel, blocks_remaining, block_size, blocks_remaining * block_size,
                copy_to_device ? "from" : "to", current_address);

      if (copy_to_device)
      {
        do {
          const u32 transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
          if (dma_check_for_bus_error(channel, cs, transfer_addr, (block_size - 1) * increment))
          {
            cs->base_address = current_address;
            cs->block_control.request.block_count = blocks_remaining;
            return true;
          }

          const tick_count_t ticks =
            dma_transfer_memory_to_device(channel, transfer_addr, increment, block_size);
          cpu_add_pending_ticks(ticks);
          ticks_remaining -= ticks;
          blocks_remaining--;
          current_address = (transfer_addr + (increment * block_size));
        } while (cs->request && blocks_remaining > 0 && ticks_remaining > 0);
      }
      else
      {
        do {
          const u32 transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
          if (dma_check_for_bus_error(channel, cs, transfer_addr, (block_size - 1) * increment))
          {
            cs->base_address = current_address;
            cs->block_control.request.block_count = blocks_remaining;
            return true;
          }

          const tick_count_t ticks =
            dma_transfer_device_to_memory(channel, transfer_addr, increment, block_size);
          cpu_add_pending_ticks(ticks);
          ticks_remaining -= ticks;
          blocks_remaining--;
          current_address = (transfer_addr + (increment * block_size));
        } while (cs->request && blocks_remaining > 0 && ticks_remaining > 0);
      }

      cs->base_address = current_address;
      cs->block_control.request.block_count = blocks_remaining;

      if (blocks_remaining > 0)
      {
        if (cs->request)
        {
          /* Halted mid-block. */
          if (!timing_event_is_active(&s_state.unhalt_event))
            dma_halt_transfer(dma_settings_halt_ticks());
          return false;
        }
        return true;
      }

      dma_complete_transfer(channel, cs);
      return true;
    }

    case SYNC_MODE_RESERVED:
    default:
      Panic("Unimplemented sync mode");
  }

  UnreachableCode();
}

static void dma_halt_transfer(tick_count_t duration)
{
  s_state.halt_ticks_remaining += duration;
  DEBUG_LOG("Halting DMA for %d ticks", s_state.halt_ticks_remaining);
  if (timing_event_is_active(&s_state.unhalt_event))
    return;

  timing_event_set_interval_and_schedule(&s_state.unhalt_event, s_state.halt_ticks_remaining);
}

static void dma_unhalt_transfer(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  DEBUG_LOG("Resuming DMA after %d ticks, %d ticks late",
            ticks, -(s_state.halt_ticks_remaining - ticks));
  s_state.halt_ticks_remaining -= ticks;
  timing_event_deactivate(&s_state.unhalt_event);

  /* Linear-priority sweep.  Mostly fine; OTC must run after GPU or it
   * wipes out the linked list it's about to consume. */
  for (u32 i = 0; i < DMA_NUM_CHANNELS; i++)
  {
    if (dma_can_transfer_channel((dma_channel_t)i, false))
    {
      if (!dma_transfer_channel((dma_channel_t)i))
        return;
    }
  }

  s_state.halt_ticks_remaining = 0;
}

static void ensure_transfer_buffer(u32 word_count)
{
  if (s_state.transfer_buffer_cap >= word_count)
    return;
  s_state.transfer_buffer = (u32*)realloc(s_state.transfer_buffer, word_count * sizeof(u32));
  if (!s_state.transfer_buffer)
    Panic("Out of memory growing DMA transfer buffer");
  s_state.transfer_buffer_cap = word_count;
}

static tick_count_t dma_transfer_memory_to_device(dma_channel_t channel, u32 address, u32 increment, u32 word_count)
{
  const u32 mask = bus_get_ram_mask();
  address &= mask;

  const u32* src_pointer = (u32*)(bus_get_ram_pointer() + address);
  if (channel != DMA_CHANNEL_GPU)
  {
    if ((s32)increment < 0 || ((address + (increment * word_count)) & mask) <= address)
    {
      /* Wraparound -> stage to a contiguous buffer first. */
      ensure_transfer_buffer(word_count);
      src_pointer = s_state.transfer_buffer;

      u8* ram_pointer = bus_get_ram_pointer();
      u32 a = address;
      for (u32 i = 0; i < word_count; i++)
      {
        memcpy(&s_state.transfer_buffer[i], &ram_pointer[a], sizeof(u32));
        a = (a + increment) & mask;
      }
    }
  }

  switch (channel)
  {
    case DMA_CHANNEL_GPU:
    {
      if (gpu_begin_dma_write && gpu_begin_dma_write())
      {
        u8* ram_pointer = bus_get_ram_pointer();
        u32 a = address;
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value;
          memcpy(&value, &ram_pointer[a], sizeof(u32));
          if (gpu_dma_write) gpu_dma_write(a, value);
          a = (a + increment) & mask;
        }
        if (gpu_end_dma_write) gpu_end_dma_write();
      }
      break;
    }

    case DMA_CHANNEL_SPU:
      spu_dma_write(src_pointer, word_count);
      break;

    case DMA_CHANNEL_MDECIN:
      mdec_dma_write(src_pointer, word_count);
      break;

    case DMA_CHANNEL_CDROM:
    case DMA_CHANNEL_MDECOUT:
    case DMA_CHANNEL_PIO:
    default:
      ERROR_LOG("Unhandled DMA channel %u for device write", (u32)channel);
      break;
  }

  return bus_get_dma_ram_tick_count(word_count);
}

static tick_count_t dma_transfer_device_to_memory(dma_channel_t channel, u32 address, u32 increment, u32 word_count)
{
  const u32 mask = bus_get_ram_mask();
  /* TODO-style note: may not be quite right for OTC. */
  address &= mask;

  if (channel == DMA_CHANNEL_OTC)
  {
    /* Build the ordering table: each word points to the previous, except
     * the last which holds the terminator. */
    u8* ram_pointer = bus_get_ram_pointer();
    const u32 word_count_less_1 = word_count - 1u;
    u32 a = address;
    for (u32 i = 0; i < word_count_less_1; i++)
    {
      u32 next = ((a - 4u) & mask);
      memcpy(&ram_pointer[a], &next, sizeof(next));
      a = next;
    }
    const u32 terminator = UINT32_C(0xFFFFFF);
    memcpy(&ram_pointer[a], &terminator, sizeof(terminator));
    return bus_get_dma_ram_tick_count(word_count);
  }

  u32* dest_pointer = (u32*)(&bus_get_ram_pointer()[address]);
  bool used_buffer = false;
  if ((s32)increment < 0 || ((address + (increment * word_count)) & mask) <= address)
  {
    /* wraparound -> stage */
    ensure_transfer_buffer(word_count);
    dest_pointer = s_state.transfer_buffer;
    used_buffer = true;
  }
  else if (channel == DMA_CHANNEL_CDROM)
  {
    const u32 end_page = (address + (increment * word_count) - 1u) >> HOST_PAGE_SHIFT;
    for (u32 page = address >> HOST_PAGE_SHIFT; page <= end_page; page++)
    {
      if (bus_is_ram_code_page(page) && cpu_code_cache_invalidate_blocks_with_page_index)
      {
        cpu_code_cache_invalidate_blocks_with_page_index(page);
      }
    }
  }

  switch (channel)
  {
    case DMA_CHANNEL_GPU:
      if (gpu_dma_read) gpu_dma_read(dest_pointer, word_count);
      break;

    case DMA_CHANNEL_CDROM:
      cdrom_dma_read(dest_pointer, word_count);
      break;

    case DMA_CHANNEL_SPU:
      spu_dma_read(dest_pointer, word_count);
      break;

    case DMA_CHANNEL_MDECOUT:
      mdec_dma_read(dest_pointer, word_count);
      break;

    default:
      ERROR_LOG("Unhandled DMA channel %u for device read", (u32)channel);
      for (u32 i = 0; i < word_count; i++)
        dest_pointer[i] = UINT32_C(0xFFFFFFFF);
      break;
  }

  if (used_buffer)
  {
    u8* ram_pointer = bus_get_ram_pointer();
    u32 a = address;
    for (u32 i = 0; i < word_count; i++)
    {
      memcpy(&ram_pointer[a], &s_state.transfer_buffer[i], sizeof(u32));
      a = (a + increment) & mask;
    }
  }

  tick_count_t ticks = bus_get_dma_ram_tick_count(word_count);
  if (channel == DMA_CHANNEL_CDROM)
  {
    const int speedup = settings_get_cdrom_read_speedup ? settings_get_cdrom_read_speedup()
                                                        : default_cdrom_read_speedup();
    if (speedup != 1)
      ticks = (speedup == 0) ? 0 : (ticks / speedup);
  }
  return ticks;
}
