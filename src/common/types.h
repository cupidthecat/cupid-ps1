#ifndef CUPID_COMMON_TYPES_H
#define CUPID_COMMON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif

#ifndef NDEBUG
#define ALWAYS_INLINE_RELEASE inline
#else
#define ALWAYS_INLINE_RELEASE ALWAYS_INLINE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NEVER_INLINE __attribute__((noinline))
#else
#define NEVER_INLINE
#endif

#ifndef UNREFERENCED_VARIABLE
#define UNREFERENCED_VARIABLE(P) ((void)(P))
#endif

#ifndef countof
#define countof(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#if defined(__GNUC__) || defined(__clang__)
#define OFFSETOF(st, m) __builtin_offsetof(st, m)
#define PRINTFLIKE(n, m) __attribute__((format(printf, n, m)))
#else
#define OFFSETOF(st, m) offsetof(st, m)
#define PRINTFLIKE(n, m)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NORETURN_FUNCTION_POINTER __attribute__((noreturn))
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN_FUNCTION_POINTER
#define NORETURN
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ASSUME(x)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(x))                                                                                                          \
      __builtin_unreachable();                                                                                         \
  } while (0)
#else
#define ASSUME(x) ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RESTRICT __restrict__
#else
#define RESTRICT
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

typedef int8_t   s8;
typedef uint8_t  u8;
typedef int16_t  s16;
typedef uint16_t u16;
typedef int32_t  s32;
typedef uint32_t u32;
typedef int64_t  s64;
typedef uint64_t u64;

#if defined(__GNUC__) || defined(__clang__)
#  if defined(__x86_64__)
#    define CPU_ARCH_X64 1
#  elif defined(__i386__)
#    define CPU_ARCH_X86 1
#  elif defined(__aarch64__)
#    define CPU_ARCH_ARM64 1
#  elif defined(__arm__)
#    define CPU_ARCH_ARM32 1
#  elif defined(__riscv) && __riscv_xlen == 64
#    define CPU_ARCH_RISCV64 1
#  elif defined(__loongarch64)
#    define CPU_ARCH_LOONGARCH64 1
#  else
#    error Unknown architecture.
#  endif
#else
#  error Unsupported compiler.
#endif

#if defined(CPU_ARCH_X64)
#  define CPU_ARCH_STR "x64"
#elif defined(CPU_ARCH_X86)
#  define CPU_ARCH_STR "x86"
#elif defined(CPU_ARCH_ARM32)
#  define CPU_ARCH_STR "arm32"
#elif defined(CPU_ARCH_ARM64)
#  define CPU_ARCH_STR "arm64"
#elif defined(CPU_ARCH_RISCV64)
#  define CPU_ARCH_STR "riscv64"
#elif defined(CPU_ARCH_LOONGARCH64)
#  define CPU_ARCH_STR "loongarch64"
#else
#  define CPU_ARCH_STR "Unknown"
#endif

#if defined(__linux__)
#  define TARGET_OS_STR "Linux"
#  define TARGET_OS_LINUX 1
#else
#  error cupid-ps1 is Linux-only.
#endif

#ifndef HOST_PAGE_SIZE_OVERRIDE
#  define HOST_PAGE_SIZE  0x1000u
#  define HOST_PAGE_MASK  (HOST_PAGE_SIZE - 1u)
#  define HOST_PAGE_SHIFT 12u
#else
#  define HOST_PAGE_SIZE  HOST_PAGE_SIZE_OVERRIDE
#  define HOST_PAGE_MASK  (HOST_PAGE_SIZE - 1u)
#  define HOST_PAGE_SHIFT host_page_shift_runtime() /* not expected */
#endif
#define MIN_HOST_PAGE_SIZE HOST_PAGE_SIZE
#define MAX_HOST_PAGE_SIZE HOST_PAGE_SIZE

#ifndef HOST_CACHE_LINE_SIZE
#  define HOST_CACHE_LINE_SIZE 64u
#endif
#define ALIGN_TO_CACHE_LINE _Alignas(HOST_CACHE_LINE_SIZE)

#define BASE_FROM_RECORD_FIELD(ptr, base_type, field)                                                                  \
  ((base_type*)(((char*)(ptr)) - OFFSETOF(base_type, field)))

/* In C, enum bitwise ops are already integer ops. Macro kept as no-op so any
 * porting site that called IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(T) compiles. */
#define IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(type_) /* no-op in C */

#endif /* CUPID_COMMON_TYPES_H */
