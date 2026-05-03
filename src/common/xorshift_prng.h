#ifndef CUPID_COMMON_XORSHIFT_PRNG_H
#define CUPID_COMMON_XORSHIFT_PRNG_H

#include "types.h"

typedef struct {
  u64 s0;
  u64 s1;
} xorshift128pp_state_t;

typedef struct {
  xorshift128pp_state_t state;
} xorshift128pp_t;

ALWAYS_INLINE u64 xorshift_splitmix64(u64* x)
{
  u64 z = (*x += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

ALWAYS_INLINE u64 xorshift_rotate_left64(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

ALWAYS_INLINE xorshift128pp_state_t xorshift128pp_initial_state(u64 seed)
{
  xorshift128pp_state_t s;
  s.s0 = xorshift_splitmix64(&seed);
  s.s1 = xorshift_splitmix64(&seed);
  return s;
}

ALWAYS_INLINE void xorshift128pp_init(xorshift128pp_t* p)         { p->state = xorshift128pp_initial_state(0); }
ALWAYS_INLINE void xorshift128pp_seed(xorshift128pp_t* p, u64 sd) { p->state = xorshift128pp_initial_state(sd); }
ALWAYS_INLINE void xorshift128pp_set_state(xorshift128pp_t* p, const xorshift128pp_state_t* s) { p->state = *s; }

ALWAYS_INLINE u64 xorshift128pp_next(xorshift128pp_t* p)
{
  /* https://xoroshiro.di.unimi.it/xoroshiro128plusplus.c */
  u64 s0 = p->state.s0;
  u64 s1 = p->state.s1;
  u64 result = xorshift_rotate_left64(s0 + s1, 17) + s0;
  s1 ^= s0;
  p->state.s0 = xorshift_rotate_left64(s0, 49) ^ s1 ^ (s1 << 21);
  p->state.s1 = xorshift_rotate_left64(s1, 28);
  return result;
}

ALWAYS_INLINE u64 xorshift128pp_next_range(xorshift128pp_t* p, u64 n)
{
  const u64 max_allowed = (UINT64_C(0xFFFFFFFFFFFFFFFF) / n) * n;
  for (;;) {
    const u64 x = xorshift128pp_next(p);
    if (x > max_allowed) continue;
    return x % n;
  }
}

#endif /* CUPID_COMMON_XORSHIFT_PRNG_H */
