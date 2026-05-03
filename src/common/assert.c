#include "assert.h"

#include <stdio.h>
#include <stdlib.h>

void Y_OnAssertFailed(const char* msg, const char* func, const char* file, unsigned line)
{
  char buf[512];
  snprintf(buf, sizeof(buf), "%s in function %s (%s:%u)\n", msg, func, file, line);
  fputs(buf, stderr);
  fflush(stderr);
  abort();
}

NORETURN void Y_OnPanicReached(const char* msg, const char* func, const char* file, unsigned line)
{
  char buf[512];
  snprintf(buf, sizeof(buf), "%s in function %s (%s:%u)\n", msg, func, file, line);
  fputs(buf, stderr);
  fflush(stderr);
  abort();
}
