/*
 * Doubly-linked list of active events kept sorted by next_run_time.  The
 * scheduler tail-walks the list, invoking each event whose next_run_time
 * has been reached, and re-sorts after every fired callback (the callback
 * may have re-scheduled itself or another event).
 *
 * Talks to cpu_core via cpu_get_pending_ticks / cpu_reset_pending_ticks /
 * cpu_add_pending_ticks (inline) and cpu_dispatch_interrupt + g_cpu_state.
 */

#include "core/timing_event.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(TimingEvents);

typedef struct {
  timing_event_t* active_events_head;
  timing_event_t* active_events_tail;
  timing_event_t* current_event;
  u32             active_event_count;
  global_ticks_t  current_event_next_run_time;
  global_ticks_t  global_tick_counter;
  global_ticks_t  event_run_tick_counter;
} timing_events_state_t;

ALIGN_TO_CACHE_LINE static timing_events_state_t s_state;

static void           timing_events_sort_event(timing_event_t* event);
static void           timing_events_add_active(timing_event_t* event);
static void           timing_events_remove_active(timing_event_t* event);
static void           timing_events_sort_events(void);
static timing_event_t* timing_events_find_active(const char* name_data, u32 name_len);
static global_ticks_t timing_events_get_timestamp_for_new_event(void);
static void           timing_events_commit_global_ticks(global_ticks_t new_global_ticks);

global_ticks_t timing_events_get_global_tick_counter(void)
{
  return s_state.global_tick_counter;
}

global_ticks_t timing_events_get_event_run_tick_counter(void)
{
  return s_state.event_run_tick_counter;
}

static global_ticks_t timing_events_get_timestamp_for_new_event(void)
{
  /* Schedule relative to the currently-being-processed event, but if we
   * haven't run events in a while, include the pending CPU time so events
   * scheduled mid-block don't fire before they should. */
  return s_state.global_tick_counter + (global_ticks_t)cpu_get_pending_ticks();
}

void timing_events_initialize(void)
{
  timing_events_reset();
}

void timing_events_reset(void)
{
  s_state.global_tick_counter    = 0;
  s_state.event_run_tick_counter = 0;
}

void timing_events_shutdown(void)
{
  Assert(s_state.active_event_count == 0);
}

void timing_events_update_cpu_downcount(void)
{
  DebugAssert(s_state.active_events_head->next_run_time >= s_state.global_tick_counter);
  const u32 event_downcount =
    (u32)(s_state.active_events_head->next_run_time - s_state.global_tick_counter);
  g_cpu_state.downcount = cpu_has_pending_interrupt() ? 0u : event_downcount;
}

timing_event_t** timing_events_get_head_event_ptr(void)
{
  return &s_state.active_events_head;
}

void timing_events_set_global_tick_counter(global_ticks_t ticks)
{
  s_state.global_tick_counter = ticks;
}

static void timing_events_sort_event(timing_event_t* event)
{
  const global_ticks_t event_runtime = event->next_run_time;

  if (event->prev && event->prev->next_run_time > event_runtime)
  {
    /* move backwards */
    timing_event_t* current = event->prev;
    while (current && current->next_run_time > event_runtime)
      current = current->prev;

    /* unlink */
    if (event->prev)
      event->prev->next = event->next;
    else
      s_state.active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_state.active_events_tail = event->prev;

    /* insert after current */
    if (current)
    {
      event->next = current->next;
      if (current->next)
        current->next->prev = event;
      else
        s_state.active_events_tail = event;

      event->prev = current;
      current->next = event;
    }
    else
    {
      /* insert at front */
      DebugAssert(s_state.active_events_head);
      s_state.active_events_head->prev = event;
      event->prev = NULL;
      event->next = s_state.active_events_head;
      s_state.active_events_head = event;
      timing_events_update_cpu_downcount();
    }
  }
  else if (event->next && event_runtime > event->next->next_run_time)
  {
    /* move forwards */
    timing_event_t* current = event->next;
    while (current && event_runtime > current->next_run_time)
      current = current->next;

    /* unlink */
    if (event->prev)
    {
      event->prev->next = event->next;
    }
    else
    {
      s_state.active_events_head = event->next;
      if (!s_state.current_event)
        timing_events_update_cpu_downcount();
    }
    if (event->next)
      event->next->prev = event->prev;
    else
      s_state.active_events_tail = event->prev;

    /* insert before current */
    if (current)
    {
      event->next = current;
      event->prev = current->prev;

      if (current->prev)
      {
        current->prev->next = event;
      }
      else
      {
        s_state.active_events_head = event;
        if (!s_state.current_event)
          timing_events_update_cpu_downcount();
      }

      current->prev = event;
    }
    else
    {
      /* insert at back */
      DebugAssert(s_state.active_events_tail);
      s_state.active_events_tail->next = event;
      event->next = NULL;
      event->prev = s_state.active_events_tail;
      s_state.active_events_tail = event;
    }
  }
}

