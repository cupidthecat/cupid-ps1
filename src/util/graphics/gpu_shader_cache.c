/*
 * Notes:
 *  - Blobs zstd-compressed in Insert and decompressed in Lookup (matches
 *    cache files written before zstd was wired: those have
 *    compressed_size == uncompressed_size and load as raw.  Header signature
 *    unchanged so existing on-disk caches stay valid.
 *  - std::vector<CacheIndexEntry> → flat array + bsearch + insertion sort on
 *    insert (rare path).
 *  - fmt::format("{}.idx", base) → snprintf.
 *  - GPUShader::GetStageName for log msgs → local stage_name() helper since
 *    gpu_device hasn't been ported yet.
 */

#include "gpu_shader_cache.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

LOG_CHANNEL(GPUDevice);

typedef struct {
  u32 signature;
  u32 render_api_version;
  u32 cache_version;
} cache_file_header_t;

#define EXPECTED_SIGNATURE 0x434B5544u   /* "DUKC" */
#define KEY_COMPARE_SIZE   (offsetof(gpu_shader_cache_index_key_t, unused))

_Static_assert(sizeof(cache_file_header_t) == 12, "cache file header has no padding");
_Static_assert(sizeof(gpu_shader_cache_index_key_t) == 48, "index key has no padding");
_Static_assert(sizeof(gpu_shader_cache_index_entry_t) == 56, "index entry has no padding");

static const char* stage_name(gpu_shader_stage_t s)
{
  switch (s)
  {
    case GPU_SHADER_STAGE_VERTEX:   return "Vertex";
    case GPU_SHADER_STAGE_FRAGMENT: return "Fragment";
    case GPU_SHADER_STAGE_GEOMETRY: return "Geometry";
    case GPU_SHADER_STAGE_COMPUTE:  return "Compute";
    default:                        return "?";
  }
}

static int compare_keys(const void* a_, const void* b_)
{
  /* both ptrs reference structs with matching prefix layout (key + entry). */
  return memcmp(a_, b_, KEY_COMPARE_SIZE);
}

static void filename_concat(char* dst, size_t dst_size, const char* base, const char* suffix)
{
  snprintf(dst, dst_size, "%s%s", base, suffix);
}

