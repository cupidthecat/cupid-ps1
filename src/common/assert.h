#ifndef CUPID_COMMON_ASSERT_H
#define CUPID_COMMON_ASSERT_H

#include "types.h"

void Y_OnAssertFailed(const char* msg, const char* func, const char* file, unsigned line);
NORETURN void Y_OnPanicReached(const char* msg, const char* func, const char* file, unsigned line);

#define Assert(expr)                                                                                                   \
  do {                                                                                                                 \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Assertion failed: '" #expr "'", __func__, __FILE__, __LINE__);                                 \
  } while (0)

#define AssertMsg(expr, msg)                                                                                           \
  do {                                                                                                                 \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Assertion failed: '" msg "'", __func__, __FILE__, __LINE__);                                   \
  } while (0)

#if !defined(NDEBUG)
#  define DebugAssert(expr)                                                                                            \
    do {                                                                                                               \
      if (!(expr))                                                                                                     \
        Y_OnAssertFailed("Debug assertion failed: '" #expr "'", __func__, __FILE__, __LINE__);                         \
    } while (0)
#  define DebugAssertMsg(expr, msg)                                                                                    \
    do {                                                                                                               \
      if (!(expr))                                                                                                     \
        Y_OnAssertFailed("Debug assertion failed: '" msg "'", __func__, __FILE__, __LINE__);                           \
    } while (0)
#else
#  define DebugAssert(expr)         ((void)0)
#  define DebugAssertMsg(expr, msg) ((void)0)
#endif

#define Panic(message)  Y_OnPanicReached("Panic triggered: '" message "'", __func__, __FILE__, __LINE__)
#define PureCall()      Y_OnPanicReached("PureCall encountered",          __func__, __FILE__, __LINE__)

#if !defined(NDEBUG)
#  define UnreachableCode() Y_OnPanicReached("Unreachable code reached", __func__, __FILE__, __LINE__)
#else
#  define UnreachableCode() ASSUME(false)
#endif

#define DefaultCaseIsUnreachable()                                                                                     \
  default:                                                                                                             \
    UnreachableCode();                                                                                                 \
    break

#endif /* CUPID_COMMON_ASSERT_H */
