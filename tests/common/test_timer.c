/* cupid-ps1 host timer deadlines. */
#include "tests/test_harness.h"
#include "common/timer.h"
#include <unistd.h>

TEST(HostTimer, past_exact_deadline_does_not_wrap)
{
  alarm(3);
  timer_sleep_until(1, true);
  timer_sleep_until(0, true);
  alarm(0);
}
