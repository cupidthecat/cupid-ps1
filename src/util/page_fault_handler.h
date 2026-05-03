/*
 *

 * Used by the dynarec to detect writes to PSX RAM pages and invalidate
 * cached translations.
 */
#ifndef CUPID_UTIL_PAGE_FAULT_HANDLER_H
#define CUPID_UTIL_PAGE_FAULT_HANDLER_H

#include "common/types.h"

typedef struct Error Error;

typedef enum {
  PAGE_FAULT_HANDLER_CONTINUE_EXECUTION = 0,
  PAGE_FAULT_HANDLER_EXECUTE_NEXT,
} page_fault_handler_result_t;

/* Provided by the dynarec module (or any consumer); empty default in our
 * implementation calls a registered callback. */
page_fault_handler_result_t page_fault_handler_handle(void* exception_pc, void* fault_address, bool is_write);

bool page_fault_handler_install(Error* err);

/* Replaceable callback set by consumers (e.g. CPU recompiler). */
typedef page_fault_handler_result_t (*page_fault_handler_cb_t)(void* exception_pc, void* fault_address, bool is_write);
void page_fault_handler_set_callback(page_fault_handler_cb_t cb);

#endif /* CUPID_UTIL_PAGE_FAULT_HANDLER_H */
