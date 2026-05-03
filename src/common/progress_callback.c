#include "progress_callback.h"

#include "assert.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Host);

static void base_state_changed(progress_callback_t* self, u32 changed)
{
  (void)self; (void)changed;
}
static void base_set_title(progress_callback_t* self, const char* t, u32 len)
{
  (void)self; (void)t; (void)len;
}
static void base_alert_prompt(progress_callback_t* self, progress_prompt_icon_t icon,
                              const char* msg, u32 msg_len)
{
  (void)self; (void)icon; (void)msg; (void)msg_len;
}
static bool base_confirm_prompt(progress_callback_t* self, progress_prompt_icon_t icon,
                                const char* msg, u32 msg_len,
                                const char* yes_text, u32 yes_len,
                                const char* no_text,  u32 no_len)
{
  (void)self; (void)icon; (void)msg; (void)msg_len;
  (void)yes_text; (void)yes_len; (void)no_text; (void)no_len;
  return false;
}
static void base_append_message(progress_callback_t* self, const char* msg, u32 msg_len)
{
  (void)self;
  log_write_fmt(log_pack_category(LOG_CHANNEL_Host, LOG_LEVEL_INFO, LOG_COLOR_STRONG_ORANGE),
                "%.*s", (int)msg_len, msg);
}
static void base_set_auto_close(progress_callback_t* self, bool enabled)
{
  (void)self; (void)enabled;
}

static const progress_callback_vtable_t s_base_vtable = {
   base_state_changed,
  base_set_title,
  base_alert_prompt,
  base_confirm_prompt,
  base_append_message,
  base_set_auto_close, 
};

void progress_callback_init(progress_callback_t* pc)
{
  progress_callback_init_with_vtable(pc, &s_base_vtable);
}

void progress_callback_init_with_vtable(progress_callback_t* pc, const progress_callback_vtable_t* vtbl)
{
  pc->vtbl                = vtbl;
  small_string_stack_init(&pc->status_text);
  pc->progress_range      = 1;
  pc->progress_value      = 0;
  pc->base_progress_value = 0;
  pc->cancellable         = false;
  pc->cancelled           = false;
  pc->saved_state         = NULL;
}

void progress_callback_destroy(progress_callback_t* pc)
{
  small_string_destroy(&pc->status_text.s);
  progress_state_t* s = pc->saved_state;
  while (s) {
    progress_state_t* next = s->next;
    small_string_destroy(&s->status_text.s);
    free(s);
    s = next;
  }
  pc->saved_state = NULL;
}

bool progress_callback_is_cancellable(const progress_callback_t* pc) { return pc->cancellable; }
bool progress_callback_is_cancelled  (const progress_callback_t* pc) { return pc->cancelled; }

void progress_callback_set_cancellable(progress_callback_t* pc, bool cancellable)
{
  if (pc->cancellable == cancellable) return;
  pc->cancellable = cancellable;
  pc->vtbl->state_changed(pc, PROGRESS_STATE_CHANGE_CANCELLABLE);
}

void progress_callback_set_title(progress_callback_t* pc, const char* title, u32 title_len)
{
  pc->vtbl->set_title(pc, title, title_len);
}

void progress_callback_set_status_text(progress_callback_t* pc, const char* text, u32 text_len)
{
  if (small_string_equals_view(&pc->status_text.s, text, text_len))
    return;

  log_write_fmt(log_pack_category(LOG_CHANNEL_Host, LOG_LEVEL_INFO, LOG_COLOR_DEFAULT),
                "Status: %.*s", (int)text_len, text);

  small_string_assign_view(&pc->status_text.s, text, text_len);
  pc->vtbl->state_changed(pc, PROGRESS_STATE_CHANGE_STATUS_TEXT);
}

void progress_callback_set_status_text_fmt(progress_callback_t* pc, const char* fmt, ...)
{
  tiny_string_t buf;
  tiny_string_init(&buf);
  va_list ap; va_start(ap, fmt);
  small_string_vsprintf(&buf.s, fmt, ap);
  va_end(ap);
  progress_callback_set_status_text(pc, small_string_c_str(&buf.s), small_string_length(&buf.s));
  small_string_destroy(&buf.s);
}

void progress_callback_push_state(progress_callback_t* pc)
{
  progress_state_t* st = (progress_state_t*)malloc(sizeof(*st));
  small_string_stack_init((small_string_stack_t*)&st->status_text);
  small_string_assign(&st->status_text.s, &pc->status_text.s);
  st->cancellable         = pc->cancellable;
  st->progress_range      = pc->progress_range;
  st->progress_value      = pc->progress_value;
  st->base_progress_value = pc->base_progress_value;
  st->next                = pc->saved_state;
  pc->saved_state         = st;
}

