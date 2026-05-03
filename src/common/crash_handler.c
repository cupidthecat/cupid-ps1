/*
 * to stderr on SIGSEGV/SIGABRT/SIGFPE/SIGILL via execinfo.h, then runs the
 * cleanup callback and re-raises with default disposition.
 */

#include "crash_handler.h"

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static crash_handler_cleanup_t s_cleanup;
static char                    s_dump_dir[1024];
static u32                     s_dump_dir_len;
static volatile sig_atomic_t   s_in_signal_handler;

static void write_backtrace(void)
{
  void* frames[64];
  int   n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

void crash_handler_signal_handler(int signo, siginfo_t* si, void* ctx)
{
  (void)si; (void)ctx;

  /* Avoid re-entrance: if we crash inside the handler (e.g. inside backtrace),
   * skip the cleanup/backtrace and just abort. */
  if (!s_in_signal_handler)
  {
    s_in_signal_handler = 1;
    fprintf(stderr, "\n*** crash: signal %d (%s) ***\n", signo, strsignal(signo));
    fflush(stderr);
    if (s_cleanup) s_cleanup();
    write_backtrace();
    s_in_signal_handler = 0;
  }

  /* We can't continue from here.  Force SIGABRT with default disposition so
   * the OS produces a core dump. */
  static const char abort_message[] = "Aborting application.\n";
  (void)!write(STDERR_FILENO, abort_message, sizeof(abort_message) - 1);

  struct sigaction sa = {0};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGABRT, &sa, NULL);
  raise(SIGABRT);
}

bool crash_handler_install(crash_handler_cleanup_t cleanup)
{
  s_cleanup = cleanup;

  struct sigaction sa = {0};
  sa.sa_sigaction = crash_handler_signal_handler;
  /* SA_NODEFER: a recursive fault inside the handler must not be blocked,
   * otherwise we'd silently deadlock instead of dumping. */
  sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);

  const int signals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (sigaction(signals[i], &sa, NULL) != 0)
      return false;
  return true;
}

void crash_handler_set_write_directory(const char* dir, u32 dir_len)
{
  if (dir_len >= sizeof(s_dump_dir)) dir_len = (u32)sizeof(s_dump_dir) - 1;
  memcpy(s_dump_dir, dir, dir_len);
  s_dump_dir[dir_len] = '\0';
  s_dump_dir_len      = dir_len;
}

void crash_handler_write_dump_for_caller(const char* msg, u32 msg_len)
{
  fprintf(stderr, "crash dump (caller): %.*s\n", (int)msg_len, msg);
  write_backtrace();
}
