/*
 * GPU Texture Cache: a per-page LRU of decoded texture pages, keyed by
 * (page_index, palette_reg, texture_mode), backed by a hash cache that
 * deduplicates identical (texture_hash, palette_hash) pages across multiple
 * SourceKeys.  Sources are intrusive list nodes pinned across up to
 * MAX_PAGE_REFS_PER_SOURCE (=6) VRAM pages so a write to any of them
 * cheaply invalidates the affected entries.
 *
 * Scope:
 *   - Cache *off* by default; gated on `g_settings.gpu_texture_cache`.
 *     When off, `gpu_texture_cache_lookup_source` returns NULL, and the GPU
 *     back-end falls through to its non-cached uniform-buffer texture path.
 *   - Texture replacement disk loading is dropped entirely (`#if 0`'d, with
 *     TODO markers).
 *   - VRAM dumping / texture dumping dropped (post-MVP polish).
 *   - ImGui debug visualization dropped.
 *
 * Module state is file-static inside gpu_hw_texture_cache.c (no globals
 * exported beyond these functions).
 */

#ifndef CUPID_CORE_GPU_HW_TEXTURE_CACHE_H
#define CUPID_CORE_GPU_HW_TEXTURE_CACHE_H

#include "common/types.h"
#include "core/gpu_types.h"

typedef struct Error            Error;
typedef struct gpu_hw           gpu_hw_t;
typedef struct opengl_texture   opengl_texture_t;
typedef struct state_wrapper    state_wrapper_t;

/* 4 pages in C16 mode, 2+4 pages in P8 mode, 1+1 pages in P4 mode. */
#define GPU_TC_MAX_PAGE_REFS_PER_SOURCE 6u
#define GPU_TC_MAX_PAGE_REFS_PER_WRITE  32u

typedef u64 gpu_tc_hash_t;

typedef enum {
  GPU_TC_PALETTE_RECORD_FLAG_NONE                       = 0,
  GPU_TC_PALETTE_RECORD_FLAG_HAS_SEMI_TRANSPARENT_DRAWS = (1u << 0),
} gpu_tc_palette_record_flags_t;

/* SourceKey; 4-byte composite (page, mode, palette_reg).  Layout is
 * byte-for-byte fixed so the same memcmp-style comparison shape is OK
 * (we use field comparison since C doesn't have operator== overloads). */
typedef struct {
  u8                         page;     /* VRAM page index, 0..NUM_VRAM_PAGES */
  gpu_texture_mode_t         mode;
  gpu_texture_palette_reg_t  palette;
} gpu_tc_source_key_t;
_Static_assert(sizeof(gpu_tc_source_key_t) == 4, "SourceKey size must be 4");

ALWAYS_INLINE bool gpu_tc_source_key_eq(gpu_tc_source_key_t a, gpu_tc_source_key_t b)
{
  return a.page == b.page && a.mode == b.mode && a.palette.bits == b.palette.bits;
}
ALWAYS_INLINE bool gpu_tc_source_key_has_palette(gpu_tc_source_key_t k)
{
  return k.mode < GPU_TEXTURE_MODE_DIRECT_16BIT;
}

typedef struct tc_source gpu_tc_source_t;

/* Initialize the cache against the supplied HW back-end.  Must be called
 * after `g_gpu_device` is up and after `g_settings` is populated.  Returns
 * true on success; on failure `error` (if non-NULL) is populated.  When the
 * cache is disabled in settings this is a no-op success that simply records
 * the disabled state. */
bool gpu_texture_cache_initialize(gpu_hw_t* backend, Error* error);

 /* Tear down: invalidates everything, releases textures, clears module-static
 * state.  Safe to call multiple times. */
void gpu_texture_cache_shutdown(void);

/* Update settings post-init (e.g. when the user toggles texture cache or
 * replacements at runtime).  Toggles the on/off bit and invalidates if
 * tracking state changed. */
