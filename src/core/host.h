/*
 * frontend (SDL2 driver).  Here we declare plain extern functions
 * the core may call; the frontend provides the definitions.  std::function
 * callbacks become C function-pointer + user-data pairs; std::string_view
 * becomes (const char* ptr, size_t len) pairs; std::optional results become
 * out-params returning bool for "present".
 */

#ifndef CUPID_CORE_HOST_H
#define CUPID_CORE_HOST_H

#include "common/types.h"

#include <stddef.h>
#include <time.h>

typedef struct Error Error;

bool host_resource_file_exists(const char* filename, size_t filename_len, bool allow_override);

/* On success: *out_data malloc'd, *out_size set, returns true.  Caller frees. */
bool host_read_resource_file(const char* filename, size_t filename_len, bool allow_override,
                             u8** out_data, size_t* out_size, Error* error);

/* On success: *out_str malloc'd NUL-terminated, *out_len excludes NUL, returns true. */
bool host_read_resource_file_to_string(const char* filename, size_t filename_len, bool allow_override,
                                       char** out_str, size_t* out_len, Error* error);

bool host_get_resource_file_timestamp(const char* filename, size_t filename_len, bool allow_override,
                                      time_t* out_timestamp);

NORETURN void host_report_fatal_error(const char* title, size_t title_len,
                                      const char* message, size_t message_len);

void host_report_error_async(const char* title, size_t title_len,
                             const char* message, size_t message_len);

void host_report_status_message(const char* message, size_t message_len);

/* Confirm dialog.  Callback fires on an arbitrary thread; use
 * host_run_on_core_thread() inside it for VM-safe work.  yes_text/no_text may be
 * NULL for defaults. */
typedef void (*host_confirm_message_async_callback_t)(bool result, void* user);
void host_confirm_message_async(const char* title, size_t title_len,
                                const char* message, size_t message_len,
                                host_confirm_message_async_callback_t callback, void* user,
                                const char* yes_text, size_t yes_text_len,
                                const char* no_text, size_t no_text_len);

void host_open_url(const char* url, size_t url_len);

/* Returns malloc'd UTF-8 string (NUL-terminated) or NULL.  Caller frees. */
char* host_get_clipboard_text(void);

bool host_copy_text_to_clipboard(const char* text, size_t text_len);

/* Returned arrays are statically allocated; do not free.  *out_count is the
 * number of (display_name, code) pairs in *out_names / *out_codes. */
void host_get_available_language_list(const char* const** out_names,
                                      const char* const** out_codes,
                                      size_t* out_count);

const char* host_get_language_name(const char* language_code, size_t language_code_len);

bool host_change_language(const char* new_language);

bool host_is_on_core_thread(void);

/* fn(user) runs on the core (VM) thread.  If block, returns after fn finishes. */
void host_run_on_core_thread(void (*fn)(void* user), void* user, bool block);

/* fn(user) runs on the main/UI thread. */
void host_run_on_ui_thread(void (*fn)(void* user), void* user, bool block);

void host_queue_async_task(void (*fn)(void* user), void* user);
void host_wait_for_all_async_tasks(void);

void host_commit_base_setting_changes(void);

#endif /* CUPID_CORE_HOST_H */
