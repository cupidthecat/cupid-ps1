/* cupid-ps1 Windows terminal crash diagnostics and cleanup dispatch. */
#include "common/crash_handler.h"
#if defined(_WIN32)
#include "windows_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static crash_handler_cleanup_t s_cleanup;
static LPTOP_LEVEL_EXCEPTION_FILTER s_previous;
static volatile LONG s_reporting;

static void write_trace(void)
{
  void* frames[64];
  const USHORT count = CaptureStackBackTrace(0, 64, frames, NULL);
  for (USHORT i = 0; i < count; i++) fprintf(stderr, "  [%u] %p\n", (unsigned)i, frames[i]);
  fflush(stderr);
}

static LONG WINAPI unhandled_exception(PEXCEPTION_POINTERS exception)
{
  if (InterlockedCompareExchange(&s_reporting, 1, 0) == 0) {
    fprintf(stderr, "\n*** crash: Windows exception %08lX at %p ***\n",
            (unsigned long)exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    if (s_cleanup) s_cleanup();
    write_trace();
    if (s_previous) return s_previous(exception);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static void crt_signal(int number)
{
  if (InterlockedCompareExchange(&s_reporting, 1, 0) == 0) {
    fprintf(stderr, "\n*** crash: signal %d ***\n", number);
    if (s_cleanup) s_cleanup();
    write_trace();
  }
  signal(number, SIG_DFL);
  raise(number);
  _Exit(128 + number);
}

bool crash_handler_install(crash_handler_cleanup_t cleanup)
{
  s_cleanup = cleanup;
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(unhandled_exception);
  if (previous != unhandled_exception) s_previous = previous;
  const int signals[] = {SIGABRT, SIGFPE, SIGILL};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (signal(signals[i], crt_signal) == SIG_ERR) return false;
  return true;
}

void crash_handler_set_write_directory(const char* directory, u32 length)
{
  /* Diagnostics use stderr; this backend does not create dump files. */
  (void)directory; (void)length;
}

void crash_handler_write_dump_for_caller(const char* message, u32 length)
{
  fprintf(stderr, "crash dump (caller): %.*s\n", (int)length, message);
  write_trace();
}
#endif
