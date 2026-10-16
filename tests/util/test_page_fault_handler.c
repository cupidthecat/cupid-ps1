/* cupid-ps1 page-fault handler lifecycle checks. */
#include "tests/test_harness.h"
#include "common/error.h"
#include "util/page_fault_handler.h"

#include <signal.h>

static volatile sig_atomic_t s_received;

static void previous_handler(int sig)
{
  (void)sig;
  s_received = 1;
}

static void later_handler(int sig)
{
  (void)sig;
  s_received = 2;
}

TEST(PageFaultHandler, uninstall_restores_previous_handler_and_allows_reinstall)
{
  struct sigaction action = {0}, original;
  action.sa_handler       = previous_handler;
  sigemptyset(&action.sa_mask);
  EXPECT_EQ(sigaction(SIGSEGV, &action, &original), 0);
  Error error;
  Error_init(&error);
  for (unsigned i = 0; i < 2; i++) {
    EXPECT_TRUE(page_fault_handler_install(&error));
    EXPECT_TRUE(page_fault_handler_uninstall(&error));
    s_received = 0;
    EXPECT_EQ(raise(SIGSEGV), 0);
    EXPECT_EQ(s_received, 1);
  }
  EXPECT_EQ(sigaction(SIGSEGV, &original, NULL), 0);
  Error_destroy(&error);
}

TEST(PageFaultHandler, uninstall_keeps_a_handler_installed_later)
{
  Error error;
  Error_init(&error);
  struct sigaction original, action = {0};
  EXPECT_EQ(sigaction(SIGSEGV, NULL, &original), 0);
  EXPECT_TRUE(page_fault_handler_install(&error));
  action.sa_handler = later_handler;
  sigemptyset(&action.sa_mask);
  EXPECT_EQ(sigaction(SIGSEGV, &action, NULL), 0);
  EXPECT_TRUE(page_fault_handler_uninstall(&error));
  s_received = 0;
  EXPECT_EQ(raise(SIGSEGV), 0);
  EXPECT_EQ(s_received, 2);
  EXPECT_EQ(sigaction(SIGSEGV, &original, NULL), 0);
  Error_destroy(&error);
}

TEST(PageFaultHandler, uninstall_is_idempotent)
{
  Error error;
  Error_init(&error);
  EXPECT_TRUE(page_fault_handler_uninstall(&error));
  EXPECT_TRUE(page_fault_handler_uninstall(&error));
  Error_destroy(&error);
}
