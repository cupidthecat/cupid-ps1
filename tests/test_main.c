/* Test runner main(). Iterates the test registry built up by TEST() macro
 * constructors at process start, runs each test in a forked child for
 * isolation, prints PASS/FAIL/CRASH per test, exits non-zero on any failure.
 *
 * Filter: pass a substring as argv[1] to run only matching tests.
 */

#include "tests/test_harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static test_entry_t* s_head = NULL;
static test_entry_t* s_tail = NULL;
static u32           s_count = 0;

void test_register(test_entry_t* e)
{
  e->next = NULL;
  if (s_tail) s_tail->next = e;
  else        s_head = e;
  s_tail = e;
  s_count++;
}

void test_fail(const char* file, int line, const char* fmt, ...)
{
  fprintf(stderr, "    %s:%d ", file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  _exit(2);
}

static int run_one(test_entry_t* e)
{
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "  FORK failed for %s.%s\n", e->suite, e->name);
    return -1;
  }
  if (pid == 0) {
    e->fn();
    _exit(0);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status))   return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

int main(int argc, char** argv)
{
  const char* filter = (argc >= 2) ? argv[1] : NULL;

  u32 ran = 0, passed = 0, failed = 0;
  for (test_entry_t* e = s_head; e; e = e->next) {
    if (filter) {
      char fq[256];
      snprintf(fq, sizeof(fq), "%s.%s", e->suite, e->name);
      if (!strstr(fq, filter)) continue;
    }
    ran++;
    printf("[ RUN  ] %s.%s\n", e->suite, e->name);
    fflush(stdout);
    const int rc = run_one(e);
    if (rc == 0) {
      printf("[  OK  ] %s.%s\n", e->suite, e->name);
      passed++;
    } else if (rc >= 128) {
      printf("[CRASH ] %s.%s (signal %d)\n", e->suite, e->name, rc - 128);
      failed++;
    } else {
      printf("[ FAIL ] %s.%s (exit %d)\n", e->suite, e->name, rc);
      failed++;
    }
  }

  printf("\n%u test%s ran: %u passed, %u failed (registered: %u)\n",
         ran, ran == 1 ? "" : "s", passed, failed, s_count);
  return (failed == 0) ? 0 : 1;
}
