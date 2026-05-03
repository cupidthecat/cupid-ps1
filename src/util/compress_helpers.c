/*
 * Codecs: Deflate / Gzip (zlib), Zstandard (libzstd), XZ (liblzma).
 * Gzip is plain Deflate framed with windowBits=31.
 */

#include "util/compress_helpers.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>
#include <lzma.h>

LOG_CHANNEL(CompressHelpers);

compress_type_t compress_helpers_type_from_path(const char* path)
{
  const char* ext = NULL;
  u32         ext_len = 0;
  path_get_extension_cstr(path, &ext, &ext_len);

  if (ext_len == 2 && (ext[0] == 'g' || ext[0] == 'G') &&
                      (ext[1] == 'z' || ext[1] == 'Z'))
    return COMPRESS_TYPE_GZIP;
  if (ext_len == 3 && (ext[0] == 'z' || ext[0] == 'Z') &&
                      (ext[1] == 's' || ext[1] == 'S') &&
                      (ext[2] == 't' || ext[2] == 'T'))
    return COMPRESS_TYPE_ZSTANDARD;
  if (ext_len == 2 && (ext[0] == 'x' || ext[0] == 'X') &&
                      (ext[1] == 'z' || ext[1] == 'Z'))
    return COMPRESS_TYPE_XZ;

  return COMPRESS_TYPE_UNCOMPRESSED;
}

const char* compress_helpers_zlib_error_to_string(int res)
{
  switch (res) {
    case Z_OK:            return "Z_OK";
    case Z_STREAM_END:    return "Z_STREAM_END";
    case Z_NEED_DICT:     return "Z_NEED_DICT";
    case Z_ERRNO:         return "Z_ERRNO";
    case Z_STREAM_ERROR:  return "Z_STREAM_ERROR";
    case Z_DATA_ERROR:    return "Z_DATA_ERROR";
    case Z_MEM_ERROR:     return "Z_MEM_ERROR";
    case Z_BUF_ERROR:     return "Z_BUF_ERROR";
    case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
    default:              return "Z_UNKNOWN_ERROR";
  }
}

/* zlib windowBits: 15 = zlib (deflate), 31 = gzip (deflate + gzip framing). */
static int zlib_window_bits_for(compress_type_t type)
{
  return (type == COMPRESS_TYPE_GZIP) ? 31 : 15;
}

/* Streaming inflate: counts decompressed bytes.  Handles both zlib and gzip
 * via windowBits selection. */
static bool zlib_get_decompressed_size(compress_type_t type,
                                       const u8* src, size_t src_len,
                                       size_t* out_size, Error* error)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int res = inflateInit2(&zs, zlib_window_bits_for(type));
  if (res != Z_OK) {
    Error_set_string_fmt(error, "inflateInit2() failed: %s (%d)",
                         compress_helpers_zlib_error_to_string(res), res);
    return false;
  }

  u8 temp[1024];
  zs.next_in  = (Bytef*)src;
  zs.avail_in = (uInt)src_len;

  while (zs.avail_in > 0) {
    zs.next_out  = temp;
    zs.avail_out = sizeof(temp);
    res = inflate(&zs, Z_NO_FLUSH);
    if (res == Z_STREAM_END)
      break;
    if (res != Z_OK) {
      Error_set_string_fmt(error, "inflate() failed: %s (%d)",
                           compress_helpers_zlib_error_to_string(res), res);
      inflateEnd(&zs);
      return false;
    }
  }

  *out_size = (size_t)zs.total_out;
  inflateEnd(&zs);
  return true;
}

/* Streaming inflate into a caller-sized dst.  Used for both zlib and gzip
 * paths so windowBits can be selected (uncompress() is hard-coded to zlib). */
