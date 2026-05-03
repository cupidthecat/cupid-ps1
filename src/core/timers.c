/*
 * Three root counters: T0/T1 driven by GPU dotclock or hblank, T2 driven by
 * sysclk or sysclk/8.  Each can fire an IRQ at target or overflow.
 *
 * External (parallel-agent) dependencies:
 *   gpu_synchronize_crtc()          ; gpu agent
 *   gpu_is_crtc_scanline_pending()  ; gpu agent
 *   system_get_max_slice_ticks()    ; system agent
 *   system_scale_ticks_to_overclock(t)
 *   system_unscale_ticks_to_overclock(t, carry*)
 */

#include "core/timers.h"
#include "core/interrupt_controller.h"
#include "core/timing_event.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"

#include <limits.h>
#include <stdlib.h>

LOG_CHANNEL(Timers);

extern void         gpu_synchronize_crtc(void)         __attribute__((weak));
extern bool         gpu_is_crtc_scanline_pending(void) __attribute__((weak));
extern tick_count_t system_get_max_slice_ticks(void)   __attribute__((weak));
extern tick_count_t system_scale_ticks_to_overclock(tick_count_t t) __attribute__((weak));
extern tick_count_t system_unscale_ticks_to_overclock(tick_count_t t, tick_count_t* carry)
  __attribute__((weak));

/* Default scaling/slice values when the system module isn't yet linked. */
static tick_count_t default_max_slice_ticks(void) { return 1000; }
static tick_count_t default_scale(tick_count_t t)   { return t; }
static tick_count_t default_unscale(tick_count_t t, tick_count_t* c) { (void)c; return t; }

#define NUM_TIMERS 3u

typedef enum {
  SYNC_MODE_PAUSE_WHILE_GATE_ACTIVE = 0,
  SYNC_MODE_RESET_ON_GATE_END       = 1,
  SYNC_MODE_RESET_AND_RUN_ON_START  = 2,
  SYNC_MODE_FREE_RUN_ON_GATE_END    = 3,
} timers_sync_mode_t;

typedef union {
  u32 bits;
  struct {
    u32 sync_enable        : 1;  /* bit 0 */
    u32 sync_mode          : 2;  /* bit 1-2 */
    u32 reset_at_target    : 1;  /* bit 3 */
    u32 irq_at_target      : 1;  /* bit 4 */
    u32 irq_on_overflow    : 1;  /* bit 5 */
    u32 irq_repeat         : 1;  /* bit 6 */
    u32 irq_pulse_n        : 1;  /* bit 7 */
    u32 clock_source       : 2;  /* bit 8-9 */
    u32 interrupt_request_n: 1;  /* bit 10 */
    u32 reached_target     : 1;  /* bit 11 */
    u32 reached_overflow   : 1;  /* bit 12 */
    u32 _pad               : 19;
  };
} timer_counter_mode_t;

typedef struct {
  timer_counter_mode_t mode;
  u32  counter;
  u32  target;
  bool gate;
  bool use_external_clock;
  bool external_counting_enabled;
  bool counting_enabled;
  bool irq_done;
} timer_counter_state_t;

typedef struct {
  timing_event_t        sysclk_event;
  timer_counter_state_t counters[NUM_TIMERS];
  tick_count_t          sysclk_ticks_carry; /* 0 unless overclocking is enabled */
  u32                   sysclk_div_8_carry; /* partial ticks for timer 2 with sysclk/8 */
} timers_state_t;

static const char s_sysclk_event_name[] = "Timer SysClk Interrupt";

ALIGN_TO_CACHE_LINE static timers_state_t s_state;

static void         timers_update_counting_enabled(timer_counter_state_t* cs);
static void         timers_check_for_irq          (u32 index, u32 old_counter);
static void         timers_add_sysclk_ticks       (void* user, tick_count_t sysclk_ticks, tick_count_t ticks_late);
static tick_count_t timers_get_ticks_until_next_interrupt(void);
static void         timers_update_sysclk_event    (void);

void timers_initialize(void)
{
  timing_event_init(&s_state.sysclk_event,
                    s_sysclk_event_name, (u32)(sizeof(s_sysclk_event_name) - 1),
                    1, 1, &timers_add_sysclk_ticks, NULL);
  timers_reset();
}

void timers_shutdown(void)
{
  timing_event_deactivate(&s_state.sysclk_event);
  timing_event_destroy(&s_state.sysclk_event);
}

void timers_reset(void)
{
  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    timer_counter_state_t* cs = &s_state.counters[i];
    cs->mode.bits = 0;
    cs->mode.interrupt_request_n = 1;
    cs->counter = 0;
    cs->target = 0;
    cs->gate = false;
    cs->external_counting_enabled = false;
    cs->counting_enabled = true;
    cs->irq_done = false;
  }

  timing_event_deactivate(&s_state.sysclk_event);
  s_state.sysclk_ticks_carry = 0;
  s_state.sysclk_div_8_carry = 0;
  timers_update_sysclk_event();
}

