/*
 *

 * MD5 hash of an entire CD image (or single track) for game-database lookup.
 */
#ifndef CUPID_UTIL_CD_IMAGE_HASHER_H
#define CUPID_UTIL_CD_IMAGE_HASHER_H

#include "common/types.h"

#define CD_IMAGE_HASH_SIZE 16

typedef struct cd_image            cd_image_t;
typedef struct Error               Error;
typedef struct progress_callback   progress_callback_t;
typedef struct small_string        small_string_t;

/* Decode 32-char hex string into a 16-byte hash.  Returns true on success. */
bool cd_image_hash_from_string(const char* str, u32 len, u8 out_hash[CD_IMAGE_HASH_SIZE]);
/* Format 16-byte hash as 32 lowercase hex chars + NUL into out (33 bytes). */
void cd_image_hash_to_string(const u8 hash[CD_IMAGE_HASH_SIZE], char out[33]);

bool cd_image_hasher_get_image_hash(cd_image_t* image, u8 out_hash[CD_IMAGE_HASH_SIZE],
                                    progress_callback_t* pc, Error* err);
bool cd_image_hasher_get_track_hash(cd_image_t* image, u8 track, u8 out_hash[CD_IMAGE_HASH_SIZE],
                                    progress_callback_t* pc, Error* err);

#endif /* CUPID_UTIL_CD_IMAGE_HASHER_H */