static bool zlib_decompress(compress_type_t type,
                            u8* dst, size_t dst_len,
                            const u8* src, size_t src_len, Error* error)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int res = inflateInit2(&zs, zlib_window_bits_for(type));
  if (res != Z_OK) {
    Error_set_string_fmt(error, "inflateInit2() failed: %s (%d)",
                         compress_helpers_zlib_error_to_string(res), res);
    return false;
  }
  zs.next_in   = (Bytef*)src;
  zs.avail_in  = (uInt)src_len;
  zs.next_out  = dst;
  zs.avail_out = (uInt)dst_len;

  res = inflate(&zs, Z_FINISH);
  if (res != Z_STREAM_END) {
    Error_set_string_fmt(error, "inflate() failed: %s (%d)",
                         compress_helpers_zlib_error_to_string(res), res);
    inflateEnd(&zs);
    return false;
  }
  if ((size_t)zs.total_out != dst_len) {
    Error_set_string_fmt(error, "inflate() produced %zu bytes, expected %zu",
                         (size_t)zs.total_out, dst_len);
    inflateEnd(&zs);
    return false;
  }
  inflateEnd(&zs);
  return true;
}

static bool zlib_compress_alloc(compress_type_t type,
                                const u8* src, size_t src_len, int clevel,
                                u8** out_dst, size_t* out_dst_len, Error* error)
{
  if (clevel < 0) clevel = Z_DEFAULT_COMPRESSION;

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int res = deflateInit2(&zs, clevel, Z_DEFLATED,
                         zlib_window_bits_for(type), 8, Z_DEFAULT_STRATEGY);
  if (res != Z_OK) {
    Error_set_string_fmt(error, "deflateInit2() failed: %s (%d)",
                         compress_helpers_zlib_error_to_string(res), res);
    return false;
  }

  uLong  bound = deflateBound(&zs, (uLong)src_len);
  u8*    buf   = (u8*)malloc(bound > 0 ? bound : 1);
  if (!buf) {
    Error_set_string(error, "out of memory in zlib_compress_alloc");
    deflateEnd(&zs);
    return false;
  }

  zs.next_in   = (Bytef*)src;
  zs.avail_in  = (uInt)src_len;
  zs.next_out  = buf;
  zs.avail_out = (uInt)bound;
  res = deflate(&zs, Z_FINISH);
  if (res != Z_STREAM_END) {
    Error_set_string_fmt(error, "deflate() failed: %s (%d)",
                         compress_helpers_zlib_error_to_string(res), res);
    free(buf);
    deflateEnd(&zs);
    return false;
  }
  const size_t out_len = (size_t)zs.total_out;
  deflateEnd(&zs);

  u8* tight = (u8*)realloc(buf, out_len > 0 ? out_len : 1);
  *out_dst     = tight ? tight : buf;
  *out_dst_len = out_len;
  return true;
}

static bool zstd_get_decompressed_size(const u8* src, size_t src_len,
                                       size_t* out_size, Error* error)
{
  const unsigned long long fcs = ZSTD_getFrameContentSize(src, src_len);
  if (fcs == ZSTD_CONTENTSIZE_ERROR) {
    Error_set_string(error, "ZSTD_getFrameContentSize: not a valid zstd frame");
    return false;
  }
  if (fcs == ZSTD_CONTENTSIZE_UNKNOWN) {
    Error_set_string(error, "zstd frame has unknown decompressed size (streaming-only)");
    return false;
  }
  *out_size = (size_t)fcs;
  return true;
}

static bool zstd_decompress(u8* dst, size_t dst_len,
                            const u8* src, size_t src_len, Error* error)
{
  const size_t r = ZSTD_decompress(dst, dst_len, src, src_len);
  if (ZSTD_isError(r)) {
    Error_set_string_fmt(error, "ZSTD_decompress: %s", ZSTD_getErrorName(r));
    return false;
  }
  if (r != dst_len) {
    Error_set_string_fmt(error, "zstd decompress size mismatch: got %zu, expected %zu", r, dst_len);
    return false;
  }
  return true;
}

static bool zstd_compress_alloc(const u8* src, size_t src_len, int clevel,
                                u8** out_dst, size_t* out_dst_len, Error* error)
{
  const size_t bound = ZSTD_compressBound(src_len);
  u8* buf = (u8*)malloc(bound > 0 ? bound : 1);
  if (!buf) {
    Error_set_string(error, "out of memory in zstd_compress_alloc");
    return false;
  }
  /* clevel < 0 selects ZSTD's default level (zero is also default). */
  const int level = (clevel < 0) ? 0 : clevel;
  const size_t r = ZSTD_compress(buf, bound, src, src_len, level);
  if (ZSTD_isError(r)) {
    Error_set_string_fmt(error, "ZSTD_compress: %s", ZSTD_getErrorName(r));
    free(buf);
    return false;
  }
  u8* tight = (u8*)realloc(buf, r > 0 ? r : 1);
  *out_dst     = tight ? tight : buf;
  *out_dst_len = r;
  return true;
}

