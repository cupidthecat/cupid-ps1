/*
 *

 * Mostly based on this public-domain implementation:
 *   https://gist.github.com/jrabbit/1042021
 */
#include "sha1_digest.h"

#include "assert.h"

#include <stdint.h>
#include <string.h>

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() / blk(): byte-swap the 32-bit word on little-endian hosts (this is the
 * "expand during the round function" trick from SSLeay). */
#define blk0(i) (block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00) | (rol(block->l[i], 8) & 0x00FF00FF))
#define blk(i) \
  (block->l[i & 15] = \
     rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1))

/* (R0+R1), R2, R3, R4 are the per-stage SHA-1 mixing operations. */
#define R0(v, w, x, y, z, i) \
  (z) += ((w & (x ^ y)) ^ y) + blk0(i) + 0x5A827999 + rol(v, 5); \
  (w)  = rol(w, 30);
#define R1(v, w, x, y, z, i) \
  (z) += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5); \
  (w)  = rol(w, 30);
#define R2(v, w, x, y, z, i) \
  (z) += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5); \
  (w)  = rol(w, 30);
#define R3(v, w, x, y, z, i) \
  (z) += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5); \
  (w)  = rol(w, 30);
#define R4(v, w, x, y, z, i) \
  (z) += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5); \
  (w)  = rol(w, 30);