bool timers_do_state(state_wrapper_t* sw)
{
  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    timer_counter_state_t* cs = &s_state.counters[i];
    state_wrapper_do_u32 (sw, &cs->mode.bits);
    state_wrapper_do_u32 (sw, &cs->counter);
    state_wrapper_do_u32 (sw, &cs->target);
    state_wrapper_do_bool(sw, &cs->gate);
    state_wrapper_do_bool(sw, &cs->use_external_clock);
    state_wrapper_do_bool(sw, &cs->external_counting_enabled);
    state_wrapper_do_bool(sw, &cs->counting_enabled);
    state_wrapper_do_bool(sw, &cs->irq_done);
  }

  state_wrapper_do_s32(sw, &s_state.sysclk_ticks_carry);
  state_wrapper_do_u32(sw, &s_state.sysclk_div_8_carry);

  if (state_wrapper_is_reading(sw))
    timers_update_sysclk_event();

  return !state_wrapper_has_error(sw);
}

void timers_cpu_clocks_changed(void)
{
  s_state.sysclk_ticks_carry = 0;
}

bool timers_is_using_external_clock(u32 timer)
{
  return s_state.counters[timer].external_counting_enabled;
}

bool timers_is_sync_enabled(u32 timer)
{
  return s_state.counters[timer].mode.sync_enable != 0;
}

bool timers_is_external_irq_enabled(u32 timer)
{
  const timer_counter_state_t* cs = &s_state.counters[timer];
  return (cs->external_counting_enabled && (cs->mode.bits & ((1u << 4) | (1u << 5))) != 0);
}

void timers_set_gate(u32 timer, bool state)
{
  timer_counter_state_t* cs = &s_state.counters[timer];
  if (cs->gate == state)
    return;

  cs->gate = state;

  if (!cs->mode.sync_enable)
    return;

  /* Gate prevents counting in or outside of the active window, so we need a
   * correct counter value before we change modes.  For "reset on gate end",
   * the reset can wait until the gate clears. */
  if (!cs->use_external_clock &&
      (cs->mode.sync_mode != SYNC_MODE_RESET_ON_GATE_END || !state))
  {
    timing_event_invoke_early(&s_state.sysclk_event, false);
  }

  switch ((timers_sync_mode_t)cs->mode.sync_mode)
  {
    case SYNC_MODE_PAUSE_WHILE_GATE_ACTIVE: break;

    case SYNC_MODE_RESET_ON_GATE_END:
      cs->counter = state ? cs->counter : 0;
      break;

    case SYNC_MODE_RESET_AND_RUN_ON_START:
      /* PS2 hardwires the counter to 0 outside the gate. */
      cs->counter = state ? 0 : cs->counter;
      break;

    case SYNC_MODE_FREE_RUN_ON_GATE_END:
      cs->mode.sync_enable &= state ? 1u : 0u;
      break;

    default:
      UnreachableCode();
  }

  timers_update_counting_enabled(cs);
  timers_update_sysclk_event();
}

tick_count_t timers_get_ticks_until_irq(u32 timer)
{
  const timer_counter_state_t* cs = &s_state.counters[timer];
  if (!cs->counting_enabled)
    return INT32_MAX;

  tick_count_t ticks_until_irq = INT32_MAX;
  if (cs->mode.irq_at_target && cs->counter < cs->target)
    ticks_until_irq = (tick_count_t)(cs->target - cs->counter);
  if (cs->mode.irq_on_overflow)
  {
    const tick_count_t overflow = (tick_count_t)(0xFFFFu - cs->counter);
    if (overflow < ticks_until_irq) ticks_until_irq = overflow;
  }
  return ticks_until_irq;
}

void timers_add_ticks(u32 timer, tick_count_t count)
{
  timer_counter_state_t* cs = &s_state.counters[timer];
  const u32 old_counter = cs->counter;
  cs->counter += (u32)count;
  timers_check_for_irq(timer, old_counter);
}

u32 timers_dbg_counter(u32 timer)
{
  return (timer < NUM_TIMERS) ? s_state.counters[timer].counter : 0;
}

u32 timers_dbg_mode(u32 timer)
{
  return (timer < NUM_TIMERS) ? s_state.counters[timer].mode.bits : 0;
}

u32 timers_dbg_target(u32 timer)
{
  return (timer < NUM_TIMERS) ? s_state.counters[timer].target : 0;
}

