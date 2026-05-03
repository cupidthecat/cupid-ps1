/*
 * Keyed object store backed by an .idx + .bin file pair.  Used as a shader
 * cache.  cupid-ps1 has no consumer yet so this lives as the API surface, not
 * wired into anything.
 *
 * Notes:
 *   - Keys stored inline (malloc'd char*) per entry rather than in a
 *     BumpStringPool.  Slower for huge caches but BumpStringPool isn't
 *     ported yet.  Swap in later if needed.
 *   - Index linear array; lookups are binary search.
 *   - Compression: only `COMPRESS_TYPE_UNCOMPRESSED` and
 *     `COMPRESS_TYPE_DEFLATE` (the rest of the compress types are
 *     deferred - see compress_helpers.h).
 */

#ifndef CUPID_UTIL_OBJECT_ARCHIVE_H
#define CUPID_UTIL_OBJECT_ARCHIVE_H

#include "common/error.h"
#include "common/types.h"
#include "util/compress_helpers.h"

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char OBJECT_ARCHIVE_ERROR_DOES_NOT_EXIST[];
extern const char OBJECT_ARCHIVE_ERROR_ALREADY_EXISTS[];

typedef struct {
  u32             file_offset;
  u32             compressed_size;
  u32             uncompressed_size;
  u32             key_size;
  compress_type_t compress_type;
  char*           key;             /* malloc'd, len key_size + 1 (NUL) */
} object_archive_entry_t;

typedef struct {
  FILE*                   index_file;
  FILE*                   blob_file;
  object_archive_entry_t* entries;       /* sorted by key */
  u32                     entry_count;
  u32                     entry_capacity;
} object_archive_t;

void object_archive_init   (object_archive_t* a);
void object_archive_destroy(object_archive_t* a);
void object_archive_close  (object_archive_t* a);

bool object_archive_is_open(const object_archive_t* a);
u32  object_archive_size   (const object_archive_t* a);

 /* Open/create an archive at base_path → opens "base_path.idx" + ".bin".
 * If they exist + version matches, opens existing.  Otherwise creates new. */
bool object_archive_open_path  (object_archive_t* a, const char* base_path,
                                u32 data_version, Error* error);

 /* Take ownership of pre-opened files (caller transfers ownership; archive
 * fcloses on close). */
bool object_archive_open_files (object_archive_t* a, FILE* index_file,
                                FILE* blob_file, u32 data_version, Error* error);
bool object_archive_create_files(object_archive_t* a, FILE* index_file,
                                 FILE* blob_file, u32 data_version, Error* error);

/* Truncate both files; archive remains open and ready for inserts. */
bool object_archive_clear      (object_archive_t* a, Error* error);

 /* Look up an object.  Returns malloc'd buffer + len on success (caller
 * frees with free()).  On miss: sets error to ERROR_DOES_NOT_EXIST. */
bool object_archive_lookup     (object_archive_t* a, const char* key,
                                u8** out_data, size_t* out_len, Error* error);

bool object_archive_contains   (const object_archive_t* a, const char* key);

bool object_archive_insert     (object_archive_t* a, const char* key,
                                const void* data, size_t data_size,
                                compress_type_t compression, Error* error);

u64  object_archive_total_object_size(const object_archive_t* a);
u64  object_archive_total_size       (const object_archive_t* a);

#ifdef __cplusplus
}
#endif

#endif
