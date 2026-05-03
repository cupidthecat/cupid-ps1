/*
 *

 * Smart-pointer wrappers (unique_aligned_ptr) dropped - C callers pair
 * AlignedMalloc/AlignedFree manually.
 */
#ifndef CUPID_COMMON_ALIGN_H
#define CUPID_COMMON_ALIGN_H

#include "types.h"

#include <stdlib.h>

/* Macros so a single name handles every integer width without templates. */

#define IsAligned(value, alignment)        (((value) % (alignment)) == 0)
#define AlignUp(value, alignment)          ((((value) + (alignment) - 1) / (alignment)) * (alignment))
#define AlignDown(value, alignment)        (((value) / (alignment)) * (alignment))
#define IsAlignedPow2(value, alignment)    (((value) & ((alignment) - 1)) == 0)
#define AlignUpPow2(value, alignment)      (((value) + (alignment) - 1) & ~((__typeof__(value))((alignment) - 1)))
#define AlignDownPow2(value, alignment)    ((value) & ~((__typeof__(value))((alignment) - 1)))
#define IsPow2(value)                      (((value) & ((value) - 1)) == 0)

ALWAYS_INLINE u32 PreviousPow2_u32(u32 v)
{
  if (v == 0) return 0;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  return (v >> 1) + 1;
}
ALWAYS_INLINE u64 PreviousPow2_u64(u64 v)
{
  if (v == 0) return 0;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v |= v >> 32;
  return (v >> 1) + 1;
}
ALWAYS_INLINE u32 NextPow2_u32(u32 v)
{
  if (v == 0) return 0;
  v--;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  v++;
  return v;
}
ALWAYS_INLINE u64 NextPow2_u64(u64 v)
{
  if (v == 0) return 0;
  v--;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v |= v >> 32;
  v++;
  return v;
}

#define PreviousPow2(v) _Generic((v), u32: PreviousPow2_u32, u64: PreviousPow2_u64)(v)
#define NextPow2(v)     _Generic((v), u32: NextPow2_u32,     u64: NextPow2_u64    )(v)

ALWAYS_INLINE void* AlignedMalloc(size_t size, size_t alignment)
{
  void* p = NULL;
  if (posix_memalign(&p, alignment, size) != 0)
    return NULL;
  return p;
}

ALWAYS_INLINE void AlignedFree(void* ptr)
{
  free(ptr);
}

#endif /* CUPID_COMMON_ALIGN_H */
