/*
 *

 *   class SHA1Digest -> struct sha1_digest_t
 *   ctor / Reset()   -> sha1_digest_init / sha1_digest_reset
 *   Update()         -> sha1_digest_update
 *   Final()          -> sha1_digest_final (writes 20 bytes)
 *   GetDigest()      -> sha1_digest_compute (one-shot)
 *   DigestToString() -> sha1_digest_to_string (40 hex + NUL)
 */
#ifndef CUPID_COMMON_SHA1_DIGEST_H
#define CUPID_COMMON_SHA1_DIGEST_H

#include "types.h"

#define SHA1_DIGEST_LENGTH 20

typedef struct sha1_digest {
  u32 state[5];
  u32 count[2];
  u8  buffer[64];
} sha1_digest_t;

void sha1_digest_init(sha1_digest_t* ctx);
void sha1_digest_reset(sha1_digest_t* ctx);
void sha1_digest_update(sha1_digest_t* ctx, const u8* data, size_t len);
void sha1_digest_final(sha1_digest_t* ctx, u8 out[SHA1_DIGEST_LENGTH]);

/* One-shot helper. */
void sha1_digest_compute(const u8* data, size_t len, u8 out[SHA1_DIGEST_LENGTH]);

/* Encodes 20-byte digest as 40 lowercase hex chars + NUL terminator. */
void sha1_digest_to_string(const u8 digest[SHA1_DIGEST_LENGTH], char out[41]);

#endif /* CUPID_COMMON_SHA1_DIGEST_H */