static void timing_events_add_active(timing_event_t* event)
{
  DebugAssert(!event->prev && !event->next);
  s_state.active_event_count++;

  const global_ticks_t event_runtime = event->next_run_time;
  timing_event_t* current = NULL;
  timing_event_t* next = s_state.active_events_head;
  while (next && event_runtime > next->next_run_time)
  {
    current = next;
    next = next->next;
  }

  if (!next)
  {
    /* new tail */
    event->prev = s_state.active_events_tail;
    if (s_state.active_events_tail)
    {
      s_state.active_events_tail->next = event;
      s_state.active_events_tail = event;
    }
    else
    {
      /* first event */
      s_state.active_events_tail = event;
      s_state.active_events_head = event;
      timing_events_update_cpu_downcount();
    }
  }
  else if (!current)
  {
    /* new head */
    event->next = s_state.active_events_head;
    s_state.active_events_head->prev = event;
    s_state.active_events_head = event;
    timing_events_update_cpu_downcount();
  }
  else
  {
    /* in-between current < event > next */
    event->prev = current;
    event->next = next;
    current->next = event;
    next->prev = event;
  }
}

static void timing_events_remove_active(timing_event_t* event)
{
  DebugAssert(s_state.active_event_count > 0);

  if (event->next)
    event->next->prev = event->prev;
  else
    s_state.active_events_tail = event->prev;

  if (event->prev)
  {
    event->prev->next = event->next;
  }
  else
  {
    s_state.active_events_head = event->next;
    if (s_state.active_events_head && !s_state.current_event)
      timing_events_update_cpu_downcount();
  }

  event->prev = NULL;
  event->next = NULL;

  s_state.active_event_count--;
}

static void timing_events_sort_events(void)
{
  /* Snapshot all events into a small heap array, then re-add in order to
   * normalise the linked list after a save-state load. */
  const u32 count = s_state.active_event_count;
  timing_event_t** events = NULL;
  if (count > 0)
  {
    events = (timing_event_t**)malloc(sizeof(timing_event_t*) * count);
    if (!events)
      Panic("Out of memory sorting timing events");
  }

  u32 idx = 0;
  timing_event_t* next = s_state.active_events_head;
  while (next)
  {
    timing_event_t* current = next;
    events[idx++] = current;
    next = current->next;
    current->prev = NULL;
    current->next = NULL;
  }

  s_state.active_events_head = NULL;
  s_state.active_events_tail = NULL;
  s_state.active_event_count = 0;

  for (u32 i = 0; i < count; i++)
    timing_events_add_active(events[i]);

  free(events);
}

static timing_event_t* timing_events_find_active(const char* name_data, u32 name_len)
{
  for (timing_event_t* event = s_state.active_events_head; event; event = event->next)
  {
    if (event->name_len == name_len && memcmp(event->name_data, name_data, name_len) == 0)
      return event;
  }
  return NULL;
}

bool timing_events_is_running_events(void)
{
  return (s_state.current_event != NULL);
}

void timing_events_cancel_running_event(void)
{
  timing_event_t* const event = s_state.current_event;
  if (!event)
    return;

  /* Might need to sort it, since we're bailing out. */
  if (event->active)
  {
    event->next_run_time = s_state.current_event_next_run_time;
    timing_events_sort_event(event);
  }

  s_state.current_event = NULL;
}