void progress_callback_pop_state(progress_callback_t* pc)
{
  DebugAssert(pc->saved_state);
  progress_state_t* st = pc->saved_state;

  const u32 new_value = (pc->progress_range != 0)
    ? (u32)(((float)pc->progress_value / (float)pc->progress_range) * (float)st->progress_range)
    : st->progress_value;

  u32 changed = PROGRESS_STATE_CHANGE_NONE;
  if (!small_string_equals(&pc->status_text.s, &st->status_text.s))
    changed |= PROGRESS_STATE_CHANGE_STATUS_TEXT;
  if (pc->progress_range != st->progress_range || pc->progress_value != new_value)
    changed |= PROGRESS_STATE_CHANGE_PROGRESS;
  if (pc->cancellable != st->cancellable)
    changed |= PROGRESS_STATE_CHANGE_CANCELLABLE;

  pc->cancellable         = st->cancellable;
  small_string_assign    (&pc->status_text.s, &st->status_text.s);
  pc->base_progress_value = st->base_progress_value;
  pc->progress_range      = st->progress_range;
  pc->progress_value      = new_value;
  pc->saved_state         = st->next;

  small_string_destroy(&st->status_text.s);
  free(st);

  if (changed != PROGRESS_STATE_CHANGE_NONE)
    pc->vtbl->state_changed(pc, changed);
}

void progress_callback_set_state(progress_callback_t* pc, u32 value, u32 range)
{
  progress_callback_set_state_full(pc, small_string_c_str(&pc->status_text.s),
                                   small_string_length(&pc->status_text.s),
                                   value, range, pc->cancellable);
}

void progress_callback_set_state_with_text(progress_callback_t* pc, const char* text, u32 text_len,
                                           u32 value, u32 range)
{
  progress_callback_set_state_full(pc, text, text_len, value, range, pc->cancellable);
}

void progress_callback_set_state_full(progress_callback_t* pc, const char* text, u32 text_len,
                                      u32 value, u32 range, bool cancellable)
{
  u32 changed = PROGRESS_STATE_CHANGE_NONE;
  if (pc->cancellable != cancellable) {
    pc->cancellable = cancellable;
    changed |= PROGRESS_STATE_CHANGE_CANCELLABLE;
  }
  if (!small_string_equals_view(&pc->status_text.s, text, text_len)) {
    small_string_assign_view(&pc->status_text.s, text, text_len);
    changed |= PROGRESS_STATE_CHANGE_STATUS_TEXT;
  }

  const u32 prev_range = pc->progress_range;
  const u32 prev_value = pc->progress_value;

  if (pc->saved_state) {
    pc->progress_range      = pc->saved_state->progress_range * range;
    pc->progress_value      = pc->saved_state->progress_value * range;
    pc->base_progress_value = pc->progress_value;
  } else {
    pc->progress_range      = range;
    pc->progress_value      = value;
    pc->base_progress_value = 0;
  }

  if (range != prev_range || value != prev_value)
    changed |= PROGRESS_STATE_CHANGE_PROGRESS;
  if (changed != PROGRESS_STATE_CHANGE_NONE)
    pc->vtbl->state_changed(pc, changed);
}

void progress_callback_set_progress_range(progress_callback_t* pc, u32 range)
{
  const u32 prev_range = pc->progress_range;
  const u32 prev_value = pc->progress_value;

  if (pc->saved_state) {
    pc->progress_range      = pc->saved_state->progress_range * range;
    pc->progress_value      = pc->saved_state->progress_value * range;
    pc->base_progress_value = pc->progress_value;
  } else {
    pc->progress_range      = range;
    pc->progress_value      = 0;
    pc->base_progress_value = 0;
  }

  if (pc->progress_range != prev_range || pc->progress_value != prev_value)
    pc->vtbl->state_changed(pc, PROGRESS_STATE_CHANGE_PROGRESS);
}

void progress_callback_set_progress_value(progress_callback_t* pc, u32 value)
{
  const u32 nv = pc->base_progress_value + value;
  if (pc->progress_value != nv) {
    pc->progress_value = nv;
    pc->vtbl->state_changed(pc, PROGRESS_STATE_CHANGE_PROGRESS);
  }
}

void progress_callback_increment_progress(progress_callback_t* pc)
{
  progress_callback_set_progress_value(pc, (pc->progress_value - pc->base_progress_value) + 1);
}

void progress_callback_alert_prompt(progress_callback_t* pc, progress_prompt_icon_t icon,
                                    const char* msg, u32 msg_len)
{
  pc->vtbl->alert_prompt(pc, icon, msg, msg_len);
}

bool progress_callback_confirm_prompt(progress_callback_t* pc, progress_prompt_icon_t icon,
                                      const char* msg, u32 msg_len,
                                      const char* yes_text, u32 yes_len,
                                      const char* no_text,  u32 no_len)
{
  return pc->vtbl->confirm_prompt(pc, icon, msg, msg_len, yes_text, yes_len, no_text, no_len);
}

void progress_callback_append_message(progress_callback_t* pc, const char* msg, u32 msg_len)
{
  pc->vtbl->append_message(pc, msg, msg_len);
}

void progress_callback_set_status_text_and_append_message(progress_callback_t* pc, const char* msg, u32 msg_len)
{
  progress_callback_set_status_text(pc, msg, msg_len);
  progress_callback_append_message(pc, msg, msg_len);
}

void progress_callback_set_auto_close(progress_callback_t* pc, bool enabled)
{
  pc->vtbl->set_auto_close(pc, enabled);
}

static progress_callback_t s_null;
static bool                s_null_inited;

progress_callback_t* progress_callback_null(void)
{
  if (!s_null_inited) {
    progress_callback_init(&s_null);
    s_null_inited = true;
  }
  return &s_null;
}
