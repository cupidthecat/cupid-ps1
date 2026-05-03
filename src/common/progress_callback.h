/*
 * C++ ProgressCallback (virtual base) → struct progress_callback_t with a
 * vtable.  The "with prompt" subclass collapses into the same struct: just
 * supply prompt-handling vtable entries.
 */

#ifndef CUPID_COMMON_PROGRESS_CALLBACK_H
#define CUPID_COMMON_PROGRESS_CALLBACK_H

#include "small_string.h"
#include "types.h"

typedef struct progress_callback progress_callback_t;
typedef struct progress_state    progress_state_t;

typedef enum {
  PROGRESS_STATE_CHANGE_NONE        = 0,
  PROGRESS_STATE_CHANGE_PROGRESS    = 1 << 0,
  PROGRESS_STATE_CHANGE_STATUS_TEXT = 1 << 1,
  PROGRESS_STATE_CHANGE_CANCELLABLE = 1 << 2,
} progress_state_change_t;

typedef enum {
  PROGRESS_PROMPT_ICON_ERROR,
  PROGRESS_PROMPT_ICON_WARNING,
  PROGRESS_PROMPT_ICON_QUESTION,
  PROGRESS_PROMPT_ICON_INFORMATION,
} progress_prompt_icon_t;

typedef struct {
  void (*state_changed)  (progress_callback_t* self, u32 changed_flags);
  void (*set_title)      (progress_callback_t* self, const char* title, u32 title_len);
  void (*alert_prompt)   (progress_callback_t* self, progress_prompt_icon_t icon,
                          const char* msg, u32 msg_len);
  bool (*confirm_prompt) (progress_callback_t* self, progress_prompt_icon_t icon,
                          const char* msg, u32 msg_len,
                          const char* yes_text, u32 yes_len,
                          const char* no_text,  u32 no_len);
  void (*append_message) (progress_callback_t* self, const char* msg, u32 msg_len);
  void (*set_auto_close) (progress_callback_t* self, bool enabled);
} progress_callback_vtable_t;

struct progress_state {
  progress_state_t* next;
  small_string_stack_t status_text;
  u32 progress_range;
  u32 progress_value;
  u32 base_progress_value;
  bool cancellable;
};

struct progress_callback {
  const progress_callback_vtable_t* vtbl;

  small_string_stack_t status_text;
  u32  progress_range;     /* default 1 */
  u32  progress_value;     /* default 0 */
  u32  base_progress_value;
  bool cancellable;
  bool cancelled;
  progress_state_t* saved_state; /* singly-linked stack, head = top */
};

/* Initialize with default base vtable (all overrides are no-ops). */
void progress_callback_init(progress_callback_t* pc);
void progress_callback_init_with_vtable(progress_callback_t* pc, const progress_callback_vtable_t* vtbl);
void progress_callback_destroy(progress_callback_t* pc);

bool progress_callback_is_cancellable(const progress_callback_t* pc);
bool progress_callback_is_cancelled  (const progress_callback_t* pc);
void progress_callback_set_cancellable(progress_callback_t* pc, bool cancellable);

void progress_callback_set_title(progress_callback_t* pc, const char* title, u32 title_len);

void progress_callback_set_status_text     (progress_callback_t* pc, const char* text, u32 text_len);
void progress_callback_set_status_text_fmt (progress_callback_t* pc, const char* fmt, ...) PRINTFLIKE(2, 3);

void progress_callback_push_state(progress_callback_t* pc);
void progress_callback_pop_state (progress_callback_t* pc);

void progress_callback_set_state            (progress_callback_t* pc, u32 value, u32 range);
void progress_callback_set_state_with_text  (progress_callback_t* pc, const char* text, u32 text_len, u32 value, u32 range);
void progress_callback_set_state_full       (progress_callback_t* pc, const char* text, u32 text_len, u32 value, u32 range, bool cancellable);

void progress_callback_set_progress_range (progress_callback_t* pc, u32 range);
void progress_callback_set_progress_value (progress_callback_t* pc, u32 value);
void progress_callback_increment_progress (progress_callback_t* pc);

void progress_callback_alert_prompt   (progress_callback_t* pc, progress_prompt_icon_t icon,
                                       const char* msg, u32 msg_len);
bool progress_callback_confirm_prompt (progress_callback_t* pc, progress_prompt_icon_t icon,
                                       const char* msg, u32 msg_len,
                                       const char* yes_text, u32 yes_len,
                                       const char* no_text,  u32 no_len);
void progress_callback_append_message (progress_callback_t* pc, const char* msg, u32 msg_len);
void progress_callback_set_status_text_and_append_message(progress_callback_t* pc, const char* msg, u32 msg_len);
void progress_callback_set_auto_close (progress_callback_t* pc, bool enabled);

/* Process-wide no-op singleton. */
progress_callback_t* progress_callback_null(void);

#endif /* CUPID_COMMON_PROGRESS_CALLBACK_H */
