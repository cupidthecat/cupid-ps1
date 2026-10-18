/*
 * Three root counters with target and overflow interrupts.
 * T0 selects the system clock or GPU dotclock; T1 selects the system clock or
 * hblank; T2 selects the system clock or its divide-by-eight output.
 * GPU synchronization and system tick scaling coordinate counter updates.
 */

#include "core/timers/timers.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/scheduler/timing_event.h"

#include "util/state/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"

#include <limits.h>
#include <stdlib.h>

LOG_CHANNEL(Timers);

extern void         gpu_reschedule_crtc(void) __attribute__((weak));
extern void         gpu_synchronize_crtc(void) __attribute__((weak));
extern bool         gpu_is_crtc_scanline_pending(void) __attribute__((weak));
extern tick_count_t system_get_max_slice_ticks(void) __attribute__((weak));
extern tick_count_t system_scale_ticks_to_overclock(tick_count_t t) __attribute__((weak));
extern tick_count_t system_unscale_ticks_to_overclock(tick_count_t t, tick_count_t* carry) __attribute__((weak));

/* Default slice and identity tick scaling when optional system hooks are absent. */
static tick_count_t default_max_slice_ticks(void)
{
  return 1000;
}
static tick_count_t default_scale(tick_count_t t)
{
  return t;
}
static tick_count_t default_unscale(tick_count_t t, tick_count_t* c)
{
  (void)c;
  return t;
}

#define NUM_TIMERS   3u
#define TIMER_PERIOD 0x10000u

typedef enum {
  SYNC_MODE_PAUSE_WHILE_GATE_ACTIVE = 0,
  SYNC_MODE_RESET_ON_GATE_END       = 1,
  SYNC_MODE_RESET_AND_RUN_ON_START  = 2,
  SYNC_MODE_FREE_RUN_ON_GATE_END    = 3,
} timers_sync_mode_t;

typedef union {
  u32 bits;
  struct {
    u32 sync_enable : 1;         /* bit 0 */
    u32 sync_mode : 2;           /* bit 1-2 */
    u32 reset_at_target : 1;     /* bit 3 */
    u32 irq_at_target : 1;       /* bit 4 */
    u32 irq_on_overflow : 1;     /* bit 5 */
    u32 irq_repeat : 1;          /* bit 6 */
    u32 irq_pulse_n : 1;         /* bit 7 */
    u32 clock_source : 2;        /* bit 8-9 */
    u32 interrupt_request_n : 1; /* bit 10 */
    u32 reached_target : 1;      /* bit 11 */
    u32 reached_overflow : 1;    /* bit 12 */
    u32 _pad : 19;
  };
} timer_counter_mode_t;

typedef struct {
  timer_counter_mode_t mode;
  u32                  counter;
  u32                  target;
  bool                 gate;
  bool                 use_external_clock;
  bool                 external_counting_enabled;
  bool                 counting_enabled;
  bool                 irq_done;
  u8                   reset_hold; /* Clocks left before counting resumes; 2 exposes the target on reads. */
} timer_counter_state_t;

typedef struct {
  timing_event_t        sysclk_event;
  timer_counter_state_t counters[NUM_TIMERS];
  tick_count_t          sysclk_ticks_carry; /* 0 unless overclocking is enabled */
  u32                   sysclk_div_8_carry; /* partial ticks for timer 2 with sysclk/8 */
} timers_state_t;

static const char s_sysclk_event_name[] = "Timer SysClk Interrupt";

ALIGN_TO_CACHE_LINE static timers_state_t s_state;

static void         timers_update_counting_enabled(u32 timer, timer_counter_state_t* cs);
static void         timers_signal_events(u32 timer, bool target, bool overflow);
static void         timers_add_sysclk_ticks(void* user, tick_count_t sysclk_ticks, tick_count_t ticks_late);
static tick_count_t timers_get_ticks_until_next_interrupt(void);
static void         timers_update_sysclk_event(void);

void timers_initialize(void)
{
  timing_event_init(&s_state.sysclk_event, s_sysclk_event_name, (u32)(sizeof(s_sysclk_event_name) - 1), 1, 1,
                    &timers_add_sysclk_ticks, NULL);
  timers_reset();
}

void timers_shutdown(void)
{
  timing_event_deactivate(&s_state.sysclk_event);
  timing_event_destroy(&s_state.sysclk_event);
}

