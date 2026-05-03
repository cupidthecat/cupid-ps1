/*
 *

 * C++ templates collapsed to per-width inline funcs, with _Generic umbrellas
 */
#ifndef CUPID_COMMON_BITUTILS_H
#define CUPID_COMMON_BITUTILS_H

#include "types.h"

ALWAYS_INLINE u16 ZeroExtend16(u8 v)  { return (u16)v; }
ALWAYS_INLINE u32 ZeroExtend32_u8 (u8  v) { return (u32)v; }
ALWAYS_INLINE u32 ZeroExtend32_u16(u16 v) { return (u32)v; }
ALWAYS_INLINE u64 ZeroExtend64_u8 (u8  v) { return (u64)v; }
ALWAYS_INLINE u64 ZeroExtend64_u16(u16 v) { return (u64)v; }
ALWAYS_INLINE u64 ZeroExtend64_u32(u32 v) { return (u64)v; }

#define ZeroExtend32(v) ((u32)(v))
#define ZeroExtend64(v) ((u64)(v))

ALWAYS_INLINE u16 SignExtend16(s8 v)  { return (u16)(s16)v; }
ALWAYS_INLINE u32 SignExtend32_s8 (s8  v) { return (u32)(s32)v; }
ALWAYS_INLINE u32 SignExtend32_s16(s16 v) { return (u32)(s32)v; }
ALWAYS_INLINE u64 SignExtend64_s8 (s8  v) { return (u64)(s64)v; }
ALWAYS_INLINE u64 SignExtend64_s16(s16 v) { return (u64)(s64)v; }
ALWAYS_INLINE u64 SignExtend64_s32(s32 v) { return (u64)(s64)v; }

#define SignExtend32(v) ((u32)(s32)(v))
#define SignExtend64(v) ((u64)(s64)(v))

#define Truncate8(v)  ((u8) (v))
#define Truncate16(v) ((u16)(v))
#define Truncate32(v) ((u32)(v))

ALWAYS_INLINE u8    BoolToUInt8 (bool v) { return (u8)v;  }
ALWAYS_INLINE u16   BoolToUInt16(bool v) { return (u16)v; }
ALWAYS_INLINE u32   BoolToUInt32(bool v) { return (u32)v; }
ALWAYS_INLINE u64   BoolToUInt64(bool v) { return (u64)v; }
ALWAYS_INLINE float BoolToFloat (bool v) { return (float)v; }

#define ConvertToBool(v) ((bool)(v))

ALWAYS_INLINE bool ConvertToBoolUnchecked_u8(u8 v)
{
  bool r;
  memcpy(&r, &v, sizeof(bool));
  return r;
}

ALWAYS_INLINE u32 SignExtendN_u32(u32 value, unsigned nbits)
{
  const unsigned shift = 32u - nbits;
  return (u32)((s32)(value << shift) >> shift);
}
ALWAYS_INLINE u64 SignExtendN_u64(u64 value, unsigned nbits)
{
  const unsigned shift = 64u - nbits;
  return (u64)((s64)(value << shift) >> shift);
}

#if defined(__GNUC__) || defined(__clang__)
ALWAYS_INLINE unsigned CountLeadingZeros_u32(u32 v) { return (unsigned)__builtin_clz((unsigned)v); }
ALWAYS_INLINE unsigned CountLeadingZeros_u64(u64 v) { return (unsigned)__builtin_clzll((unsigned long long)v); }
ALWAYS_INLINE unsigned CountTrailingZeros_u32(u32 v) { return (unsigned)__builtin_ctz((unsigned)v); }
ALWAYS_INLINE unsigned CountTrailingZeros_u64(u64 v) { return (unsigned)__builtin_ctzll((unsigned long long)v); }
#else
#  error CLZ/CTZ require GCC/clang builtins.
#endif

#define CountLeadingZeros(v)                                                                                           \
  _Generic((v),                                                                                                        \
           u8 : CountLeadingZeros_u32((u32)(v)) - 24u,                                                                 \
           u16: CountLeadingZeros_u32((u32)(v)) - 16u,                                                                 \
           u32: CountLeadingZeros_u32((v)),                                                                            \
           u64: CountLeadingZeros_u64((v))) 

#define CountTrailingZeros(v)                                                                                          \
  _Generic((v),                                                                                                        \
           u8 : CountTrailingZeros_u32((u32)(v)),                                                                      \
           u16: CountTrailingZeros_u32((u32)(v)),                                                                      \
           u32: CountTrailingZeros_u32((v)),                                                                           \
           u64: CountTrailingZeros_u64((v))) 

#if defined(__GNUC__) || defined(__clang__)
ALWAYS_INLINE u16 ByteSwap_u16(u16 v) { return __builtin_bswap16(v); }
ALWAYS_INLINE u32 ByteSwap_u32(u32 v) { return __builtin_bswap32(v); }
ALWAYS_INLINE u64 ByteSwap_u64(u64 v) { return __builtin_bswap64(v); }
ALWAYS_INLINE s16 ByteSwap_s16(s16 v) { return (s16)__builtin_bswap16((u16)v); }
ALWAYS_INLINE s32 ByteSwap_s32(s32 v) { return (s32)__builtin_bswap32((u32)v); }
ALWAYS_INLINE s64 ByteSwap_s64(s64 v) { return (s64)__builtin_bswap64((u64)v); }
#else
#  error byteswap requires GCC/clang builtins.
#endif

#define ByteSwap(v)                                                                                                    \
  _Generic((v),                                                                                                        \
           u16: ByteSwap_u16, u32: ByteSwap_u32, u64: ByteSwap_u64,                                                    \
           s16: ByteSwap_s16, s32: ByteSwap_s32, s64: ByteSwap_s64)(v) 

#endif /* CUPID_COMMON_BITUTILS_H */
