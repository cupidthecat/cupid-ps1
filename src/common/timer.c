#include "timer.h"

#include <errno.h>
#include <time.h>

double timer_get_frequency(void) { return 1.0; }

timer_value_t timer_get_current_value(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (timer_value_t)ts.tv_nsec + (timer_value_t)ts.tv_sec * 1000000000ull;
}

double timer_value_to_nanoseconds (timer_value_t v) { return (double)v; }
double timer_value_to_milliseconds(timer_value_t v) { return (double)v / 1000000.0; }
double timer_value_to_seconds     (timer_value_t v) { return (double)v / 1000000000.0; }

timer_value_t timer_seconds_to_value     (double s)  { return (timer_value_t)(s  * 1000000000.0); }
timer_value_t timer_milliseconds_to_value(double ms) { return (timer_value_t)(ms * 1000000.0); }
timer_value_t timer_nanoseconds_to_value (double ns) { return (timer_value_t)ns; }

void timer_nanosleep(u64 ns)
{
  const struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
  nanosleep(&ts, NULL);
}

void timer_busy_wait(u64 ns)
{
  const timer_value_t start = timer_get_current_value();
  const timer_value_t end   = start + timer_nanoseconds_to_value((double)ns);
  if (end < start) {
    while (timer_get_current_value() > end) { }
  }
  while (timer_get_current_value() < end) { }
}

void timer_hybrid_sleep(u64 ns, u64 min_sleep_time_ns)
{
  const timer_value_t start = timer_get_current_value();
  const timer_value_t end   = start + timer_nanoseconds_to_value((double)ns);
  if (end < start) {
    while (timer_get_current_value() > end) { }
  }
  timer_value_t cur = timer_get_current_value();
  while (cur < end) {
    const timer_value_t remaining = end - cur;
    if (remaining >= min_sleep_time_ns)
      timer_nanosleep(min_sleep_time_ns);
    cur = timer_get_current_value();
  }
}

void timer_sleep_until(timer_value_t v, bool exact)
{
  if (exact) {
    const timer_value_t guard   = (timer_value_t)(0.5 * 1000000.0);
    const timer_value_t wake_at = v - guard;
    if (wake_at > timer_get_current_value())
      timer_sleep_until(wake_at, false);
    while (timer_get_current_value() < v) { }
    return;
  }
  struct timespec ts;
  ts.tv_sec  = (time_t)(v / 1000000000ull);
  ts.tv_nsec = (long)  (v % 1000000000ull);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

void timer_init(timer_t_* t)                              { t->start = timer_get_current_value(); }
void timer_reset(timer_t_* t)                             { t->start = timer_get_current_value(); }
void timer_reset_to(timer_t_* t, timer_value_t v)         { t->start = v; }
timer_value_t timer_get_start(const timer_t_* t)          { return t->start; }

double timer_get_seconds(const timer_t_* t)      { return timer_value_to_seconds     (timer_get_current_value() - t->start); }
double timer_get_milliseconds(const timer_t_* t) { return timer_value_to_milliseconds(timer_get_current_value() - t->start); }
double timer_get_nanoseconds(const timer_t_* t)  { return timer_value_to_nanoseconds (timer_get_current_value() - t->start); }

double timer_get_seconds_and_reset(timer_t_* t)
{
  const timer_value_t v = timer_get_current_value();
  const double r = timer_value_to_seconds(v - t->start);
  t->start = v;
  return r;
}
double timer_get_milliseconds_and_reset(timer_t_* t)
{
  const timer_value_t v = timer_get_current_value();
  const double r = timer_value_to_milliseconds(v - t->start);
  t->start = v;
  return r;
}
double timer_get_nanoseconds_and_reset(timer_t_* t)
{
  const timer_value_t v = timer_get_current_value();
  const double r = timer_value_to_nanoseconds(v - t->start);
  t->start = v;
  return r;
}

bool timer_reset_if_seconds_passed(timer_t_* t, double s)
{
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_seconds(v - t->start) < s) return false;
  t->start = v;
  return true;
}
bool timer_reset_if_milliseconds_passed(timer_t_* t, double ms)
{
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_milliseconds(v - t->start) < ms) return false;
  t->start = v;
  return true;
}
bool timer_reset_if_nanoseconds_passed(timer_t_* t, double ns)
{
  const timer_value_t v = timer_get_current_value();
  if (timer_value_to_nanoseconds(v - t->start) < ns) return false;
  t->start = v;
  return true;
}
