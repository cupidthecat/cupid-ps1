#include "timer.h"

#if defined(_WIN32)
#include "windows/windows_util.h"
#include "assert.h"
#else
#include <errno.h>
#include <time.h>
#endif

double timer_get_frequency(void) {
  return 1.0;
}

timer_value_t timer_get_current_value(void) {
#if defined(_WIN32)
  LARGE_INTEGER counter, frequency;
  if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    Panic("Reading the monotonic clock failed");
  const u64 seconds   = (u64)counter.QuadPart / (u64)frequency.QuadPart;
  const u64 remainder = (u64)counter.QuadPart % (u64)frequency.QuadPart;
  return seconds * 1000000000ull + (u64)((long double)remainder * 1000000000.0L / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (timer_value_t)ts.tv_nsec + (timer_value_t)ts.tv_sec * 1000000000ull;
#endif
}

double timer_value_to_nanoseconds(timer_value_t v) {
  return (double)v;
}
double timer_value_to_milliseconds(timer_value_t v) {
  return (double)v / 1000000.0;
}
double timer_value_to_seconds(timer_value_t v) {
  return (double)v / 1000000000.0;
}

timer_value_t timer_seconds_to_value(double s) {
  return (timer_value_t)(s * 1000000000.0);
}
timer_value_t timer_milliseconds_to_value(double ms) {
  return (timer_value_t)(ms * 1000000.0);
}
timer_value_t timer_nanoseconds_to_value(double ns) {
  return (timer_value_t)ns;
}

void timer_nanosleep(u64 ns) {
#if defined(_WIN32)
  if (!ns) {
    SwitchToThread();
    return;
  }
  HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer && GetLastError() == ERROR_INVALID_PARAMETER)
    timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
  if (!timer) Panic("Creating a sleep timer failed");
  LARGE_INTEGER due;
  due.QuadPart          = -(LONGLONG)(ns / 100u + (ns % 100u != 0));
  const bool  scheduled = SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0) != 0;
  const DWORD result    = scheduled ? WaitForSingleObject(timer, INFINITE) : WAIT_FAILED;
  CloseHandle(timer);
  if (result != WAIT_OBJECT_0) Panic("Waiting for the sleep timer failed");
#else
  const struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
  nanosleep(&ts, NULL);
#endif
}

void timer_busy_wait(u64 ns) {
  const timer_value_t start = timer_get_current_value();
  const timer_value_t end   = start + timer_nanoseconds_to_value((double)ns);
  if (end < start) {
    while (timer_get_current_value() > end) {
    }
  }
  while (timer_get_current_value() < end) {
  }
}

void timer_hybrid_sleep(u64 ns, u64 min_sleep_time_ns) {
  const timer_value_t start = timer_get_current_value();
  const timer_value_t end   = start + timer_nanoseconds_to_value((double)ns);
  if (end < start) {
    while (timer_get_current_value() > end) {
    }
  }
  timer_value_t cur = timer_get_current_value();
  while (cur < end) {
    const timer_value_t remaining = end - cur;
    if (remaining >= min_sleep_time_ns) timer_nanosleep(min_sleep_time_ns);
    cur = timer_get_current_value();
  }
}

void timer_sleep_until(timer_value_t v, bool exact) {
  if (exact) {
    const timer_value_t guard   = (timer_value_t)(0.5 * 1000000.0);
    const timer_value_t wake_at = v > guard ? v - guard : 0;
    if (wake_at > timer_get_current_value()) timer_sleep_until(wake_at, false);
    while (timer_get_current_value() < v) {
    }
    return;
  }
#if defined(_WIN32)
  timer_value_t now;
  while ((now = timer_get_current_value()) < v)
    timer_nanosleep(v - now);
#else
  struct timespec ts;
  ts.tv_sec  = (time_t)(v / 1000000000ull);
  ts.tv_nsec = (long)(v % 1000000000ull);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
  }
#endif
}

void timer_init(timer_t_* t) {
  t->start = timer_get_current_value();
}
void timer_reset(timer_t_* t) {
  t->start = timer_get_current_value();
}
void timer_reset_to(timer_t_* t, timer_value_t v) {
  t->start = v;
}
timer_value_t timer_get_start(const timer_t_* t) {
  return t->start;
}

double timer_get_seconds(const timer_t_* t) {
  return timer_value_to_seconds(timer_get_current_value() - t->start);
}
double timer_get_milliseconds(const timer_t_* t) {
  return timer_value_to_milliseconds(timer_get_current_value() - t->start);
}
double timer_get_nanoseconds(const timer_t_* t) {
  return timer_value_to_nanoseconds(timer_get_current_value() - t->start);
}

double timer_get_seconds_and_reset(timer_t_* t) {
  const timer_value_t v = timer_get_current_value();
  const double        r = timer_value_to_seconds(v - t->start);
  t->start              = v;
  return r;
}
double timer_get_milliseconds_and_reset(timer_t_* t) {
  const timer_value_t v = timer_get_current_value();
  const double        r = timer_value_to_milliseconds(v - t->start);
  t->start              = v;
  return r;
}
double timer_get_nanoseconds_and_reset(timer_t_* t) {
  const timer_value_t v = timer_get_current_value();
  const double        r = timer_value_to_nanoseconds(v - t->start);
  t->start              = v;
  return r;
}

bool timer_reset_if_seconds_passed(timer_t_* t, double s) {
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_seconds(v - t->start) < s) return false;
  t->start = v;
  return true;
}
bool timer_reset_if_milliseconds_passed(timer_t_* t, double ms) {
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_milliseconds(v - t->start) < ms) return false;
  t->start = v;
  return true;
}
bool timer_reset_if_nanoseconds_passed(timer_t_* t, double ns) {
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_nanoseconds(v - t->start) < ns) return false;
  t->start = v;
  return true;
}
