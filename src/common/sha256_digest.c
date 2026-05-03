/*
 *

 * Based on https://github.com/B-Con/crypto-algorithms/blob/master/sha256.c
 * by Brad Conte (brad AT bradconte.com).
 */
#include "sha256_digest.h"

#include <string.h>

#define ROTLEFT(a, b)  (((a) << (b)) | ((a) >> (32 - (b))))
#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)       (ROTRIGHT(x, 2)  ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x)       (ROTRIGHT(x, 6)  ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x)      (ROTRIGHT(x, 7)  ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x)      (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

static const u32 k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2, 
};

static void sha256_transform_block(sha256_digest_t* ctx)
{
  u32 m[64];

  /* First 16 message-schedule words come straight from the block (big-endian). */
  size_t i = 0;
  for (size_t j = 0; i < 16; ++i, j += 4)
  {
    m[i] = ((u32)ctx->block[j]     << 24) |
           ((u32)ctx->block[j + 1] << 16) |
           ((u32)ctx->block[j + 2] <<  8) | 
           ((u32)ctx->block[j + 3]);
  }
  /* Remaining 48 words computed via the SIG0/SIG1 expansion. */
  for (; i < 64; ++i)
    m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

  u32 a = ctx->state[0];
  u32 b = ctx->state[1];
  u32 c = ctx->state[2];
  u32 d = ctx->state[3];
  u32 e = ctx->state[4];
  u32 f = ctx->state[5];
  u32 g = ctx->state[6];
  u32 h = ctx->state[7];

  for (i = 0; i < 64; ++i)
  {
    u32 t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
    u32 t2 = EP0(a) + MAJ(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void sha256_digest_init(sha256_digest_t* ctx)
{
  sha256_digest_reset(ctx);
}

void sha256_digest_reset(sha256_digest_t* ctx)
{
  ctx->block_length = 0;
  ctx->bit_length   = 0;
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
  memset(ctx->block, 0, sizeof(ctx->block));
}

void sha256_digest_update(sha256_digest_t* ctx, const u8* data, size_t len)
{
  for (size_t pos = 0; pos < len;)
  {
    const size_t remaining = len - pos;
    const size_t space     = SHA256_BLOCK_SIZE - ctx->block_length;
    const u32    copy_len  = (u32)((remaining < space) ? remaining : space);

    memcpy(&ctx->block[ctx->block_length], &data[pos], copy_len);
    ctx->block_length += copy_len;
    pos += copy_len;

    if (ctx->block_length == SHA256_BLOCK_SIZE)
    {
      sha256_transform_block(ctx);
      ctx->bit_length  += 512;
      ctx->block_length = 0;
    }
  }
}

void sha256_digest_final(sha256_digest_t* ctx, u8 out[SHA256_DIGEST_LENGTH])
{
  /* Append the 0x80 sentinel byte then zero-pad. If there isn't room for the
   * 8-byte length suffix in this block, transform now and start a fresh block. */
  if (ctx->block_length < 56)
  {
    u32 i = ctx->block_length;
    ctx->block[i++] = 0x80;
    while (i < 56)
      ctx->block[i++] = 0x00;
  }
  else
  {
    u32 i = ctx->block_length;
    ctx->block[i++] = 0x80;
    while (i < 64)
      ctx->block[i++] = 0x00;
    sha256_transform_block(ctx);
    memset(ctx->block, 0, sizeof(ctx->block));
  }

  /* Append big-endian 64-bit length-in-bits and run the final transform. */
  ctx->bit_length += (u64)ctx->block_length * 8u;
  ctx->block[63] = (u8)(ctx->bit_length);
  ctx->block[62] = (u8)(ctx->bit_length >> 8);
  ctx->block[61] = (u8)(ctx->bit_length >> 16);
  ctx->block[60] = (u8)(ctx->bit_length >> 24);
  ctx->block[59] = (u8)(ctx->bit_length >> 32);
  ctx->block[58] = (u8)(ctx->bit_length >> 40);
  ctx->block[57] = (u8)(ctx->bit_length >> 48);
  ctx->block[56] = (u8)(ctx->bit_length >> 56);
  sha256_transform_block(ctx);

  /* SHA uses big-endian state words; this implementation keeps them in host
   * (little-endian) order, so reverse bytes when writing the final digest. */
  for (size_t i = 0; i < 4; ++i)
  {
    out[i]      = (u8)((ctx->state[0] >> (24 - i * 8)) & 0xff);
    out[i + 4]  = (u8)((ctx->state[1] >> (24 - i * 8)) & 0xff);
    out[i + 8]  = (u8)((ctx->state[2] >> (24 - i * 8)) & 0xff);
    out[i + 12] = (u8)((ctx->state[3] >> (24 - i * 8)) & 0xff);
    out[i + 16] = (u8)((ctx->state[4] >> (24 - i * 8)) & 0xff);
    out[i + 20] = (u8)((ctx->state[5] >> (24 - i * 8)) & 0xff);
    out[i + 24] = (u8)((ctx->state[6] >> (24 - i * 8)) & 0xff);
    out[i + 28] = (u8)((ctx->state[7] >> (24 - i * 8)) & 0xff);
  }
}

void sha256_digest_compute(const u8* data, size_t len, u8 out[SHA256_DIGEST_LENGTH])
{
  sha256_digest_t ctx;
  sha256_digest_init(&ctx);
  sha256_digest_update(&ctx, data, len);
  sha256_digest_final(&ctx, out);
}

void sha256_digest_to_string(const u8 digest[SHA256_DIGEST_LENGTH], char out[65])
{
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i)
  {
    out[i * 2]     = hex[(digest[i] >> 4) & 0xf];
    out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  out[64] = '\0';
}
