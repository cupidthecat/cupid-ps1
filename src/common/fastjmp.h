/*
 * Lightweight setjmp/longjmp without signal-mask save/restore overhead.
 * Used by the (future) recompiler exception path.  Linux x86-64 and
 * AArch64 only - Windows variants and other arches are dropped.
 */

#ifndef CUPID_COMMON_FASTJMP_H
#define CUPID_COMMON_FASTJMP_H

#include "common/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__)
#  define FASTJMP_BUF_SIZE 64u
#elif defined(__aarch64__)
#  define FASTJMP_BUF_SIZE 168u
#else
#  error "fastjmp: unsupported architecture (Linux x86_64 / AArch64 only)"
#endif

typedef struct {
  _Alignas(16) uint8_t buf[FASTJMP_BUF_SIZE];
} fastjmp_buf;

int  fastjmp_set(fastjmp_buf* buf);
void fastjmp_jmp(const fastjmp_buf* buf, int ret) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