static void timers_check_for_irq(u32 timer, u32 old_counter)
{
  timer_counter_state_t* cs = &s_state.counters[timer];

  bool interrupt_request = false;
  if (cs->counter >= cs->target && (old_counter < cs->target || cs->target == 0))
  {
    interrupt_request |= cs->mode.irq_at_target ? true : false;
    cs->mode.reached_target = 1;

    if (cs->mode.reset_at_target && cs->target > 0)
      cs->counter %= cs->target;
  }
  if (cs->counter >= 0xFFFFu)
  {
    interrupt_request |= cs->mode.irq_on_overflow ? true : false;
    cs->mode.reached_overflow = 1;
    cs->counter %= 0xFFFFu;
  }

  if (interrupt_request)
  {
    const interrupt_controller_irq_t irqnum =
      (interrupt_controller_irq_t)((u32)INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
    if (!cs->mode.irq_pulse_n)
    {
      if (!cs->irq_done || cs->mode.irq_repeat)
      {
        /* Real hardware drives the line low for a few cycles before going
         * back high; we approximate with an immediate edge pair. */
        DEBUG_LOG("Raising timer %u pulse IRQ", timer);
        interrupt_controller_set_line_state(irqnum, false);
        interrupt_controller_set_line_state(irqnum, true);
      }
      cs->irq_done = true;
      cs->mode.interrupt_request_n = 1;
    }
    else
    {
      /* Toggle mode.  Non-repeat behaviour here is approximated. */
      cs->mode.interrupt_request_n ^= 1u;
      if (!cs->mode.interrupt_request_n)
        DEBUG_LOG("Raising timer %u alternate IRQ", timer);

      interrupt_controller_set_line_state(irqnum, !cs->mode.interrupt_request_n);
    }
  }
}

static void timers_add_sysclk_ticks(void* user, tick_count_t sysclk_ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  if (system_unscale_ticks_to_overclock)
    sysclk_ticks = system_unscale_ticks_to_overclock(sysclk_ticks, &s_state.sysclk_ticks_carry);
  else
    sysclk_ticks = default_unscale(sysclk_ticks, &s_state.sysclk_ticks_carry);

  if (!s_state.counters[0].external_counting_enabled && s_state.counters[0].counting_enabled)
    timers_add_ticks(0, sysclk_ticks);
  if (!s_state.counters[1].external_counting_enabled && s_state.counters[1].counting_enabled)
    timers_add_ticks(1, sysclk_ticks);
  if (s_state.counters[2].external_counting_enabled)
  {
    tick_count_t sysclk_div_8_ticks =
      (sysclk_ticks + (tick_count_t)s_state.sysclk_div_8_carry) / 8;
    s_state.sysclk_div_8_carry =
      (u32)((sysclk_ticks + (tick_count_t)s_state.sysclk_div_8_carry) % 8);
    timers_add_ticks(2, sysclk_div_8_ticks);
  }
  else if (s_state.counters[2].counting_enabled)
  {
    timers_add_ticks(2, sysclk_ticks);
  }

  timers_update_sysclk_event();
}

u32 timers_read_register(u32 offset)
{
  const u32 timer_index = (offset >> 4) & 0x03u;
  const u32 port_offset = offset & 0x0Fu;
  if (timer_index >= 3u)
  {
    ERROR_LOG("Timer read out of range: offset 0x%02X", offset);
    return UINT32_C(0xFFFFFFFF);
  }

  timer_counter_state_t* cs = &s_state.counters[timer_index];

  switch (port_offset)
  {
    case 0x00:
    {
      if (timer_index < 2 && cs->external_counting_enabled && gpu_synchronize_crtc)
      {
        /* T0/T1 depend on the GPU. */
        if (timer_index == 0 ||
            (gpu_is_crtc_scanline_pending && gpu_is_crtc_scanline_pending()))
          gpu_synchronize_crtc();
      }

      timing_event_invoke_early(&s_state.sysclk_event, false);
      return cs->counter;
    }

    case 0x04:
    {
      if (timer_index < 2 && cs->external_counting_enabled && gpu_synchronize_crtc)
      {
        if (timer_index == 0 ||
            (gpu_is_crtc_scanline_pending && gpu_is_crtc_scanline_pending()))
          gpu_synchronize_crtc();
      }

      timing_event_invoke_early(&s_state.sysclk_event, false);

      const u32 bits = cs->mode.bits;
      cs->mode.reached_overflow = 0;
      cs->mode.reached_target   = 0;
      return bits;
    }

    case 0x08:
      return cs->target;

    default:
      ERROR_LOG("Read unknown register in timer %u (offset 0x%02X)", timer_index, offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void timers_write_register(u32 offset, u32 value)
{
  const u32 timer_index = (offset >> 4) & 0x03u;
  const u32 port_offset = offset & 0x0Fu;
  if (timer_index >= 3u)
  {
    ERROR_LOG("Timer write out of range: offset 0%02X value 0x%08X", offset, value);
    return;
  }

  timer_counter_state_t* cs = &s_state.counters[timer_index];

  if (timer_index < 2 && cs->external_counting_enabled && gpu_synchronize_crtc)
  {
    if (timer_index == 0 ||
        (gpu_is_crtc_scanline_pending && gpu_is_crtc_scanline_pending()))
      gpu_synchronize_crtc();
  }

  timing_event_invoke_early(&s_state.sysclk_event, false);

  /* Strictly speaking these IRQ checks should probably happen on the next tick. */
  switch (port_offset)
  {
    case 0x00:
    {
      const u32 old_counter = cs->counter;
      DEBUG_LOG("Timer %u write counter %u", timer_index, value);
      cs->counter = value & 0xFFFFu;
      timers_check_for_irq(timer_index, old_counter);
      if (timer_index == 2 || !cs->external_counting_enabled)
        timers_update_sysclk_event();
      break;
    }

    case 0x04:
    {
      static const u32 WRITE_MASK = 0xE3FFu; /* 0b1110_0011_1111_1111 */

      DEBUG_LOG("Timer %u write mode register 0x%04X", timer_index, value);
      cs->mode.bits = (value & WRITE_MASK) | (cs->mode.bits & ~WRITE_MASK);

      /* Use a local for clock_source.  See the long-form comment in
       * GCC, but kept structurally identical for clarity. */
      const u8 clock_source = (u8)cs->mode.clock_source;
      cs->use_external_clock = (clock_source & (timer_index == 2 ? 2u : 1u)) != 0;

      cs->counter = 0;
      cs->irq_done = false;
      interrupt_controller_set_line_state(
        (interrupt_controller_irq_t)((u32)INTERRUPT_CONTROLLER_IRQ_TMR0 + timer_index), false);

      timers_update_counting_enabled(cs);
      timers_check_for_irq(timer_index, cs->counter);
      timers_update_sysclk_event();
      break;
    }

    case 0x08:
    {
      DEBUG_LOG("Timer %u write target 0x%04X", timer_index, value & 0xFFFFu);
      cs->target = value & 0xFFFFu;
      timers_check_for_irq(timer_index, cs->counter);
      if (timer_index == 2 || !cs->external_counting_enabled)
        timers_update_sysclk_event();
      break;
    }

    default:
      ERROR_LOG("Write unknown register in timer %u (offset 0x%02X, value 0x%X)",
                timer_index, offset, value);
      break;
  }
}

static void timers_update_counting_enabled(timer_counter_state_t* cs)
{
  if (cs->mode.sync_enable)
  {
    switch ((timers_sync_mode_t)cs->mode.sync_mode)
    {
      case SYNC_MODE_PAUSE_WHILE_GATE_ACTIVE:
        cs->counting_enabled = !cs->gate;
        break;

      case SYNC_MODE_RESET_ON_GATE_END:
        cs->counting_enabled = true;
        break;

      case SYNC_MODE_RESET_AND_RUN_ON_START:
      case SYNC_MODE_FREE_RUN_ON_GATE_END:
        cs->counting_enabled = cs->gate;
        break;

      default:
        UnreachableCode();
    }
  }
  else
  {
    cs->counting_enabled = true;
  }

  cs->external_counting_enabled = cs->use_external_clock && cs->counting_enabled;
}

static tick_count_t timers_get_ticks_until_next_interrupt(void)
{
  tick_count_t min_ticks = system_get_max_slice_ticks ? system_get_max_slice_ticks()
                                                      : default_max_slice_ticks();
  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    const timer_counter_state_t* cs = &s_state.counters[i];
    if (!cs->counting_enabled || (i < 2 && cs->external_counting_enabled) ||
        (!cs->mode.irq_at_target && !cs->mode.irq_on_overflow &&
         (cs->mode.irq_repeat || !cs->irq_done))) 
    {
      continue;
    }

    if (cs->mode.irq_at_target)
    {
      tick_count_t ticks = (cs->counter <= cs->target)
                             ? (tick_count_t)(cs->target - cs->counter) 
                             : (tick_count_t)((0xFFFFu - cs->counter) + cs->target);
      if (cs->external_counting_enabled) /* sysclk/8 for timer 2 */
        ticks *= 8;

      if (ticks < min_ticks) min_ticks = ticks;
    }
    if (cs->mode.irq_on_overflow)
    {
      tick_count_t ticks = (tick_count_t)(0xFFFFu - cs->counter);
      if (cs->external_counting_enabled)
        ticks *= 8;
      if (ticks < min_ticks) min_ticks = ticks;
    }
  }

  if (min_ticks < 1) min_ticks = 1;
  return system_scale_ticks_to_overclock ? system_scale_ticks_to_overclock(min_ticks)
                                         : default_scale(min_ticks);
}

static void timers_update_sysclk_event(void)
{
  timing_event_schedule(&s_state.sysclk_event, timers_get_ticks_until_next_interrupt());
}
