#ifndef CUPID_TESTS_TEST_HARNESS_H
#define CUPID_TESTS_TEST_HARNESS_H

#include "common/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_fn_t)(void);

typedef struct test_entry {
  const char*        suite;
  const char*        name;
  test_fn_t          fn;
  const char*        file;
  int                line;
  struct test_entry* next;
} test_entry_t;

void test_register(test_entry_t* e);

/* Mark current test as failed; printed by the harness with file:line.
 * Calls _exit(2) to abort the per-test child cleanly. */
void test_fail(const char* file, int line, const char* fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));

#define TEST(suite_, name_)                                                    \
  static void test_##suite_##_##name_##_fn(void);                              \
  static test_entry_t test_##suite_##_##name_##_entry = {                      \
    .suite = #suite_, .name = #name_,                                          \
    .fn = test_##suite_##_##name_##_fn,                                        \
    .file = __FILE__, .line = __LINE__,                                        \
    .next = NULL,                                                              \
  };                                                                           \
  __attribute__((constructor)) static void                                     \
    test_##suite_##_##name_##_register(void)                                   \
  {                                                                            \
    test_register(&test_##suite_##_##name_##_entry);                           \
  }                                                                            \
  static void test_##suite_##_##name_##_fn(void)

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond))                                                               \
      test_fail(__FILE__, __LINE__, "EXPECT_TRUE(%s) failed", #cond);          \
  } while (0)

#define EXPECT_FALSE(cond)                                                     \
  do {                                                                         \
    if ((cond))                                                                \
      test_fail(__FILE__, __LINE__, "EXPECT_FALSE(%s) failed", #cond);         \
  } while (0)

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    long long _a = (long long)(a);                                             \
    long long _b = (long long)(b);                                             \
    if (_a != _b)                                                              \
      test_fail(__FILE__, __LINE__, "EXPECT_EQ(%s, %s) failed: %lld != %lld",  \
                #a, #b, _a, _b);                                               \
  } while (0)

#define EXPECT_NE(a, b)                                                        \
  do {                                                                         \
    long long _a = (long long)(a);                                             \
    long long _b = (long long)(b);                                             \
    if (_a == _b)                                                              \
      test_fail(__FILE__, __LINE__, "EXPECT_NE(%s, %s) failed: both = %lld",   \
                #a, #b, _a);                                                   \
  } while (0)

#define EXPECT_STREQ(a, b)                                                     \
  do {                                                                         \
    const char* _sa = (a);                                                     \
    const char* _sb = (b);                                                     \
    if (!_sa || !_sb || strcmp(_sa, _sb) != 0)                                 \
      test_fail(__FILE__, __LINE__, "EXPECT_STREQ failed: \"%s\" != \"%s\"",   \
                _sa ? _sa : "(null)", _sb ? _sb : "(null)");                   \
  } while (0)

#define EXPECT_NULL(p)                                                         \
  do {                                                                         \
    if ((p) != NULL)                                                           \
      test_fail(__FILE__, __LINE__, "EXPECT_NULL(%s) failed", #p);             \
  } while (0)

#define EXPECT_NOT_NULL(p)                                                     \
  do {                                                                         \
    if ((p) == NULL)                                                           \
      test_fail(__FILE__, __LINE__, "EXPECT_NOT_NULL(%s) failed", #p);         \
  } while (0)

#define EXPECT_FLOAT_NEAR(a, b, eps)                                           \
  do {                                                                         \
    double _a = (double)(a);                                                   \
    double _b = (double)(b);                                                   \
    double _e = (double)(eps);                                                 \
    double _d = _a - _b;                                                       \
    if (_d < 0) _d = -_d;                                                      \
    if (_d > _e)                                                               \
      test_fail(__FILE__, __LINE__,                                            \
                "EXPECT_FLOAT_NEAR(%s,%s,%s) failed: |%g - %g| = %g > %g",     \
                #a, #b, #eps, _a, _b, _d, _e);                                 \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif
