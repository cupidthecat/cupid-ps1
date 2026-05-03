/*
 *

 *   class MD5Digest -> struct md5_digest_t
 *   ctor / Reset()  -> md5_digest_init / md5_digest_reset
 *   Update()        -> md5_digest_update
 *   Final()         -> md5_digest_final (writes 16 bytes)
 *   HashData()      -> md5_digest_compute (one-shot)
 */
#ifndef CUPID_COMMON_MD5_DIGEST_H
#define CUPID_COMMON_MD5_DIGEST_H

#include "types.h"

#define MD5_DIGEST_LENGTH 16

typedef struct md5_digest {
  u32 buf[4];
  u32 bits[2];
  u8  in[64];
} md5_digest_t;

void md5_digest_init(md5_digest_t* ctx);
void md5_digest_reset(md5_digest_t* ctx);
void md5_digest_update(md5_digest_t* ctx, const u8* data, size_t len);
void md5_digest_final(md5_digest_t* ctx, u8 out[MD5_DIGEST_LENGTH]);

/* One-shot helper. */
void md5_digest_compute(const u8* data, size_t len, u8 out[MD5_DIGEST_LENGTH]);

/* Encodes 16-byte digest as 32 lowercase hex chars + NUL terminator. */
void md5_digest_to_string(const u8 digest[MD5_DIGEST_LENGTH], char out[33]);

#endif /* CUPID_COMMON_MD5_DIGEST_H */