bool gpu_texture_cache_update_settings(bool use_texture_cache, Error* error);

 /* Clear all cached entries (page sources + hash cache) but keep tracking
 * state and pipelines.  Called on save-state load and on explicit cache
 * flush. */
void gpu_texture_cache_clear(void);

void gpu_texture_cache_invalidate(void);

/* Lookup or create the cache entry for a (page, palette, mode) key.  Returns
 * NULL when the cache is disabled, or when entry creation fails.  `uv_rect`
 * is the bounding box of the UVs the draw is about to read; pass an
 * "invalid" sentinel rect (all-zero is fine) to skip UV tracking.  Caller
 * must NOT free the returned pointer. */
const gpu_tc_source_t* gpu_texture_cache_lookup_source(gpu_tc_source_key_t key,
                                                        s32 uv_l, s32 uv_t,
                                                       s32 uv_r, s32 uv_b, 
                                                       gpu_tc_palette_record_flags_t flags);

opengl_texture_t* gpu_texture_cache_source_get_texture(const gpu_tc_source_t* src);

/* Invalidate all cache entries whose source pages overlap [start..end] in
 * VRAM (rectangle in pixels).  `update_vram_writes` controls whether the
 * dump-tracking list is also updated; `remove_from_hash_cache` forces hash
 * cache entries to be dropped instead of just dereferenced. */
void gpu_texture_cache_add_written_rectangle(s32 left, s32 top, s32 right, s32 bottom,
                                             bool update_vram_writes,
                                             bool remove_from_hash_cache);

/* Hook called when the GPU rasterizes into the VRAM render target (used by
 * texture-cache dirty tracking and split detection). */
void gpu_texture_cache_add_drawn_rectangle(s32 left, s32 top, s32 right, s32 bottom,
                                           s32 clip_l, s32 clip_t, s32 clip_r, s32 clip_b);

void gpu_texture_cache_write_vram(u32 x, u32 y, u32 width, u32 height, const void* data,
                                  bool set_mask, bool check_mask,
                                  s32 bounds_l, s32 bounds_t, s32 bounds_r, s32 bounds_b);

void gpu_texture_cache_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                 u32 width, u32 height,
                                 bool set_mask, bool check_mask,
                                 s32 src_l, s32 src_t, s32 src_r, s32 src_b,
                                 s32 dst_l, s32 dst_t, s32 dst_r, s32 dst_b);

/* CLUT-write invalidation: nukes any source whose palette region the write
 * may have touched. */
void gpu_texture_cache_invalidate_clut(gpu_texture_palette_reg_t reg, bool clut_is_8bit);

/* Drawn-rect query: was [l..r] x [t..b] of VRAM written to since the last
 * cache invalidation? */
bool gpu_texture_cache_is_rect_drawn(s32 l, s32 t, s32 r, s32 b);

/* Are any of the source pages this key references currently dirty? */
bool gpu_texture_cache_are_source_pages_drawn(gpu_tc_source_key_t key,
                                              s32 l, s32 t, s32 r, s32 b);

/* Periodic compaction (LRU eviction past hash-cache size limit). */
void gpu_texture_cache_compact(void);

/* Hooks invoked by the surrounding system. */
void gpu_texture_cache_game_serial_changed(void);
void gpu_texture_cache_reload_texture_replacements(bool show_info, bool show_info_if_none);

/* Save-state size probe + (de)serialization.  Zero-write the cache region so
 * save states stay forward/backward compatible while the cache itself is
 * off. */
bool gpu_texture_cache_get_state_size(state_wrapper_t* sw, u32* out_size);
bool gpu_texture_cache_do_state(state_wrapper_t* sw, bool skip);

/* True if the cache is currently active (settings toggle + init succeeded). */
bool gpu_texture_cache_is_enabled(void);

#endif /* CUPID_CORE_GPU_HW_TEXTURE_CACHE_H */
