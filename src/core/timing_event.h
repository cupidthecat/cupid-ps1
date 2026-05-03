/*
 * The C++ class TimingEvent (callback subclassed via a function pointer in
 * the original) folds into a plain struct: there are no virtuals, just a
 * fp + user pair.  The "subclass this" goes in user_param.
 *
 * Active events form a doubly-linked list sorted by next_run_time.  We don't
 * use a heap (yet); the active count is small (~20) so linear insert is fine.
 *
 * namespace TimingEvents -> timing_events_ prefix.
 */

#ifndef CUPID_CORE_TIMING_EVENT_H
#define CUPID_CORE_TIMING_EVENT_H

#include "core/types.h"

typedef struct state_wrapper state_wrapper_t;

 /*
 * (user, ticks_executed, ticks_late).  */
typedef void (*timing_event_cb_t)(void* user, tick_count_t ticks, tick_count_t ticks_late);

typedef struct timing_event {
  struct timing_event* prev;
  struct timing_event* next;

  timing_event_cb_t callback;
  void* callback_param;

  global_ticks_t next_run_time;
  global_ticks_t last_run_time;

  tick_count_t period;
  tick_count_t interval;
  bool         active;

  /* Borrowed string view: caller owns storage and must outlive the event.
   * Save-state lookup compares by name, so it must be stable. */
  const char* name_data;
  u32         name_len;
} timing_event_t;

/* Initialise with a name view, period, interval, callback + user.  The event
 * starts inactive; call timing_event_activate() (or schedule()) to add it
 * to the active list. */
void timing_event_init(timing_event_t* ev,
                       const char* name_data, u32 name_len,
                       tick_count_t period, tick_count_t interval,
                       timing_event_cb_t callback, void* callback_param);

/* Asserts the event was deactivated by the caller before destruction. */
void timing_event_destroy(timing_event_t* ev);

ALWAYS_INLINE bool         timing_event_is_active (const timing_event_t* ev) { return ev->active; }
ALWAYS_INLINE tick_count_t timing_event_get_period(const timing_event_t* ev) { return ev->period; }
ALWAYS_INLINE tick_count_t timing_event_get_interval(const timing_event_t* ev) { return ev->interval; }

 /* Pending time is included (i.e. measured against the current "as if we ran
 * events now" timestamp). */
tick_count_t timing_event_get_ticks_since_last_execution(const timing_event_t* ev);
tick_count_t timing_event_get_ticks_until_next_execution(const timing_event_t* ev);

/* Adds ticks to the current execution.  Asserts the event is active. */
void timing_event_delay(timing_event_t* ev, tick_count_t ticks);

void timing_event_schedule(timing_event_t* ev, tick_count_t ticks);
void timing_event_set_interval_and_schedule(timing_event_t* ev, tick_count_t ticks);
void timing_event_set_period_and_schedule  (timing_event_t* ev, tick_count_t ticks);

/* Drain the event with whatever time has accumulated.  When force is false
 * and less than `period` ticks are pending, the callback is *not* invoked. */
void timing_event_invoke_early(timing_event_t* ev, bool force);

/* Adds/removes from the active list.  Do NOT call from inside a callback;
 * return Deactivate flag instead by setting active = false. */
void timing_event_activate  (timing_event_t* ev);
void timing_event_deactivate(timing_event_t* ev);

ALWAYS_INLINE void timing_event_set_state(timing_event_t* ev, bool active)
{
  if (active) timing_event_activate(ev); else timing_event_deactivate(ev);
}

/* Direct setters without rescheduling.  Caller guarantees no scheduling
 * invariant break (e.g. only called outside the active loop). */
ALWAYS_INLINE void timing_event_set_interval(timing_event_t* ev, tick_count_t interval) { ev->interval = interval; }
ALWAYS_INLINE void timing_event_set_period  (timing_event_t* ev, tick_count_t period)   { ev->period   = period; }

global_ticks_t timing_events_get_global_tick_counter   (void);
global_ticks_t timing_events_get_event_run_tick_counter(void);

void timing_events_initialize(void);
void timing_events_reset     (void);
void timing_events_shutdown  (void);

bool timing_events_do_state(state_wrapper_t* sw);

bool timing_events_is_running_events(void);
void timing_events_cancel_running_event(void);
void timing_events_run_events(void);
void timing_events_commit_leftover_ticks(void);

void timing_events_update_cpu_downcount(void);

/* Direct access to the head of the active-events list; used by save-state
 * iteration in other modules. */
timing_event_t** timing_events_get_head_event_ptr(void);

/* GPU dump replayer hook: forces the global counter forward.  Don't use for
 * normal operation; breaks invariants the active queue depends on. */
void timing_events_set_global_tick_counter(global_ticks_t ticks);

#endif /* CUPID_CORE_TIMING_EVENT_H */
