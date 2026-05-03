/*
 * On Linux all values are direct nanoseconds (frequency = 1.0), so most
 * conversions are casts.
 */

#ifndef CUPID_COMMON_TIMER_H
#define CUPID_COMMON_TIMER_H

#include "types.h"

typedef u64 timer_value_t;

typedef struct {
  timer_value_t start;
} timer_t_;          /* underscore: <time.h> already typedefs `timer_t` */

double        timer_get_frequency(void);
timer_value_t timer_get_current_value(void);

double        timer_value_to_seconds     (timer_value_t v);
double        timer_value_to_milliseconds(timer_value_t v);
double        timer_value_to_nanoseconds (timer_value_t v);
timer_value_t timer_seconds_to_value     (double s);
timer_value_t timer_milliseconds_to_value(double ms);
timer_value_t timer_nanoseconds_to_value (double ns);

void timer_busy_wait(u64 ns);
void timer_hybrid_sleep(u64 ns, u64 min_sleep_time_ns);
void timer_nanosleep(u64 ns);
void timer_sleep_until(timer_value_t v, bool exact);

void          timer_init(timer_t_* t);
void          timer_reset(timer_t_* t);
void          timer_reset_to(timer_t_* t, timer_value_t v);
timer_value_t timer_get_start(const timer_t_* t);

double timer_get_seconds(const timer_t_* t);
double timer_get_milliseconds(const timer_t_* t);
double timer_get_nanoseconds(const timer_t_* t);

double timer_get_seconds_and_reset(timer_t_* t);
double timer_get_milliseconds_and_reset(timer_t_* t);
double timer_get_nanoseconds_and_reset(timer_t_* t);

bool timer_reset_if_seconds_passed(timer_t_* t, double s);
bool timer_reset_if_milliseconds_passed(timer_t_* t, double ms);
bool timer_reset_if_nanoseconds_passed(timer_t_* t, double ns);

#endif /* CUPID_COMMON_TIMER_H */
