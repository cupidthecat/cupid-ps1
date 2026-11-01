/* cupid-ps1 Windows access-violation dispatch and handler ownership. */
#include "util/platform/page_fault_handler.h"
#if defined(_WIN32)
#include "common/windows/windows_util.h"
#include "common/error.h"
#include <stdatomic.h>

static SRWLOCK                          s_install_lock = SRWLOCK_INIT;
static PVOID                            s_handler;
static _Atomic(page_fault_handler_cb_t) s_callback;
static _Thread_local bool               s_in_handler;

void page_fault_handler_set_callback(page_fault_handler_cb_t cb)
{
  atomic_store_explicit(&s_callback, cb, memory_order_release);
}

page_fault_handler_result_t page_fault_handler_handle(void* pc, void* address, bool write)
{
  page_fault_handler_cb_t cb = atomic_load_explicit(&s_callback, memory_order_acquire);
  return cb ? cb(pc, address, write) : PAGE_FAULT_HANDLER_EXECUTE_NEXT;
}

static LONG CALLBACK dispatch_exception(PEXCEPTION_POINTERS exception)
{
  EXCEPTION_RECORD* record = exception->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
      record->ExceptionInformation[0] > 1 || s_in_handler)
    return EXCEPTION_CONTINUE_SEARCH;
  s_in_handler = true;
  const page_fault_handler_result_t result =
      page_fault_handler_handle((void*)(uintptr_t)exception->ContextRecord->Rip, (void*)record->ExceptionInformation[1],
                                record->ExceptionInformation[0] == 1);
  s_in_handler = false;
  return result == PAGE_FAULT_HANDLER_CONTINUE_EXECUTION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

bool page_fault_handler_install(Error* error)
{
  AcquireSRWLockExclusive(&s_install_lock);
  if (s_handler) {
    ReleaseSRWLockExclusive(&s_install_lock);
    Error_set_string(error, "The access-violation handler is already installed");
    return false;
  }
  s_handler = AddVectoredExceptionHandler(1, dispatch_exception);
  if (!s_handler) windows_set_error(error, "Installing access-violation handler", GetLastError());
  const bool result = s_handler != NULL;
  ReleaseSRWLockExclusive(&s_install_lock);
  return result;
}

bool page_fault_handler_uninstall(Error* error)
{
  AcquireSRWLockExclusive(&s_install_lock);
  if (s_handler && !RemoveVectoredExceptionHandler(s_handler)) {
    windows_set_error(error, "Removing access-violation handler", GetLastError());
    ReleaseSRWLockExclusive(&s_install_lock);
    return false;
  }
  s_handler = NULL;
  page_fault_handler_set_callback(NULL);
  ReleaseSRWLockExclusive(&s_install_lock);
  return true;
}
#endif