void timers_reset(void)
{
  for (u32 i = 0; i < NUM_TIMERS; i++) {
    timer_counter_state_t* cs     = &s_state.counters[i];
    cs->mode.bits                 = 0;
    cs->mode.interrupt_request_n  = 1;
    cs->counter                   = 0;
    cs->target                    = 0;
    cs->gate                      = false;
    cs->use_external_clock        = false;
    cs->external_counting_enabled = false;
    cs->counting_enabled          = true;
    cs->irq_done                  = false;
    cs->reset_hold                = 0;
  }

  timing_event_deactivate(&s_state.sysclk_event);
  s_state.sysclk_ticks_carry = 0;
  s_state.sysclk_div_8_carry = 0;
  timers_update_sysclk_event();
}

bool timers_do_state(state_wrapper_t* sw)
{
  for (u32 i = 0; i < NUM_TIMERS; i++) {
    timer_counter_state_t* cs = &s_state.counters[i];
    state_wrapper_do_u32(sw, &cs->mode.bits);
    state_wrapper_do_u32(sw, &cs->counter);
    state_wrapper_do_u32(sw, &cs->target);
    state_wrapper_do_bool(sw, &cs->gate);
    state_wrapper_do_bool(sw, &cs->use_external_clock);
    state_wrapper_do_bool(sw, &cs->external_counting_enabled);
    state_wrapper_do_bool(sw, &cs->counting_enabled);
    state_wrapper_do_bool(sw, &cs->irq_done);
    if (state_wrapper_get_version(sw) >= 88)
      state_wrapper_do_u8(sw, &cs->reset_hold);
    else if (state_wrapper_is_reading(sw))
      cs->reset_hold = 0;
    if (state_wrapper_is_reading(sw) && (cs->reset_hold > 2 || cs->counter >= TIMER_PERIOD)) sw->error = true;
  }

  state_wrapper_do_s32(sw, &s_state.sysclk_ticks_carry);
  state_wrapper_do_u32(sw, &s_state.sysclk_div_8_carry);

  if (state_wrapper_is_reading(sw) && !state_wrapper_has_error(sw)) timers_update_sysclk_event();

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
  if (timer >= 2) return;
  timer_counter_state_t* cs = &s_state.counters[timer];
  if (cs->gate == state) return;

  cs->gate = state;

  if (!cs->mode.sync_enable) return;

  /* Gate prevents counting in or outside of the active window, so we need a
   * correct counter value before we change modes.  For "reset on gate end",
   * the reset can wait until the gate clears. */
  if (!cs->use_external_clock && (cs->mode.sync_mode != SYNC_MODE_RESET_ON_GATE_END || !state)) {
    timing_event_invoke_early(&s_state.sysclk_event, false);
  }

  switch ((timers_sync_mode_t)cs->mode.sync_mode) {
    case SYNC_MODE_PAUSE_WHILE_GATE_ACTIVE:
      break;

    case SYNC_MODE_RESET_ON_GATE_END:
      cs->counter = state ? cs->counter : 0;
      break;

    case SYNC_MODE_RESET_AND_RUN_ON_START:
      cs->counter = state ? 0 : cs->counter;
      break;

    case SYNC_MODE_FREE_RUN_ON_GATE_END:
      cs->mode.sync_enable &= state ? 1u : 0u;
      break;

    default:
      UnreachableCode();
  }

  timers_update_counting_enabled(timer, cs);
  timers_update_sysclk_event();
}

tick_count_t timers_get_ticks_until_irq(u32 timer)
{
  const timer_counter_state_t* cs = &s_state.counters[timer];
  if (!cs->counting_enabled || (!cs->mode.irq_repeat && cs->irq_done)) return INT32_MAX;
  u32 ticks = INT32_MAX;
  if (cs->mode.irq_at_target) {
    const u32 distance = cs->counter < cs->target ? cs->target - cs->counter : TIMER_PERIOD - cs->counter + cs->target;
    ticks              = distance + cs->reset_hold;
  }
  if (cs->mode.irq_on_overflow) {
    const u32 distance = TIMER_PERIOD - cs->counter + cs->reset_hold;
    if (distance < ticks) ticks = distance;
  }
  return (tick_count_t)ticks;
}

static u32 timers_visible_counter(const timer_counter_state_t* cs)
{
  return cs->reset_hold == 2 ? cs->target : cs->counter;
}