ALWAYS_INLINE_RELEASE static void timing_events_commit_global_ticks(global_ticks_t new_global_ticks)
{
  s_state.event_run_tick_counter = new_global_ticks;

  do
  {
    timing_event_t* event = s_state.active_events_head;
    s_state.global_tick_counter =
      (new_global_ticks < event->next_run_time) ? new_global_ticks : event->next_run_time;

    /* Run all callbacks whose next_run_time has been reached. */
    while (s_state.global_tick_counter >= event->next_run_time)
    {
      s_state.current_event = event;

      const tick_count_t ticks_late      = (tick_count_t)(s_state.global_tick_counter - event->next_run_time);
      const tick_count_t ticks_to_execute = (tick_count_t)(s_state.global_tick_counter - event->last_run_time);

      /* Why don't we modify event->downcount directly?  Because the active
       * list won't be sorted: adding the interval may give this event a
       * later run time than its successor, and a new event scheduled in
       * the callback could be inserted at the front despite having a
       * later run time too.  Stash the next time and sort once after. */
      s_state.current_event_next_run_time = event->next_run_time + (u32)event->interval;
      event->last_run_time = s_state.global_tick_counter;

      /* ticks_late is informational; it doesn't subtract from ticks_to_execute. */
      event->callback(event->callback_param, ticks_to_execute, ticks_late);
      if (event->active)
      {
        event->next_run_time = s_state.current_event_next_run_time;
        timing_events_sort_event(event);
      }

      event = s_state.active_events_head;
    }
  } while (new_global_ticks > s_state.global_tick_counter);
  s_state.current_event = NULL;
}

void timing_events_run_events(void)
{
  DebugAssert(!s_state.current_event);

  do
  {
    const global_ticks_t new_global_ticks =
      s_state.event_run_tick_counter + (global_ticks_t)cpu_get_pending_ticks();
    if (new_global_ticks >= s_state.active_events_head->next_run_time)
    {
      cpu_reset_pending_ticks();
      timing_events_commit_global_ticks(new_global_ticks);
    }

    if (cpu_has_pending_interrupt())
      cpu_dispatch_interrupt();

    timing_events_update_cpu_downcount();
  } while (cpu_get_pending_ticks() >= g_cpu_state.downcount);
}

void timing_events_commit_leftover_ticks(void)
{
  timing_events_commit_global_ticks(s_state.event_run_tick_counter);

  if (cpu_has_pending_interrupt())
    cpu_dispatch_interrupt();

  timing_events_update_cpu_downcount();
}