/* XZ via liblzma single-shot buffer encode/decode.  Default preset 6 (level
 * 6) matches `xz` CLI default; clevel 0..9 maps directly. */
static bool xz_get_decompressed_size(const u8* src, size_t src_len,
                                     size_t* out_size, Error* error)
{
  /* lzma_stream_buffer_decode insists on knowing dst size up front, so we
   * decode-into-grow until success.  Save-state inputs are bounded by
   * SAVE_STATE_MAX_SAVE_STATE_SIZE so this is fine. */
  size_t cap = src_len * 4 + 4096;
  for (int attempt = 0; attempt < 20; attempt++) {
    u8* dst = (u8*)malloc(cap);
    if (!dst) { Error_set_string(error, "oom in xz_get_decompressed_size"); return false; }
    size_t in_pos = 0, out_pos = 0;
    uint64_t memlimit = UINT64_MAX;
    lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, NULL,
                                           src, &in_pos, src_len,
                                           dst, &out_pos, cap);
    if (r == LZMA_OK) {
      *out_size = out_pos;
      free(dst);
      return true;
    }
    free(dst);
    if (r == LZMA_BUF_ERROR) { cap *= 2; continue; }
    Error_set_string_fmt(error, "lzma_stream_buffer_decode: %d", (int)r);
    return false;
  }
  Error_set_string(error, "xz: decompressed size grew past 20 doublings");
  return false;
}

static bool xz_decompress(u8* dst, size_t dst_len,
                          const u8* src, size_t src_len, Error* error)
{
  size_t in_pos = 0, out_pos = 0;
  uint64_t memlimit = UINT64_MAX;
  lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, NULL,
                                         src, &in_pos, src_len,
                                         dst, &out_pos, dst_len);
  if (r != LZMA_OK) {
    Error_set_string_fmt(error, "lzma_stream_buffer_decode: %d", (int)r);
    return false;
  }
  if (out_pos != dst_len) {
    Error_set_string_fmt(error, "xz decompress size mismatch: got %zu, expected %zu",
                         out_pos, dst_len);
    return false;
  }
  return true;
}

static bool xz_compress_alloc(const u8* src, size_t src_len, int clevel,
                              u8** out_dst, size_t* out_dst_len, Error* error)
{
  const uint32_t preset = (clevel < 0) ? 6u : (uint32_t)(clevel > 9 ? 9 : clevel);
  const size_t bound = lzma_stream_buffer_bound(src_len);
  u8* buf = (u8*)malloc(bound > 0 ? bound : 1);
  if (!buf) {
    Error_set_string(error, "out of memory in xz_compress_alloc");
    return false;
  }
  size_t out_pos = 0;
  lzma_ret r = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL,
                                       src, src_len,
                                       buf, &out_pos, bound);
  if (r != LZMA_OK) {
    Error_set_string_fmt(error, "lzma_easy_buffer_encode: %d", (int)r);
    free(buf);
    return false;
  }
  u8* tight = (u8*)realloc(buf, out_pos > 0 ? out_pos : 1);
  *out_dst     = tight ? tight : buf;
  *out_dst_len = out_pos;
  return true;
}

bool compress_helpers_get_decompressed_size(compress_type_t type,
                                            const u8* src, size_t src_len,
                                            size_t* out_size, Error* error)
{
  switch (type) {
    case COMPRESS_TYPE_UNCOMPRESSED:
      *out_size = src_len;
      return true;
    case COMPRESS_TYPE_DEFLATE:
    case COMPRESS_TYPE_GZIP:
      return zlib_get_decompressed_size(type, src, src_len, out_size, error);
    case COMPRESS_TYPE_ZSTANDARD:
      return zstd_get_decompressed_size(src, src_len, out_size, error);
    case COMPRESS_TYPE_XZ:
      return xz_get_decompressed_size(src, src_len, out_size, error);
  }
  Error_set_string(error, "unknown compression type");
  return false;
}