static void timers_signal_events(u32 timer, bool target, bool overflow)
{
  timer_counter_state_t* cs = &s_state.counters[timer];
  if (target) cs->mode.reached_target = 1;
  if (overflow) cs->mode.reached_overflow = 1;
  const bool requested = (target && cs->mode.irq_at_target) || (overflow && cs->mode.irq_on_overflow);
  if (!requested || (cs->irq_done && !cs->mode.irq_repeat)) return;
  const interrupt_controller_irq_t irq = (interrupt_controller_irq_t)((u32)INTERRUPT_CONTROLLER_IRQ_TMR0 + timer);
  cs->irq_done                         = true;
  if (cs->mode.irq_pulse_n) {
    cs->mode.interrupt_request_n ^= 1u;
    interrupt_controller_set_line_state(irq, !cs->mode.interrupt_request_n);
  } else {
    /* The line edges latch I_STATUS; pulse width is not modeled here. */
    interrupt_controller_set_line_state(irq, false);
    interrupt_controller_set_line_state(irq, true);
    cs->mode.interrupt_request_n = 1;
  }
}

void timers_add_ticks(u32 timer, tick_count_t count)
{
  timer_counter_state_t* cs = &s_state.counters[timer];
  if (count <= 0) return;
  u32 remaining = (u32)count;
  while (remaining) {
    if (cs->reset_hold) {
      const u32 held = remaining < cs->reset_hold ? remaining : cs->reset_hold;
      cs->reset_hold -= (u8)held;
      remaining -= held;
      if (!remaining) break;
    }
    /* Visit each comparison edge so bulk updates preserve toggle parity. */
    u32 distance = TIMER_PERIOD - cs->counter;
    if (cs->target > cs->counter && cs->target - cs->counter < distance) distance = cs->target - cs->counter;
    if (remaining < distance) {
      cs->counter += remaining;
      break;
    }
    cs->counter += distance;
    remaining -= distance;
    if (cs->counter == TIMER_PERIOD) {
      cs->counter = 0;
      timers_signal_events(timer, cs->target == 0, true);
    } else {
      timers_signal_events(timer, true, false);
      if (cs->mode.reset_at_target && cs->target > 0 && cs->target < 0xFFFFu) {
        cs->counter    = 0;
        cs->reset_hold = cs->use_external_clock ? 0 : 2;
      }
    }
  }
}

u32 timers_dbg_counter(u32 timer)
{
  return timer < NUM_TIMERS ? timers_visible_counter(&s_state.counters[timer]) : 0;
}

u32 timers_dbg_mode(u32 timer)
{
  return timer < NUM_TIMERS ? s_state.counters[timer].mode.bits : 0;
}

u32 timers_dbg_target(u32 timer)
{
  return timer < NUM_TIMERS ? s_state.counters[timer].target : 0;
}

static void timers_add_sysclk_ticks(void* user, tick_count_t sysclk_ticks, tick_count_t ticks_late)
{
  (void)user;
  (void)ticks_late;
  if (system_unscale_ticks_to_overclock)
    sysclk_ticks = system_unscale_ticks_to_overclock(sysclk_ticks, &s_state.sysclk_ticks_carry);
  else
    sysclk_ticks = default_unscale(sysclk_ticks, &s_state.sysclk_ticks_carry);

  if (!s_state.counters[0].external_counting_enabled && s_state.counters[0].counting_enabled)
    timers_add_ticks(0, sysclk_ticks);
  if (!s_state.counters[1].external_counting_enabled && s_state.counters[1].counting_enabled)
    timers_add_ticks(1, sysclk_ticks);
  if (s_state.counters[2].external_counting_enabled) {
    tick_count_t sysclk_div_8_ticks = (sysclk_ticks + (tick_count_t)s_state.sysclk_div_8_carry) / 8;
    s_state.sysclk_div_8_carry      = (u32)((sysclk_ticks + (tick_count_t)s_state.sysclk_div_8_carry) % 8);
    timers_add_ticks(2, sysclk_div_8_ticks);
  } else if (s_state.counters[2].counting_enabled) {
    timers_add_ticks(2, sysclk_ticks);
  }

  timers_update_sysclk_event();
}