bool timing_events_do_state(state_wrapper_t* sw)
{
  if (state_wrapper_get_version(sw) < 71u)
  {
    u32 old_global_tick_counter = 0;
    state_wrapper_do_u32(sw, &old_global_tick_counter);
    s_state.global_tick_counter    = (global_ticks_t)old_global_tick_counter;
    s_state.event_run_tick_counter = s_state.global_tick_counter;

    u32 event_count = 0;
    state_wrapper_do_u32(sw, &event_count);

    for (u32 i = 0; i < event_count; i++)
    {
      tiny_string_t event_name; tiny_string_init(&event_name);
      tick_count_t downcount = 0, time_since_last_run = 0, period = 0, interval = 0;
      state_wrapper_do_string(sw, &event_name.s);
      state_wrapper_do_s32   (sw, &downcount);
      state_wrapper_do_s32   (sw, &time_since_last_run);
      state_wrapper_do_s32   (sw, &period);
      state_wrapper_do_s32   (sw, &interval);
      if (state_wrapper_has_error(sw))
      {
        small_string_destroy(&event_name.s);
        return false;
      }

      timing_event_t* event = timing_events_find_active(event_name.s.buffer, event_name.s.length);
      if (!event)
      {
        WARNING_LOG("Save state has event '%s', but couldn't find this event when loading.",
                    small_string_c_str(&event_name.s));
        small_string_destroy(&event_name.s);
        continue;
      }

      event->next_run_time = s_state.global_tick_counter + (u32)downcount;
      event->last_run_time = s_state.global_tick_counter - (u32)time_since_last_run;
      event->period   = period;
      event->interval = interval;
      small_string_destroy(&event_name.s);
    }

    if (state_wrapper_get_version(sw) < 43u)
    {
      u32 last_event_run_time = 0;
      state_wrapper_do_u32(sw, &last_event_run_time);
    }

    DEBUG_LOG("Loaded %u events from save state.", event_count);
    s_state.current_event = NULL;

    /* Add pending ticks to the CPU; we may have saved while the VM was running. */
    const tick_count_t pending_ticks =
      (tick_count_t)(s_state.event_run_tick_counter - s_state.global_tick_counter);
    DebugAssert(pending_ticks >= 0);
    cpu_add_pending_ticks(pending_ticks);
    timing_events_sort_events();
    timing_events_update_cpu_downcount();
  }
  else
  {
    state_wrapper_do_u64(sw, &s_state.global_tick_counter);
    state_wrapper_do_u64(sw, &s_state.event_run_tick_counter);

    if (state_wrapper_is_reading(sw))
    {
      u32 event_count = 0;
      state_wrapper_do_u32(sw, &event_count);

      for (u32 i = 0; i < event_count; i++)
      {
        tiny_string_t event_name; tiny_string_init(&event_name);
        global_ticks_t next_run_time = 0, last_run_time = 0;
        tick_count_t period = 0, interval = 0;
        state_wrapper_do_string(sw, &event_name.s);
        state_wrapper_do_u64   (sw, &next_run_time);
        state_wrapper_do_u64   (sw, &last_run_time);
        state_wrapper_do_s32   (sw, &period);
        state_wrapper_do_s32   (sw, &interval);
        if (state_wrapper_has_error(sw))
        {
          small_string_destroy(&event_name.s);
          return false;
        }

        timing_event_t* event = timing_events_find_active(event_name.s.buffer, event_name.s.length);
        if (!event)
        {
          WARNING_LOG("Save state has event '%s', but couldn't find this event when loading.",
                      small_string_c_str(&event_name.s));
          small_string_destroy(&event_name.s);
          continue;
        }

        event->next_run_time = next_run_time;
        event->last_run_time = last_run_time;
        event->period   = period;
        event->interval = interval;
        small_string_destroy(&event_name.s);
      }

      DEBUG_LOG("Loaded %u events from save state.", event_count);

      s_state.current_event = NULL;

      timing_events_sort_events();
      timing_events_update_cpu_downcount();
    }
    else
    {
      state_wrapper_do_u32(sw, &s_state.active_event_count);

      for (timing_event_t* event = s_state.active_events_head; event; event = event->next)
      {
        tiny_string_t name; tiny_string_init(&name);
        small_string_assign_view(&name.s, event->name_data, event->name_len);
        state_wrapper_do_string(sw, &name.s);
        global_ticks_t next_run_time =
          (s_state.current_event == event) ? s_state.current_event_next_run_time : event->next_run_time;
        state_wrapper_do_u64(sw, &next_run_time);
        state_wrapper_do_u64(sw, &event->last_run_time);
        state_wrapper_do_s32(sw, &event->period);
        state_wrapper_do_s32(sw, &event->interval);
        small_string_destroy(&name.s);
      }

      DEBUG_LOG("Wrote %u events to save state.", s_state.active_event_count);
    }
  }

  return !state_wrapper_has_error(sw);
}

void timing_event_init(timing_event_t* ev,
                       const char* name_data, u32 name_len,
                        tick_count_t period, tick_count_t interval,
                       timing_event_cb_t callback, void* callback_param) 
{
  ev->prev = NULL;
  ev->next = NULL;
  ev->callback = callback;
  ev->callback_param = callback_param;
  ev->period   = period;
  ev->interval = interval;
  ev->active   = false;
  ev->name_data = name_data;
  ev->name_len  = name_len;

  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  ev->last_run_time = ts;
  ev->next_run_time = ts + (u32)interval;
}

