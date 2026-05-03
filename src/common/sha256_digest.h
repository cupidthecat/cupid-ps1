/*
 *

 *   class SHA256Digest -> struct sha256_digest_t
 *   ctor / Reset()     -> sha256_digest_init / sha256_digest_reset
 *   Update()           -> sha256_digest_update
 *   Final()            -> sha256_digest_final (writes 32 bytes)
 *   GetDigest()        -> sha256_digest_compute (one-shot)
 *   DigestToString()   -> sha256_digest_to_string (64 hex + NUL)
 */
#ifndef CUPID_COMMON_SHA256_DIGEST_H
#define CUPID_COMMON_SHA256_DIGEST_H

#include "types.h"

#define SHA256_DIGEST_LENGTH 32
#define SHA256_BLOCK_SIZE    64

typedef struct sha256_digest {
  u64 bit_length;
  u32 state[8];
  u32 block_length;
  u8  block[SHA256_BLOCK_SIZE];
} sha256_digest_t;

void sha256_digest_init(sha256_digest_t* ctx);
void sha256_digest_reset(sha256_digest_t* ctx);
void sha256_digest_update(sha256_digest_t* ctx, const u8* data, size_t len);
void sha256_digest_final(sha256_digest_t* ctx, u8 out[SHA256_DIGEST_LENGTH]);

/* One-shot helper. */
void sha256_digest_compute(const u8* data, size_t len, u8 out[SHA256_DIGEST_LENGTH]);

/* Encodes 32-byte digest as 64 lowercase hex chars + NUL terminator. */
void sha256_digest_to_string(const u8 digest[SHA256_DIGEST_LENGTH], char out[65]);

#endif /* CUPID_COMMON_SHA256_DIGEST_H */