u32 timers_read_register(u32 offset)
{
  const u32 timer_index = (offset >> 4) & 0x03u;
  const u32 port_offset = offset & 0x0Fu;
  if (timer_index >= 3u) {
    ERROR_LOG("Timer read out of range: offset 0x%02X", offset);
    return UINT32_C(0xFFFFFFFF);
  }

  timer_counter_state_t* cs = &s_state.counters[timer_index];

  switch (port_offset) {
    case 0x00: {
      if (timer_index < 2 && (cs->external_counting_enabled || cs->mode.sync_enable) && gpu_synchronize_crtc) {
        /* T0/T1 depend on the GPU. */
        if (timer_index == 0 || (gpu_is_crtc_scanline_pending && gpu_is_crtc_scanline_pending()))
          gpu_synchronize_crtc();
      }

      timing_event_invoke_early(&s_state.sysclk_event, false);
      return timers_visible_counter(cs);
    }

    case 0x04: {
      if (timer_index < 2 && (cs->external_counting_enabled || cs->mode.sync_enable) && gpu_synchronize_crtc) {
        if (timer_index == 0 || (gpu_is_crtc_scanline_pending && gpu_is_crtc_scanline_pending()))
          gpu_synchronize_crtc();
      }

      timing_event_invoke_early(&s_state.sysclk_event, false);

      const u32 bits            = cs->mode.bits;
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
  if (timer_index >= 3u) {
    ERROR_LOG("Timer write out of range: offset 0%02X value 0x%08X", offset, value);
    return;
  }

  timer_counter_state_t* cs = &s_state.counters[timer_index];

  const bool update_gpu =
      timer_index < 2 && (cs->external_counting_enabled || cs->mode.sync_enable || port_offset == 4);
  if (update_gpu && gpu_synchronize_crtc) gpu_synchronize_crtc();

  timing_event_invoke_early(&s_state.sysclk_event, false);

  switch (port_offset) {
    case 0x00: {
      DEBUG_LOG("Timer %u write counter %u", timer_index, value);
      cs->counter    = value & 0xFFFFu;
      cs->reset_hold = cs->use_external_clock ? 0 : 1;
      if (timer_index == 2 || !cs->external_counting_enabled) timers_update_sysclk_event();
      break;
    }

    case 0x04: {
      static const u32 WRITE_MASK = 0xE3FFu; /* 0b1110_0011_1111_1111 */

      DEBUG_LOG("Timer %u write mode register 0x%04X", timer_index, value);
      cs->mode.bits = (value & WRITE_MASK) | (cs->mode.bits & ~WRITE_MASK);

      const u8 clock_source  = (u8)cs->mode.clock_source;
      cs->use_external_clock = (clock_source & (timer_index == 2 ? 2u : 1u)) != 0;

      cs->counter                  = 0;
      cs->reset_hold               = cs->use_external_clock ? 0 : 1;
      cs->irq_done                 = false;
      cs->mode.interrupt_request_n = 1;
      interrupt_controller_set_line_state(
          (interrupt_controller_irq_t)((u32)INTERRUPT_CONTROLLER_IRQ_TMR0 + timer_index), false);

      timers_update_counting_enabled(timer_index, cs);
      if (cs->target == 0) timers_signal_events(timer_index, true, false);
      timers_update_sysclk_event();
      break;
    }

    case 0x08: {
      DEBUG_LOG("Timer %u write target 0x%04X", timer_index, value & 0xFFFFu);
      cs->target = value & 0xFFFFu;
      if (timer_index == 2 || !cs->external_counting_enabled) timers_update_sysclk_event();
      break;
    }

    default:
      ERROR_LOG("Write unknown register in timer %u (offset 0x%02X, value 0x%X)", timer_index, offset, value);
      break;
  }
  if (update_gpu && gpu_reschedule_crtc) gpu_reschedule_crtc();
}

static void timers_update_counting_enabled(u32 timer, timer_counter_state_t* cs)
{
  if (timer == 2) {
    cs->counting_enabled = !cs->mode.sync_enable || cs->mode.sync_mode == SYNC_MODE_RESET_ON_GATE_END ||
                           cs->mode.sync_mode == SYNC_MODE_RESET_AND_RUN_ON_START;
  } else if (cs->mode.sync_enable) {
    switch ((timers_sync_mode_t)cs->mode.sync_mode) {
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
  } else {
    cs->counting_enabled = true;
  }

  cs->external_counting_enabled = cs->use_external_clock && cs->counting_enabled;
}

static tick_count_t timers_get_ticks_until_next_interrupt(void)
{
  tick_count_t minimum = system_get_max_slice_ticks ? system_get_max_slice_ticks() : default_max_slice_ticks();
  for (u32 timer = 0; timer < NUM_TIMERS; timer++) {
    const timer_counter_state_t* cs = &s_state.counters[timer];
    if (timer < 2 && cs->external_counting_enabled) continue;
    tick_count_t ticks = timers_get_ticks_until_irq(timer);
    if (ticks == INT32_MAX) continue;
    if (cs->external_counting_enabled) ticks = ticks * 8 - (tick_count_t)s_state.sysclk_div_8_carry;
    if (ticks < minimum) minimum = ticks;
  }
  if (minimum < 1) minimum = 1;
  return system_scale_ticks_to_overclock ? system_scale_ticks_to_overclock(minimum) : default_scale(minimum);
}

static void timers_update_sysclk_event(void)
{
  timing_event_schedule(&s_state.sysclk_event, timers_get_ticks_until_next_interrupt());
}
