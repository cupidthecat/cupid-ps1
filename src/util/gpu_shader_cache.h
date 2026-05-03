/*
 * First-pass port stores blobs **uncompressed** - zstd not in cupid-ps1's
 * compress_helpers yet.  Wire-format index/blob layout, header signature, and
 * key/entry sizes preserved so a future Zstd switch is a 1-line change in the
 * helper.
 */

#ifndef CUPID_UTIL_GPU_SHADER_CACHE_H
#define CUPID_UTIL_GPU_SHADER_CACHE_H

#include "common/types.h"
#include "util/gpu_types.h"

#include <stdio.h>

typedef struct Error Error;

/* 48 bytes; layout pinned so the binary cache is portable. */
typedef struct {
  u32 shader_type;
  u32 shader_language;
  u64 source_hash_low;
  u64 source_hash_high;
  u64 entry_point_low;
  u64 entry_point_high;
  u32 source_length;
  u32 unused;
} gpu_shader_cache_index_key_t;

typedef struct {
  u32 shader_type;
  u32 shader_language;
  u64 source_hash_low;
  u64 source_hash_high;
  u64 entry_point_low;
  u64 entry_point_high;
  u32 source_length;
  u32 file_offset;
  u32 compressed_size;
  u32 uncompressed_size;
} gpu_shader_cache_index_entry_t;

typedef struct {
  gpu_shader_cache_index_entry_t* entries;        /* sorted by key prefix */
  size_t                          entry_count;
  size_t                          entry_capacity;

  char* base_filename;
  u32   render_api_version;
  u32   version;

  FILE* index_file;
  FILE* blob_file;
} gpu_shader_cache_t;

void  gpu_shader_cache_init    (gpu_shader_cache_t* c);
void  gpu_shader_cache_destroy (gpu_shader_cache_t* c);

bool  gpu_shader_cache_is_open (const gpu_shader_cache_t* c);

bool  gpu_shader_cache_open    (gpu_shader_cache_t* c, const char* base_filename,
                                u32 render_api_version, u32 cache_version);
bool  gpu_shader_cache_create  (gpu_shader_cache_t* c);
void  gpu_shader_cache_close   (gpu_shader_cache_t* c);
void  gpu_shader_cache_clear   (gpu_shader_cache_t* c);

gpu_shader_cache_index_key_t
gpu_shader_cache_compute_key(gpu_shader_stage_t stage, gpu_shader_language_t language,
                             const char* shader_code, size_t shader_code_len,
                             const char* entry_point, size_t entry_point_len);

 /* Lookup: on hit, *out_data malloc'd and *out_size set; caller frees.
 * Returns false on miss (out_data left NULL) or on error (logged). */
bool gpu_shader_cache_lookup (gpu_shader_cache_t* c, const gpu_shader_cache_index_key_t* key,
                              void** out_data, u32* out_size);

bool gpu_shader_cache_insert (gpu_shader_cache_t* c, const gpu_shader_cache_index_key_t* key,
                              const void* data, u32 data_size);

#endif /* CUPID_UTIL_GPU_SHADER_CACHE_H */