bool compress_helpers_decompress_buffer(compress_type_t type,
                                        u8* dst, size_t dst_len,
                                        const u8* src, size_t src_len,
                                        Error* error)
{
  switch (type) {
    case COMPRESS_TYPE_UNCOMPRESSED:
      if (src_len != dst_len) {
        Error_set_string_fmt(error, "uncompressed copy size mismatch: src=%zu dst=%zu",
                             src_len, dst_len);
        return false;
      }
      memcpy(dst, src, src_len);
      return true;
    case COMPRESS_TYPE_DEFLATE:
    case COMPRESS_TYPE_GZIP:
      return zlib_decompress(type, dst, dst_len, src, src_len, error);
    case COMPRESS_TYPE_ZSTANDARD:
      return zstd_decompress(dst, dst_len, src, src_len, error);
    case COMPRESS_TYPE_XZ:
      return xz_decompress(dst, dst_len, src, src_len, error);
  }
  Error_set_string(error, "unknown compression type");
  return false;
}

bool compress_helpers_decompress_alloc(compress_type_t type,
                                       const u8* src, size_t src_len,
                                       size_t decompressed_size_hint,
                                       u8** out_dst, size_t* out_dst_len,
                                       Error* error)
{
  size_t want = decompressed_size_hint;
  if (want == 0) {
    if (!compress_helpers_get_decompressed_size(type, src, src_len, &want, error))
      return false;
  }

  u8* dst = (u8*)malloc(want > 0 ? want : 1);
  if (!dst) {
    Error_set_string(error, "out of memory in decompress_alloc");
    return false;
  }
  if (!compress_helpers_decompress_buffer(type, dst, want, src, src_len, error)) {
    free(dst);
    return false;
  }

  *out_dst     = dst;
  *out_dst_len = want;
  return true;
}

bool compress_helpers_compress_alloc(compress_type_t type,
                                     const u8* src, size_t src_len,
                                     int clevel,
                                     u8** out_dst, size_t* out_dst_len,
                                     Error* error)
{
  switch (type) {
    case COMPRESS_TYPE_UNCOMPRESSED: {
      u8* buf = (u8*)malloc(src_len > 0 ? src_len : 1);
      if (!buf) {
        Error_set_string(error, "out of memory in compress_alloc");
        return false;
      }
      memcpy(buf, src, src_len);
      *out_dst     = buf;
      *out_dst_len = src_len;
      return true;
    }
    case COMPRESS_TYPE_DEFLATE:
    case COMPRESS_TYPE_GZIP:
      return zlib_compress_alloc(type, src, src_len, clevel, out_dst, out_dst_len, error);
    case COMPRESS_TYPE_ZSTANDARD:
      return zstd_compress_alloc(src, src_len, clevel, out_dst, out_dst_len, error);
    case COMPRESS_TYPE_XZ:
      return xz_compress_alloc(src, src_len, clevel, out_dst, out_dst_len, error);
  }
  Error_set_string(error, "unknown compression type");
  return false;
}

bool compress_helpers_compress_to_file(const char* path,
                                       const u8* src, size_t src_len,
                                       int clevel, bool atomic,
                                       Error* error)
{
  /* Atomic write would need fs_open_temporary_file + rename.  Keep the API
   * surface but treat `atomic` as a hint; current path is the simple
   * write-then-close.  TODO once a consumer needs the atomicity guarantee. */
  (void)atomic;

  const compress_type_t type = compress_helpers_type_from_path(path);
  u8*    cbuf  = NULL;
  size_t clen  = 0;
  if (!compress_helpers_compress_alloc(type, src, src_len, clevel, &cbuf, &clen, error))
    return false;

  FILE* fp = fs_open_file(path, "wb", error);
  if (!fp) {
    free(cbuf);
    return false;
  }
  const size_t written = fwrite(cbuf, 1, clen, fp);
  free(cbuf);
  fclose(fp);

  if (written != clen) {
    Error_set_string_fmt(error, "short write to '%s': %zu/%zu", path, written, clen);
    return false;
  }
  return true;
}
