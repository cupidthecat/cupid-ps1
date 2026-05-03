/*
 * Original used std::hash<T>{}; in C the caller hashes manually and passes
 * the value in (mixing constant 0x9e3779b9 even for size_t-sized seeds).
 */

#ifndef CUPID_COMMON_HASH_COMBINE_H
#define CUPID_COMMON_HASH_COMBINE_H

#include "types.h"

ALWAYS_INLINE void hash_combine_u64(u64* seed, u64 value)
{
  *seed ^= value + 0x9e3779b9ull + (*seed << 6) + (*seed >> 2);
}

ALWAYS_INLINE void hash_combine_u32(u32* seed, u32 value)
{
  *seed ^= value + 0x9e3779b9u + (*seed << 6) + (*seed >> 2);
}

#endif /* CUPID_COMMON_HASH_COMBINE_H */