/* Hash a single 512-bit block. This is the core of the algorithm. */
static void sha1_transform(u32 state[5], const u8 buffer[64])
{
  /* The union here lets the round macros index 32-bit words while we feed in raw bytes. */
  typedef union {
    u8  c[64];
    u32 l[16];
  } CHAR64LONG16;

  CHAR64LONG16 block[1]; /* array form so `block->l` works like in the original */
  memcpy(block, buffer, 64);

  u32 a = state[0];
  u32 b = state[1];
  u32 c = state[2];
  u32 d = state[3];
  u32 e = state[4];

  /* 4 rounds of 20 operations each. Loop unrolled. */
  R0(a, b, c, d, e, 0);
  R0(e, a, b, c, d, 1);
  R0(d, e, a, b, c, 2);
  R0(c, d, e, a, b, 3);
  R0(b, c, d, e, a, 4);
  R0(a, b, c, d, e, 5);
  R0(e, a, b, c, d, 6);
  R0(d, e, a, b, c, 7);
  R0(c, d, e, a, b, 8);
  R0(b, c, d, e, a, 9);
  R0(a, b, c, d, e, 10);
  R0(e, a, b, c, d, 11);
  R0(d, e, a, b, c, 12);
  R0(c, d, e, a, b, 13);
  R0(b, c, d, e, a, 14);
  R0(a, b, c, d, e, 15);
  R1(e, a, b, c, d, 16);
  R1(d, e, a, b, c, 17);
  R1(c, d, e, a, b, 18);
  R1(b, c, d, e, a, 19);
  R2(a, b, c, d, e, 20);
  R2(e, a, b, c, d, 21);
  R2(d, e, a, b, c, 22);
  R2(c, d, e, a, b, 23);
  R2(b, c, d, e, a, 24);
  R2(a, b, c, d, e, 25);
  R2(e, a, b, c, d, 26);
  R2(d, e, a, b, c, 27);
  R2(c, d, e, a, b, 28);
  R2(b, c, d, e, a, 29);
  R2(a, b, c, d, e, 30);
  R2(e, a, b, c, d, 31);
  R2(d, e, a, b, c, 32);
  R2(c, d, e, a, b, 33);
  R2(b, c, d, e, a, 34);
  R2(a, b, c, d, e, 35);
  R2(e, a, b, c, d, 36);
  R2(d, e, a, b, c, 37);
  R2(c, d, e, a, b, 38);
  R2(b, c, d, e, a, 39);
  R3(a, b, c, d, e, 40);
  R3(e, a, b, c, d, 41);
  R3(d, e, a, b, c, 42);
  R3(c, d, e, a, b, 43);
  R3(b, c, d, e, a, 44);
  R3(a, b, c, d, e, 45);
  R3(e, a, b, c, d, 46);
  R3(d, e, a, b, c, 47);
  R3(c, d, e, a, b, 48);
  R3(b, c, d, e, a, 49);
  R3(a, b, c, d, e, 50);
  R3(e, a, b, c, d, 51);
  R3(d, e, a, b, c, 52);
  R3(c, d, e, a, b, 53);
  R3(b, c, d, e, a, 54);
  R3(a, b, c, d, e, 55);
  R3(e, a, b, c, d, 56);
  R3(d, e, a, b, c, 57);
  R3(c, d, e, a, b, 58);
  R3(b, c, d, e, a, 59);
  R4(a, b, c, d, e, 60);
  R4(e, a, b, c, d, 61);
  R4(d, e, a, b, c, 62);
  R4(c, d, e, a, b, 63);
  R4(b, c, d, e, a, 64);
  R4(a, b, c, d, e, 65);
  R4(e, a, b, c, d, 66);
  R4(d, e, a, b, c, 67);
  R4(c, d, e, a, b, 68);
  R4(b, c, d, e, a, 69);
  R4(a, b, c, d, e, 70);
  R4(e, a, b, c, d, 71);
  R4(d, e, a, b, c, 72);
  R4(c, d, e, a, b, 73);
  R4(b, c, d, e, a, 74);
  R4(a, b, c, d, e, 75);
  R4(e, a, b, c, d, 76);
  R4(d, e, a, b, c, 77);
  R4(c, d, e, a, b, 78);
  R4(b, c, d, e, a, 79);

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void sha1_digest_init(sha1_digest_t* ctx)
{
  sha1_digest_reset(ctx);
}

void sha1_digest_reset(sha1_digest_t* ctx)
{
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count[0] = 0;
  ctx->count[1] = 0;
}

void sha1_digest_update(sha1_digest_t* ctx, const u8* data, size_t len)
{
  /* Implementation tracks bit count in two u32s; cap each call at u32 range to
   * preserve the original semantics. */
  Assert(len <= UINT32_MAX);
  const u32 ulen = (u32)len;

  u32 i;
  u32 j = ctx->count[0];
  if ((ctx->count[0] += ulen << 3) < j)
    ctx->count[1]++;
  ctx->count[1] += (ulen >> 29);
  j = (j >> 3) & 63;

  if ((j + ulen) > 63)
  {
    memcpy(&ctx->buffer[j], data, (i = 64 - j));
    sha1_transform(ctx->state, ctx->buffer);
    for (; i + 63 < ulen; i += 64)
      sha1_transform(ctx->state, &data[i]);
    j = 0;
  }
  else
  {
    i = 0;
  }
  memcpy(&ctx->buffer[j], &data[i], ulen - i);
}

void sha1_digest_final(sha1_digest_t* ctx, u8 out[SHA1_DIGEST_LENGTH])
{
  u8 finalcount[8];

  /* Big-endian length-in-bits so it's host endianness independent. */
  for (u32 i = 0; i < 8; i++)
    finalcount[i] = (u8)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);

  u8 c = 0x80;
  sha1_digest_update(ctx, &c, 1);
  while ((ctx->count[0] & 504) != 448)
  {
    c = 0x00;
    sha1_digest_update(ctx, &c, 1);
  }
  sha1_digest_update(ctx, finalcount, 8); /* triggers the final transform */

  for (u32 i = 0; i < SHA1_DIGEST_LENGTH; i++)
    out[i] = (u8)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
}

void sha1_digest_compute(const u8* data, size_t len, u8 out[SHA1_DIGEST_LENGTH])
{
  sha1_digest_t ctx;
  sha1_digest_init(&ctx);
  sha1_digest_update(&ctx, data, len);
  sha1_digest_final(&ctx, out);
}

void sha1_digest_to_string(const u8 digest[SHA1_DIGEST_LENGTH], char out[41])
{
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < SHA1_DIGEST_LENGTH; ++i)
  {
    out[i * 2]     = hex[(digest[i] >> 4) & 0xf];
    out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  out[40] = '\0';
}
