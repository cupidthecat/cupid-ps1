#ifndef CUPID_COMMON_BCDUTILS_H
#define CUPID_COMMON_BCDUTILS_H

#include "types.h"

ALWAYS_INLINE u8 BinaryToBCD(u8 v)        { return (u8)(((v / 10u) << 4) + (v % 10u)); }
ALWAYS_INLINE u8 PackedBCDToBinary(u8 v)  { return (u8)(((v >> 4) * 10u) + (v % 16u)); }
ALWAYS_INLINE bool IsValidBCDDigit(u8 d)  { return d <= 9u; }
ALWAYS_INLINE bool IsValidPackedBCD(u8 v) { return IsValidBCDDigit(v & 0x0Fu) && IsValidBCDDigit(v >> 4); }

#endif /* CUPID_COMMON_BCDUTILS_H */
