/*
 * Compression helpers.  Codecs: Deflate (zlib), Gzip (zlib + windowBits 31),
 * Zstandard (libzstd), XZ (liblzma).  Save-state path picks the codec from
 * the file extension; the on-disk save-state header may also embed a
 * decoded inner type (NONE/DEFLATE/ZSTANDARD/XZ) per save_state_version.h.
 */

#ifndef CUPID_UTIL_COMPRESS_HELPERS_H
#define CUPID_UTIL_COMPRESS_HELPERS_H

#include "common/error.h"
#include "common/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  COMPRESS_TYPE_UNCOMPRESSED = 0,
  COMPRESS_TYPE_DEFLATE      = 1,
  COMPRESS_TYPE_ZSTANDARD    = 2,
  COMPRESS_TYPE_XZ           = 3,
  COMPRESS_TYPE_GZIP         = 4,  /* zlib deflate with windowBits=31 (gzip framing) */
} compress_type_t;

/* Detect compression type from file extension.  Returns
 * COMPRESS_TYPE_UNCOMPRESSED for unknown extensions. */
compress_type_t compress_helpers_type_from_path(const char* path);

const char*     compress_helpers_zlib_error_to_string(int res);

/* Determine the decompressed size of an in-memory blob.  Streams the data
 * through the codec to count.  Returns false + sets `error` on failure. */
bool compress_helpers_get_decompressed_size(compress_type_t type,
                                            const u8* src, size_t src_len,
                                            size_t* out_size, Error* error);

/* Decompress src into a caller-supplied dst.  dst_len must equal the
 * decompressed payload size.  Returns false on codec error. */
bool compress_helpers_decompress_buffer(compress_type_t type,
                                        u8* dst, size_t dst_len,
                                        const u8* src, size_t src_len,
                                        Error* error);

/* Decompress src into a freshly-malloc'd buffer.  Caller frees with free().
 * If `decompressed_size_hint` is non-zero, allocates exactly that; else
 * inflates twice (once to size, once to fill). */
bool compress_helpers_decompress_alloc(compress_type_t type,
                                       const u8* src, size_t src_len,
                                       size_t decompressed_size_hint,
                                       u8** out_dst, size_t* out_dst_len,
                                       Error* error);

/* Compress src into a freshly-malloc'd buffer.  Caller frees with free().
 * `clevel < 0` selects the codec default. */
bool compress_helpers_compress_alloc(compress_type_t type,
                                     const u8* src, size_t src_len,
                                     int clevel,
                                     u8** out_dst, size_t* out_dst_len,
                                     Error* error);

/* Compress src to file `path`.  When `atomic`, writes to "<path>.tmp" then
 * renames over.  Type derived from path extension. */
bool compress_helpers_compress_to_file(const char* path,
                                       const u8* src, size_t src_len,
                                       int clevel, bool atomic,
                                       Error* error);

#ifdef __cplusplus
}
#endif

#endif