void timing_event_destroy(timing_event_t* ev)
{
  DebugAssert(!ev->active);
  (void)ev;
}

tick_count_t timing_event_get_ticks_since_last_execution(const timing_event_t* ev)
{
  /* Can be negative if event A->B invoked B early while in the event loop. */
  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  return (ts >= ev->last_run_time) ? (tick_count_t)(ts - ev->last_run_time) : 0;
}

tick_count_t timing_event_get_ticks_until_next_execution(const timing_event_t* ev)
{
  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  return (ts >= ev->next_run_time) ? 0 : (tick_count_t)(ev->next_run_time - ts);
}

void timing_event_delay(timing_event_t* ev, tick_count_t ticks)
{
  if (!ev->active)
  {
    Panic("Trying to delay an inactive event");
    return;
  }

  DebugAssert(s_state.current_event != ev);

  ev->next_run_time += (u32)ticks;
  timing_events_sort_event(ev);
  if (s_state.active_events_head == ev)
    timing_events_update_cpu_downcount();
}

void timing_event_schedule(timing_event_t* ev, tick_count_t ticks)
{
  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  const global_ticks_t next_run_time = ts + (u32)ticks;

  /* If we're rescheduling the currently-running event, latch its new
   * run-time into the post-callback slot. */
  s_state.current_event_next_run_time =
    (s_state.current_event == ev) ? next_run_time : s_state.current_event_next_run_time;

  if (!ev->active)
  {
    /* Going active: only count ticks from the current timestamp. */
    ev->next_run_time = next_run_time;
    ev->last_run_time = ts;
    ev->active = true;
    timing_events_add_active(ev);
  }
  else
  {
    /* Already active; leave time-since-last alone, just update the downcount.
     * If this is from an MMIO handler, re-sort the queue. */
    if (s_state.current_event != ev)
    {
      ev->next_run_time = next_run_time;
      timing_events_sort_event(ev);
      if (s_state.active_events_head == ev)
        timing_events_update_cpu_downcount();
    }
  }
}

void timing_event_set_interval_and_schedule(timing_event_t* ev, tick_count_t ticks)
{
  DebugAssert(ticks > 0);
  timing_event_set_interval(ev, ticks);
  timing_event_schedule(ev, ticks);
}

void timing_event_set_period_and_schedule(timing_event_t* ev, tick_count_t ticks)
{
  timing_event_set_period(ev, ticks);
  timing_event_set_interval(ev, ticks);
  timing_event_schedule(ev, ticks);
}

void timing_event_invoke_early(timing_event_t* ev, bool force)
{
  if (!ev->active)
    return;

  /* May happen due to other invoke_early()s mid event loop. */
  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  if (ts <= ev->last_run_time)
    return;

  /* Invoking yourself while you're the current callback would recurse;
   * the original Asserts here. */
  Assert(s_state.current_event != ev);

  const tick_count_t ticks_to_execute = (tick_count_t)(ts - ev->last_run_time);
  if (!force && ticks_to_execute < ev->period)
    return;

  ev->next_run_time = ts + (u32)ev->interval;
  ev->last_run_time = ts;

  /* Downcount changed -> resort. */
  timing_events_sort_event(ev);
  if (s_state.active_events_head == ev)
    timing_events_update_cpu_downcount();

  ev->callback(ev->callback_param, ticks_to_execute, 0);
}

void timing_event_activate(timing_event_t* ev)
{
  if (ev->active)
    return;

  const global_ticks_t ts = timing_events_get_timestamp_for_new_event();
  const global_ticks_t next_run_time = ts + (u32)ev->interval;
  ev->next_run_time = next_run_time;
  ev->last_run_time = ts;

  s_state.current_event_next_run_time =
    (s_state.current_event == ev) ? next_run_time : s_state.current_event_next_run_time;

  ev->active = true;
  timing_events_add_active(ev);
}

void timing_event_deactivate(timing_event_t* ev)
{
  if (!ev->active)
    return;

  ev->active = false;
  timing_events_remove_active(ev);
}
