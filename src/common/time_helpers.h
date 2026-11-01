#ifndef CUPID_COMMON_TIME_HELPERS_H
#define CUPID_COMMON_TIME_HELPERS_H

#include "types.h"

#include <time.h>

/* True on success, false on failure.  Caller owns out. */
static ALWAYS_INLINE bool common_local_time(time_t tvalue, struct tm* out)
{
#if defined(_WIN32)
  return localtime_s(out, &tvalue) == 0;
#else
  return localtime_r(&tvalue, out) != NULL;
#endif
}

#endif /* CUPID_COMMON_TIME_HELPERS_H */