static const char* basename_only(const char* path)
{
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

void gpu_shader_cache_init(gpu_shader_cache_t* c)
{
  memset(c, 0, sizeof(*c));
}

void gpu_shader_cache_destroy(gpu_shader_cache_t* c)
{
  gpu_shader_cache_close(c);
  free(c->entries);
  free(c->base_filename);
  memset(c, 0, sizeof(*c));
}

bool gpu_shader_cache_is_open(const gpu_shader_cache_t* c)
{
  return c->index_file != NULL;
}

static bool create_new(gpu_shader_cache_t* c, const char* index_filename, const char* blob_filename)
{
  if (fs_file_exists(index_filename))
  {
    WARNING_LOG("Removing existing index file '%s'", basename_only(index_filename));
    fs_delete_file(index_filename, NULL);
  }
  if (fs_file_exists(blob_filename))
  {
    WARNING_LOG("Removing existing blob file '%s'", basename_only(blob_filename));
    fs_delete_file(blob_filename, NULL);
  }

  c->index_file = fs_open_file(index_filename, "wb", NULL);
  if (!c->index_file)
  {
    ERROR_LOG("Failed to open index file '%s' for writing", basename_only(index_filename));
    return false;
  }

  const cache_file_header_t hdr = {
    .signature = EXPECTED_SIGNATURE,
    .render_api_version = c->render_api_version,
    .cache_version = c->version
  };
  if (fwrite(&hdr, sizeof(hdr), 1, c->index_file) != 1)
  {
    ERROR_LOG("Failed to write version to index file '%s'", basename_only(index_filename));
    fclose(c->index_file); c->index_file = NULL;
    fs_delete_file(index_filename, NULL);
    return false;
  }

  c->blob_file = fs_open_file(blob_filename, "w+b", NULL);
  if (!c->blob_file)
  {
    ERROR_LOG("Failed to open blob file '%s' for writing", basename_only(blob_filename));
    fclose(c->index_file); c->index_file = NULL;
    fs_delete_file(index_filename, NULL);
    return false;
  }

  return true;
}

static int sort_entries_cb(const void* a, const void* b)
{
  return memcmp(a, b, KEY_COMPARE_SIZE);
}

static bool read_existing(gpu_shader_cache_t* c, const char* index_filename, const char* blob_filename)
{
  c->index_file = fs_open_file(index_filename, "r+b", NULL);
  if (!c->index_file)
  {
    if (errno == EACCES)
    {
      WARNING_LOG("Failed to open shader cache index with EACCES, are you running two instances?");
      return true;   /* continue without cache */
    }
    return false;
  }

  cache_file_header_t hdr;
  if (fread(&hdr, sizeof(hdr), 1, c->index_file) != 1 ||
      hdr.signature != EXPECTED_SIGNATURE ||
      hdr.render_api_version != c->render_api_version ||
      hdr.cache_version != c->version)
  {
    ERROR_LOG("Bad file/data version in '%s'", basename_only(index_filename));
    fclose(c->index_file); c->index_file = NULL;
    return false;
  }

  c->blob_file = fs_open_file(blob_filename, "a+b", NULL);
  if (!c->blob_file)
  {
    ERROR_LOG("Blob file '%s' is missing", basename_only(blob_filename));
    fclose(c->index_file); c->index_file = NULL;
    return false;
  }

  const s64 start_pos = fs_ftell64(c->index_file);
  s64 end_pos;
  if (start_pos < 0 || fs_fseek64(c->index_file, 0, SEEK_END) != 0 ||
      (end_pos = fs_ftell64(c->index_file)) < 0 ||
      fs_fseek64(c->index_file, start_pos, SEEK_SET) != 0 ||
      ((end_pos - start_pos) % (s64)sizeof(gpu_shader_cache_index_entry_t)) != 0)
  {
    ERROR_LOG("Failed to seek in index file '%s'", basename_only(index_filename));
    fclose(c->blob_file);  c->blob_file  = NULL;
    fclose(c->index_file); c->index_file = NULL;
    return false;
  }

  const size_t n = (size_t)((end_pos - start_pos) / (s64)sizeof(gpu_shader_cache_index_entry_t));
  if (n > 0)
  {
    c->entries = (gpu_shader_cache_index_entry_t*)malloc(n * sizeof(*c->entries));
    if (!c->entries)
    {
      fclose(c->blob_file);  c->blob_file  = NULL;
      fclose(c->index_file); c->index_file = NULL;
      return false;
    }
    if (fread(c->entries, sizeof(*c->entries), n, c->index_file) != n)
    {
      ERROR_LOG("Failed to read entries from index file '%s'", basename_only(index_filename));
      free(c->entries); c->entries = NULL;
      fclose(c->blob_file);  c->blob_file  = NULL;
      fclose(c->index_file); c->index_file = NULL;
      return false;
    }
    c->entry_count    = n;
    c->entry_capacity = n;
    qsort(c->entries, n, sizeof(*c->entries), sort_entries_cb);
  }

  /* don't write before seeking */
  fs_fseek64(c->index_file, 0, SEEK_END);

  DEV_LOG("Read %zu entries from '%s'", c->entry_count, basename_only(index_filename));
  return true;
}

bool gpu_shader_cache_open(gpu_shader_cache_t* c, const char* base_filename,
                           u32 render_api_version, u32 cache_version)
{
  free(c->base_filename);
  c->base_filename       = strdup(base_filename ? base_filename : "");
  c->render_api_version  = render_api_version;
  c->version             = cache_version;

  if (!c->base_filename || c->base_filename[0] == '\0')
    return true;

  char idx[512], blb[512];
  filename_concat(idx, sizeof(idx), c->base_filename, ".idx");
  filename_concat(blb, sizeof(blb), c->base_filename, ".bin");
  return read_existing(c, idx, blb);
}

bool gpu_shader_cache_create(gpu_shader_cache_t* c)
{
  char idx[512], blb[512];
  filename_concat(idx, sizeof(idx), c->base_filename, ".idx");
  filename_concat(blb, sizeof(blb), c->base_filename, ".bin");
  return create_new(c, idx, blb);
}

void gpu_shader_cache_close(gpu_shader_cache_t* c)
{
  if (c->index_file) { fclose(c->index_file); c->index_file = NULL; }
  if (c->blob_file)  { fclose(c->blob_file);  c->blob_file  = NULL; }
}

void gpu_shader_cache_clear(gpu_shader_cache_t* c)
{
  if (!gpu_shader_cache_is_open(c)) return;
  gpu_shader_cache_close(c);
  WARNING_LOG("Clearing shader cache at %s.", basename_only(c->base_filename));

  char idx[512], blb[512];
  filename_concat(idx, sizeof(idx), c->base_filename, ".idx");
  filename_concat(blb, sizeof(blb), c->base_filename, ".bin");
  free(c->entries); c->entries = NULL; c->entry_count = c->entry_capacity = 0;
  create_new(c, idx, blb);
}

gpu_shader_cache_index_key_t
gpu_shader_cache_compute_key(gpu_shader_stage_t stage, gpu_shader_language_t language,
                             const char* shader_code, size_t shader_code_len,
                             const char* entry_point, size_t entry_point_len)
{
  union { struct { u64 lo; u64 hi; }; u8 b[16]; } h;
  gpu_shader_cache_index_key_t k;
  k.shader_type     = (u32)stage;
  k.shader_language = (u32)language;

  md5_digest_t d;
  md5_digest_init(&d);
  md5_digest_update(&d, (const u8*)shader_code, shader_code_len);
  md5_digest_final(&d, h.b);
  k.source_hash_low  = h.lo;
  k.source_hash_high = h.hi;
  k.source_length    = (u32)shader_code_len;

  md5_digest_reset(&d);
  md5_digest_update(&d, (const u8*)entry_point, entry_point_len);
  md5_digest_final(&d, h.b);
  k.entry_point_low  = h.lo;
  k.entry_point_high = h.hi;
  k.unused           = 0;
  return k;
}

static gpu_shader_cache_index_entry_t* find_entry(gpu_shader_cache_t* c, const gpu_shader_cache_index_key_t* k)
{
  if (c->entry_count == 0) return NULL;
  return (gpu_shader_cache_index_entry_t*)bsearch(k, c->entries, c->entry_count, sizeof(*c->entries), compare_keys);
}

bool gpu_shader_cache_lookup(gpu_shader_cache_t* c, const gpu_shader_cache_index_key_t* key,
                             void** out_data, u32* out_size)
{
  *out_data = NULL;
  *out_size = 0;

  const gpu_shader_cache_index_entry_t* e = find_entry(c, key);
  if (!e || !c->blob_file)
    return false;

  void* buf = malloc(e->compressed_size);
  if (!buf) return false;

  if (fseek(c->blob_file, (long)e->file_offset, SEEK_SET) != 0 ||
      fread(buf, e->compressed_size, 1, c->blob_file) != 1)
  {
    ERROR_LOG("Read %u byte %s shader from file failed", e->compressed_size,
              stage_name((gpu_shader_stage_t)key->shader_type));
    free(buf);
    return false;
  }

  if (e->compressed_size != e->uncompressed_size) {
    void* uncomp = malloc(e->uncompressed_size);
    if (!uncomp) { free(buf); return false; }
    const size_t r = ZSTD_decompress(uncomp, e->uncompressed_size, buf, e->compressed_size);
    free(buf);
    if (ZSTD_isError(r) || r != (size_t)e->uncompressed_size) {
      ERROR_LOG("zstd decompress failed for %s shader: %s",
                stage_name((gpu_shader_stage_t)key->shader_type),
                ZSTD_isError(r) ? ZSTD_getErrorName(r) : "size mismatch");
      free(uncomp);
      return false;
    }
    *out_data = uncomp;
    *out_size = e->uncompressed_size;
    return true;
  }

  *out_data = buf;
  *out_size = e->uncompressed_size;
  return true;
}

bool gpu_shader_cache_insert(gpu_shader_cache_t* c, const gpu_shader_cache_index_key_t* key,
                             const void* data, u32 data_size)
{
  /* Compress payload with Zstandard before persisting
   * (CompressHelpers::CompressToBuffer with Zstandard, level=-1 default). */
  const size_t compressed_max = ZSTD_compressBound(data_size);
  void* compressed = malloc(compressed_max);
  if (!compressed) return false;
  const size_t compressed_actual =
    ZSTD_compress(compressed, compressed_max, data, data_size, 0 /* default level */);
  if (ZSTD_isError(compressed_actual)) {
    ERROR_LOG("Failed to compress %u byte %s shader: %s",
              data_size, stage_name((gpu_shader_stage_t)key->shader_type),
              ZSTD_getErrorName(compressed_actual));
    free(compressed);
    return false;
  }

  if (!c->blob_file || fseek(c->blob_file, 0, SEEK_END) != 0) {
    free(compressed);
    return false;
  }

  /* find insert position */
  size_t lo = 0, hi = c->entry_count;
  while (lo < hi)
  {
    const size_t mid = lo + (hi - lo) / 2;
    if (memcmp(&c->entries[mid], key, KEY_COMPARE_SIZE) < 0) lo = mid + 1;
    else                                                     hi = mid;
  }

  if (c->entry_count + 1 > c->entry_capacity)
  {
    const size_t newcap = c->entry_capacity ? c->entry_capacity * 2 : 16;
    gpu_shader_cache_index_entry_t* p =
      (gpu_shader_cache_index_entry_t*)realloc(c->entries, newcap * sizeof(*p));
    if (!p) { free(compressed); return false; }
    c->entries        = p;
    c->entry_capacity = newcap;
  }

  if (lo < c->entry_count)
    memmove(&c->entries[lo + 1], &c->entries[lo], (c->entry_count - lo) * sizeof(*c->entries));
  ++c->entry_count;

  gpu_shader_cache_index_entry_t* e = &c->entries[lo];
  memset(e, 0, sizeof(*e));
  memcpy(e, key, KEY_COMPARE_SIZE);
  e->file_offset       = (u32)ftell(c->blob_file);
  e->compressed_size   = (u32)compressed_actual;
  e->uncompressed_size = data_size;

  if (fwrite(compressed, compressed_actual, 1, c->blob_file) != 1 || fflush(c->blob_file) != 0 ||
      fwrite(e, sizeof(*e), 1, c->index_file) != 1               || fflush(c->index_file) != 0)
  {
    ERROR_LOG("Failed to write %u byte %s shader blob to file", data_size,
              stage_name((gpu_shader_stage_t)key->shader_type));
    if (lo < c->entry_count - 1)
      memmove(&c->entries[lo], &c->entries[lo + 1], (c->entry_count - lo - 1) * sizeof(*c->entries));
    --c->entry_count;
    free(compressed);
    return false;
  }

  free(compressed);
  DEV_LOG("Cached compressed %s shader: %u -> %u bytes",
          stage_name((gpu_shader_stage_t)key->shader_type), data_size, (u32)compressed_actual);
  return true;
}
