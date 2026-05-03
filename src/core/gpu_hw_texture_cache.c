/*
 * GPU Texture Cache implementation.
 *
 * This C port is a structural skeleton that:
 *
 *   - Implements the full LRU-of-pages + hash-cache-of-pixel-images skeleton
 *     (intrusive doubly-linked lists, FNV-1a-keyed open-address hash table)
 *     so the data structures and invalidation flow match the original shape.
 *   - Routes all VRAM-mutation hooks (write/copy/clut/draw) through the
 *     correct page-walking invalidation path.
 *   - Defers GPU texture upload itself: the cache is *off* by default (gated
 *     on `g_settings.gpu_texture_cache`), so the lookup_source path returns
 *     NULL and the back-end falls through to its non-cached uniform-buffer
 *     texture path.  When the bit gets flipped on, the CreateSource path
 *     needs the GPU upload (TODO) wired in.
 *
 * Drop list (see "#if 0  TODO" blocks below):
 *   - Texture replacement disk loading (`LoadReplacement*`,
 *     `*ReplacementImage*`, `WriteReplacementImage*`).
 *   - VRAM dumping / texture dumping.
 *   - ImGui debug overlays.
 *
 * Naming: `GPUTextureCache::Foo` becomes `gpu_texture_cache_foo`
 * (public) or `tc_foo` (file-static).  Module state lives in a single
 * file-static `tc_state` aggregate; no globals are exported.
 */

#include "core/gpu_hw_texture_cache.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "core/gpu_helpers.h"
#include "core/gpu_hw.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/tc_image_loader.h"
#include "util/opengl_texture.h"
#include "util/state_wrapper.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

LOG_CHANNEL(GPU_HW);

#define TC_MAX_CLUT_SIZE        256u
#define TC_NUM_PAGE_DRAW_RECTS  4u

/* STATE_PALETTE_RECORD_SIZE = sizeof(GSVector4i) + sizeof(SourceKey)
 * + sizeof(PaletteRecordFlags) + sizeof(HashType) + sizeof(u16)*MAX_CLUT_SIZE
 * == 16 + 4 + 4 + 8 + 512 = 544.  Used only by GetStateSize/DoState skip
 * paths; we use the same constant so save-state byte layout is identical. */
#define TC_STATE_PALETTE_RECORD_SIZE  ((u32)(16u + 4u + 4u + 8u + 2u * TC_MAX_CLUT_SIZE))

 /* `struct tc_source` is forward-declared in the header (typedef'd as
 * gpu_tc_source_t); we just need a local typedef alias. */
typedef struct tc_source        tc_source_t;
typedef struct tc_vram_write    tc_vram_write_t;
typedef struct tc_hash_entry    tc_hash_entry_t;

typedef struct tc_node {
  void*           ref;       /* tc_source_t* or tc_vram_write_t* */
  struct tc_list* list;
  struct tc_node* prev;
  struct tc_node* next;
} tc_node_t;

typedef struct tc_list {
  tc_node_t* head;
  tc_node_t* tail;
} tc_list_t;

static void tc_list_prepend(tc_list_t* list, void* item, tc_node_t* node)
{
  node->ref  = item;
  node->list = list;
  node->prev = NULL;
  if (list->tail) {
    node->next       = list->head;
    list->head->prev = node;
    list->head       = node;
  } else {
    node->next = NULL;
    list->head = node;
    list->tail = node;
  }
}
static void tc_list_append(tc_list_t* list, void* item, tc_node_t* node)
{
  node->ref  = item;
  node->list = list;
  node->next = NULL;
  if (list->tail) {
    node->prev       = list->tail;
    list->tail->next = node;
    list->tail       = node;
  } else {
    node->prev = NULL;
    list->head = node;
    list->tail = node;
  }
}
static void tc_list_move_to_front(tc_list_t* list, tc_node_t* node)
{
  if (!node->prev)
    return;
  node->prev->next = node->next;
  if (node->next)
    node->next->prev = node->prev;
  else
    list->tail = node->prev;
  node->prev       = NULL;
  list->head->prev = node;
  node->next       = list->head;
  list->head       = node;
}
static void tc_list_unlink(tc_node_t* node)
{
  if (node->prev)
    node->prev->next = node->next;
  else if (node->list)
    node->list->head = node->next;
  if (node->next)
    node->next->prev = node->prev;
  else if (node->list)
    node->list->tail = node->prev;
}

struct tc_source {
  gpu_tc_source_key_t  key;
  u32                  num_page_refs;
  opengl_texture_t*    texture;          /* non-owning; owned by hash entry */
  tc_hash_entry_t*     from_hash_cache;
  s32                  texture_l, texture_t, texture_r, texture_b;
  s32                  palette_l, palette_t, palette_r, palette_b;
  gpu_tc_hash_t        texture_hash;
  gpu_tc_hash_t        palette_hash;
  s32                  active_uv_l, active_uv_t, active_uv_r, active_uv_b;
  gpu_tc_palette_record_flags_t palette_record_flags;

  tc_node_t            page_refs[GPU_TC_MAX_PAGE_REFS_PER_SOURCE];
  tc_node_t            hash_cache_ref;
};

struct tc_hash_entry {
  bool                used;
  bool                tombstone;
  /* key */
  gpu_tc_hash_t       texture_hash;
  gpu_tc_hash_t       palette_hash;
  gpu_tc_hash_t       mode;            /* mode is packed into HashCacheKey */
  /* value */
  opengl_texture_t*   texture;
  u32                 ref_count;
  u32                 last_used_frame;
  tc_list_t           sources;         /* tc_source_t intrusive list */
};

typedef struct {
  s32           rect_l, rect_t, rect_r, rect_b;
  gpu_tc_source_key_t key;
  gpu_tc_palette_record_flags_t flags;
  gpu_tc_hash_t palette_hash;
  u16           palette[TC_MAX_CLUT_SIZE];
} tc_palette_record_t;

struct tc_vram_write {
  s32           active_l, active_t, active_r, active_b;
  s32           write_l,  write_t,  write_r,  write_b;
  gpu_tc_hash_t hash;

  tc_palette_record_t* palette_records;
  u32                  palette_records_count;
  u32                  palette_records_cap;

  u32                  num_splits;
  u32                  num_page_refs;
  tc_node_t            page_refs[GPU_TC_MAX_PAGE_REFS_PER_WRITE];
};

 /* Page width/height for cache textures = TEXTURE_PAGE_WIDTH x TEXTURE_PAGE_HEIGHT
 * (256 x 256 texels in texel space).  In RGBA8 each texel is 4 bytes, so a full
 * staging buffer is 256*256*4 = 256 KiB. */
#define TC_DECODE_TEXEL_W   ((u32)TEXTURE_PAGE_WIDTH)
#define TC_DECODE_TEXEL_H   ((u32)VRAM_PAGE_HEIGHT)   /* TEXTURE_PAGE_HEIGHT == 256 */
#define TC_DECODE_BYTES     (TC_DECODE_TEXEL_W * TC_DECODE_TEXEL_H * 4u)

/* Scratch staging buffer reused across upload operations.  Uses the file-static
 * lifetime to avoid stack pressure (256 KiB > typical stack page). */
static u8 tc_decode_scratch[TC_DECODE_BYTES];

ALWAYS_INLINE void tc_write_rgba8(u8** dst_ptr, u16 vram_pixel)
{
  const u32 rgba = vram_rgba5551_to_rgba8888((u32)vram_pixel);
  u8* d = *dst_ptr;
  d[0] = (u8)(rgba       & 0xFFu);
  d[1] = (u8)((rgba >>  8) & 0xFFu);
  d[2] = (u8)((rgba >> 16) & 0xFFu);
  d[3] = (u8)((rgba >> 24) & 0xFFu);
  *dst_ptr = d + 4;
}

 /* 4-bit decode: each VRAM pixel packs 4 4-bit indices, produces 4 RGBA8 texels.
 * `width` is in texels; `page` points at the top-left VRAM word. */
static void tc_decode4(const u16* page, const u16* palette, u32 width, u32 height,
                       u8* dest, u32 dest_stride)
{
  for (u32 y = 0; y < height; y++) {
    const u16* row = page + (size_t)y * VRAM_WIDTH;
    u8* d = dest + (size_t)y * dest_stride;
    if ((width & 3u) == 0u) {
      const u32 vram_w = width / 4u;
      for (u32 x = 0; x < vram_w; x++) {
        const u32 pp = (u32)row[x];
        tc_write_rgba8(&d, palette[pp & 0x0Fu]);
        tc_write_rgba8(&d, palette[(pp >> 4) & 0x0Fu]);
        tc_write_rgba8(&d, palette[(pp >> 8) & 0x0Fu]);
        tc_write_rgba8(&d, palette[(pp >> 12) & 0x0Fu]);
      }
    } else {
      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++) {
        if (offs == 0)
          texel = row[x >> 2];
        tc_write_rgba8(&d, palette[texel & 0x0Fu]);
        texel = (u16)(texel >> 4);
        offs = (offs + 1u) & 3u;
      }
    }
  }
}

/* 8-bit decode: each VRAM pixel packs 2 8-bit indices, produces 2 RGBA8 texels. */
static void tc_decode8(const u16* page, const u16* palette, u32 width, u32 height,
                       u8* dest, u32 dest_stride)
{
  for (u32 y = 0; y < height; y++) {
    const u16* row = page + (size_t)y * VRAM_WIDTH;
    u8* d = dest + (size_t)y * dest_stride;
    if ((width & 1u) == 0u) {
      const u32 vram_w = width / 2u;
      for (u32 x = 0; x < vram_w; x++) {
        const u32 pp = (u32)row[x];
        tc_write_rgba8(&d, palette[pp & 0xFFu]);
        tc_write_rgba8(&d, palette[(pp >> 8) & 0xFFu]);
      }
    } else {
      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++) {
        if (offs == 0)
          texel = row[x >> 1];
        tc_write_rgba8(&d, palette[texel & 0xFFu]);
        texel = (u16)(texel >> 8);
        offs ^= 1u;
      }
    }
  }
}

/* Direct 16-bit: 1 VRAM pixel = 1 RGBA8 texel.  No CLUT lookup. */
static void tc_decode16(const u16* page, u32 width, u32 height,
                        u8* dest, u32 dest_stride)
{
  for (u32 y = 0; y < height; y++) {
    const u16* row = page + (size_t)y * VRAM_WIDTH;
    u8* d = dest + (size_t)y * dest_stride;
    for (u32 x = 0; x < width; x++)
      tc_write_rgba8(&d, row[x]);
  }
}

/* Decode the page identified by (page_index, palette, mode) into the supplied
 * RGBA8 buffer with `dest_stride` bytes per row.  Renders TEXTURE_PAGE_WIDTH x
 * VRAM_PAGE_HEIGHT texels (the cache texture extent).  Mirror of
 * DecodeTexture(). */
static void tc_decode_page(u8 page_index, gpu_texture_palette_reg_t palette_reg,
                           gpu_texture_mode_t mode, u8* dest, u32 dest_stride)
{
  const u32 sx = vram_page_start_x((u32)page_index);
  const u32 sy = vram_page_start_y((u32)page_index);
  const u16* page_ptr = &g_vram[(size_t)sy * VRAM_WIDTH + sx];

  const u32 px = gpu_texture_palette_reg_get_x_base(palette_reg);
  const u32 py = gpu_texture_palette_reg_get_y_base(palette_reg);
  const u16* palette_ptr = (mode < GPU_TEXTURE_MODE_DIRECT_16BIT)
                             ? &g_vram[(size_t)py * VRAM_WIDTH + px]
                             : NULL;

  switch (mode) {
    case GPU_TEXTURE_MODE_PALETTE_4BIT:
      tc_decode4(page_ptr, palette_ptr, TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H, dest, dest_stride);
      break;
    case GPU_TEXTURE_MODE_PALETTE_8BIT:
      tc_decode8(page_ptr, palette_ptr, TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H, dest, dest_stride);
      break;
    case GPU_TEXTURE_MODE_DIRECT_16BIT:
    case GPU_TEXTURE_MODE_RESERVED_DIRECT_16BIT:
      tc_decode16(page_ptr, TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H, dest, dest_stride);
      break;
  }
}

/* PageEntry: per-VRAM-page draw + source + write tracking. */
typedef struct {
  tc_list_t   sources;
  tc_list_t   writes;
  u32         num_draw_rects;
  s32         total_draw_l, total_draw_t, total_draw_r, total_draw_b;
  s32         draw_rects_l[TC_NUM_PAGE_DRAW_RECTS];
  s32         draw_rects_t[TC_NUM_PAGE_DRAW_RECTS];
  s32         draw_rects_r[TC_NUM_PAGE_DRAW_RECTS];
  s32         draw_rects_b[TC_NUM_PAGE_DRAW_RECTS];
} tc_page_entry_t;

/* Texture-replacement entry parsed from `textures/<serial>/replacements/`.
 * Pixels are decoded lazily on first lookup so a 1500-entry pack with no
 * matches yet costs only the directory walk + name parse. */
typedef struct {
  char* path;             /* owned, full path on disk */
  u64   tex_hash;
  u64   pal_hash;         /* 0 if not present in filename */
  u8    mode;             /* GPU_TEXTURE_MODE_*; 0xff = unparsed/unknown */
  u8    has_pal_hash;
  u8*   pixels;           /* RGBA8, NULL until first decode */
  u32   width, height;
  u32   last_used_frame;
  opengl_texture_t* gpu_tex;  /* NULL until first GPU use */
  size_t gpu_bytes;
} tc_replacement_t;

typedef struct {
  bool                enabled;          /* g_settings.gpu_texture_cache && initialized */
  bool                initialized;
  bool                track_vram_writes;
  gpu_hw_t*           backend;

  /* Hash cache: open-address table of tc_hash_entry_t.  Power-of-two cap so
   * we can mask instead of mod.  Tombstones flagged separately. */
  tc_hash_entry_t*    hash_cache;
  u32                 hash_cache_cap;
  u32                 hash_cache_count;

  size_t              hash_cache_memory_usage;

  tc_vram_write_t*    last_vram_write;

  tc_page_entry_t     pages[NUM_VRAM_PAGES];

  u32                 frame_number;     /* incremented on add_drawn_rectangle */

  /* Texture-replacement pack (see gpu_texture_cache_reload_texture_replacements). */
  tc_replacement_t*   replacements;
  u32                 replacements_count;
  u32                 replacements_cap;
  size_t              replacement_cpu_bytes;
  size_t              replacement_gpu_bytes;
  char*               current_serial;   /* strdup; empty/NULL == none loaded */
} tc_state_t;

static tc_state_t tc_state;

/* Forward decl: linear (tex_hash, pal_hash, mode) match against the loaded
 * replacement table.  Lazy-decodes PNG + lazy-uploads to GL on first hit.
 * Returns the GL texture to substitute for the freshly-decoded VRAM page,
 * or NULL when no replacement matches / load fails. */
static opengl_texture_t* tc_apply_replacement(gpu_tc_source_key_t key,
                                              gpu_tc_hash_t tex_hash, gpu_tc_hash_t pal_hash);

ALWAYS_INLINE bool tc_rect_overlaps(s32 al, s32 at, s32 ar, s32 ab,
                                    s32 bl, s32 bt, s32 br, s32 bb)
{
  return !(ar <= bl || br <= al || ab <= bt || bb <= at);
}
ALWAYS_INLINE bool tc_rect_empty(s32 l, s32 t, s32 r, s32 b)
{
  return r <= l || b <= t;
}
ALWAYS_INLINE void tc_rect_union(s32* dl, s32* dt, s32* dr, s32* db,
                                 s32  sl, s32  st, s32  sr, s32  sb)
{
  if (sl < *dl) *dl = sl;
  if (st < *dt) *dt = st;
  if (sr > *dr) *dr = sr;
  if (sb > *db) *db = sb;
}

ALWAYS_INLINE u32 tc_clamp_u32(u32 v, u32 lo, u32 hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Iterate over every VRAM page touched by [left,top..right,bottom).  Assumes
 * the rect is non-empty and pre-clipped to VRAM bounds. */
#define TC_LOOP_RECT_PAGES_BEGIN(rl, rt, rr, rb, page_var)                  \
  do {                                                                       \
    const u32 _rl = (u32)(rl), _rt = (u32)(rt), _rr = (u32)(rr), _rb = (u32)(rb); \
    if (_rr > _rl && _rb > _rt) {                                            \
      const u32 _sx = _rl / VRAM_PAGE_WIDTH;                                 \
      const u32 _ex = (_rr - 1) / VRAM_PAGE_WIDTH;                           \
      const u32 _sy = _rt / VRAM_PAGE_HEIGHT;                                \
      const u32 _ey = (_rb - 1) / VRAM_PAGE_HEIGHT;                          \
      for (u32 _py = _sy; _py <= _ey; _py++) {                               \
        for (u32 _px = _sx; _px <= _ex; _px++) {                             \
          const u32 page_var = vram_page_index(_px, _py);

#define TC_LOOP_RECT_PAGES_END                                              \
        }                                                                    \
      }                                                                      \
    }                                                                        \
  } while (0)

static gpu_tc_hash_t tc_fnv1a_init(void) { return (gpu_tc_hash_t)UINT64_C(0xcbf29ce484222325); }
static gpu_tc_hash_t tc_fnv1a_step(gpu_tc_hash_t h, const void* data, size_t len)
{
  const u8* p = (const u8*)data;
  for (size_t i = 0; i < len; i++)
    h = (h ^ p[i]) * UINT64_C(0x100000001b3);
  return h;
}

static gpu_tc_hash_t tc_hash_page(u8 page, gpu_texture_mode_t mode)
{
  /* Hash the relevant region of g_vram for this page+mode (mirror of
   * HashPage but scalar instead of XXH3).  The exact algorithm differs,
   * so cache-content fingerprint identity won't match across
   * implementations; that's OK since we don't share serialized hashes
   * across forks. */
  const u32 pn   = (u32)page;
  const u32 sx   = vram_page_start_x(pn);
  const u32 sy   = vram_page_start_y(pn);
  const u32 w    = texture_page_width_for_mode(mode);
  const u32 effective_w = (sx + w > VRAM_WIDTH) ? (VRAM_WIDTH - sx) : w;
  const u32 h    = VRAM_PAGE_HEIGHT;

  gpu_tc_hash_t hash = tc_fnv1a_init();
  for (u32 y = 0; y < h; y++) {
    const u16* row = &g_vram[(sy + y) * VRAM_WIDTH + sx];
    hash = tc_fnv1a_step(hash, row, (size_t)effective_w * sizeof(u16));
  }
  hash = tc_fnv1a_step(hash, &mode, sizeof(mode));
  return hash;
}

static gpu_tc_hash_t tc_hash_palette(gpu_texture_palette_reg_t reg, gpu_texture_mode_t mode)
{
  const u32 width = get_palette_width(mode);
  if (width == 0)
    return 0;
  const u32 px = gpu_texture_palette_reg_get_x_base(reg);
  const u32 py = gpu_texture_palette_reg_get_y_base(reg);
  const u32 effective_w = (px + width > VRAM_WIDTH) ? (VRAM_WIDTH - px) : width;
  const u16* row = &g_vram[py * VRAM_WIDTH + px];
  gpu_tc_hash_t hash = tc_fnv1a_init();
  hash = tc_fnv1a_step(hash, row, (size_t)effective_w * sizeof(u16));
  hash = tc_fnv1a_step(hash, &reg.bits, sizeof(reg.bits));
  return hash;
}

ALWAYS_INLINE u32 tc_hash_cache_probe(gpu_tc_hash_t texture_hash, gpu_tc_hash_t palette_hash,
                                      gpu_tc_hash_t mode, u32 cap_mask)
{
  /* Combine: simple xor-chain.  cap is power of two so mask is cap-1. */
  const gpu_tc_hash_t h = texture_hash ^ (palette_hash + 0x9e3779b97f4a7c15ULL) ^ (mode * 31u);
  return (u32)(h & cap_mask);
}

#define TC_HASH_CACHE_INITIAL_CAP 64u

static bool tc_hash_cache_grow(u32 new_cap);
static bool tc_hash_cache_evict_lru(void);

static void tc_hash_cache_init(void)
{
  tc_state.hash_cache       = NULL;
  tc_state.hash_cache_cap   = 0;
  tc_state.hash_cache_count = 0;
}

static void tc_hash_cache_destroy(void)
{
  if (tc_state.hash_cache) {
    /* All textures should have been destroyed by Invalidate prior to this. */
    free(tc_state.hash_cache);
    tc_state.hash_cache = NULL;
  }
  tc_state.hash_cache_cap   = 0;
  tc_state.hash_cache_count = 0;
}

static tc_hash_entry_t* tc_hash_cache_find(gpu_tc_hash_t texture_hash, gpu_tc_hash_t palette_hash,
                                           gpu_tc_hash_t mode)
{
  if (!tc_state.hash_cache || tc_state.hash_cache_cap == 0)
    return NULL;
  const u32 cap_mask = tc_state.hash_cache_cap - 1u;
  u32 idx = tc_hash_cache_probe(texture_hash, palette_hash, mode, cap_mask);
  for (u32 i = 0; i < tc_state.hash_cache_cap; i++) {
    tc_hash_entry_t* e = &tc_state.hash_cache[idx];
    if (!e->used && !e->tombstone)
      return NULL;
    if (e->used && e->texture_hash == texture_hash && e->palette_hash == palette_hash && e->mode == mode)
      return e;
    idx = (idx + 1u) & cap_mask;
  }
  return NULL;
}

static tc_hash_entry_t* tc_hash_cache_insert(gpu_tc_hash_t texture_hash, gpu_tc_hash_t palette_hash,
                                             gpu_tc_hash_t mode)
{
  /* Grow if load factor > 0.7. */
  if (tc_state.hash_cache_cap == 0 || (tc_state.hash_cache_count * 10u) >= (tc_state.hash_cache_cap * 7u)) {
    const u32 new_cap = (tc_state.hash_cache_cap == 0) ? TC_HASH_CACHE_INITIAL_CAP
                                                       : (tc_state.hash_cache_cap * 2u);
    if (!tc_hash_cache_grow(new_cap))
      return NULL;
  }

  const u32 cap_mask = tc_state.hash_cache_cap - 1u;
  u32 idx = tc_hash_cache_probe(texture_hash, palette_hash, mode, cap_mask);
  u32 first_tombstone = (u32)-1;
  for (u32 i = 0; i < tc_state.hash_cache_cap; i++) {
    tc_hash_entry_t* e = &tc_state.hash_cache[idx];
    if (e->used && e->texture_hash == texture_hash && e->palette_hash == palette_hash && e->mode == mode)
      return e;
    if (!e->used) {
      if (e->tombstone) {
        if (first_tombstone == (u32)-1)
          first_tombstone = idx;
      } else {
        const u32 slot = (first_tombstone != (u32)-1) ? first_tombstone : idx;
        tc_hash_entry_t* dst = &tc_state.hash_cache[slot];
        memset(dst, 0, sizeof(*dst));
        dst->used         = true;
        dst->tombstone    = false;
        dst->texture_hash = texture_hash;
        dst->palette_hash = palette_hash;
        dst->mode         = mode;
        tc_state.hash_cache_count++;
        return dst;
      }
    }
    idx = (idx + 1u) & cap_mask;
  }
  return NULL;
}

static bool tc_hash_cache_grow(u32 new_cap)
{
  tc_hash_entry_t* old_table = tc_state.hash_cache;
  const u32 old_cap = tc_state.hash_cache_cap;

  tc_hash_entry_t* new_table = (tc_hash_entry_t*)calloc(new_cap, sizeof(tc_hash_entry_t));
  if (!new_table)
    return false;

  tc_state.hash_cache       = new_table;
  tc_state.hash_cache_cap   = new_cap;
  tc_state.hash_cache_count = 0;

  if (old_table) {
    const u32 cap_mask = new_cap - 1u;
    for (u32 i = 0; i < old_cap; i++) {
      tc_hash_entry_t* src = &old_table[i];
      if (!src->used)
        continue;
      u32 idx = tc_hash_cache_probe(src->texture_hash, src->palette_hash, src->mode, cap_mask);
      for (u32 step = 0; step < new_cap; step++) {
        if (!new_table[idx].used) {
          new_table[idx] = *src;
          /* hash_cache_ref nodes embedded in tc_source_t still point at the
           * old `sources.list` field of `src`.  Re-target them at the new
           * slot's `sources` list. */
          new_table[idx].sources = src->sources;
          for (tc_node_t* n = new_table[idx].sources.head; n; n = n->next)
            n->list = &new_table[idx].sources;
          tc_state.hash_cache_count++;
          break;
        }
        idx = (idx + 1u) & cap_mask;
      }
    }
    free(old_table);
  }
  return true;
}

static void tc_destroy_source(tc_source_t* src, bool remove_from_hash_cache)
{
  for (u32 i = 0; i < src->num_page_refs; i++)
    tc_list_unlink(&src->page_refs[i]);

  tc_hash_entry_t* hcentry = src->from_hash_cache;
  if (hcentry) {
    tc_list_unlink(&src->hash_cache_ref);
    if (hcentry->ref_count > 0)
      hcentry->ref_count--;
    if (hcentry->ref_count == 0 && remove_from_hash_cache) {
      /* Drop GPU texture + slot. */
      if (hcentry->texture) {
        opengl_texture_destroy(hcentry->texture);
        hcentry->texture = NULL;
      }
      hcentry->used      = false;
      hcentry->tombstone = true;
      memset(&hcentry->sources, 0, sizeof(hcentry->sources));
      tc_state.hash_cache_count--;
    }
  }

  free(src);
}

static void tc_invalidate_page_sources(u32 pn)
{
  if (pn >= NUM_VRAM_PAGES)
    return;
  tc_list_t* list = &tc_state.pages[pn].sources;
  for (tc_node_t* n = list->head; n;) {
    tc_source_t* src = (tc_source_t*)n->ref;
    n = n->next;
    tc_destroy_source(src, false);
  }
}

static void tc_invalidate_page_sources_in_rect(u32 pn, s32 rl, s32 rt, s32 rr, s32 rb,
                                               bool remove_from_hash_cache)
{
  if (pn >= NUM_VRAM_PAGES)
    return;
  tc_list_t* list = &tc_state.pages[pn].sources;
  for (tc_node_t* n = list->head; n;) {
    tc_source_t* src = (tc_source_t*)n->ref;
    n = n->next;
    const bool tex_hits = tc_rect_overlaps(src->texture_l, src->texture_t, src->texture_r, src->texture_b,
                                           rl, rt, rr, rb);
    const bool pal_hits = (src->key.mode != GPU_TEXTURE_MODE_DIRECT_16BIT) &&
                          tc_rect_overlaps(src->palette_l, src->palette_t, src->palette_r, src->palette_b,
                                           rl, rt, rr, rb);
    if (!tex_hits && !pal_hits)
      continue;
    tc_destroy_source(src, remove_from_hash_cache);
  }
}

static void tc_invalidate_all_sources(void)
{
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
    tc_invalidate_page_sources(i);
}

static void tc_remove_vram_write(tc_vram_write_t* vrw)
{
  for (u32 i = 0; i < vrw->num_page_refs; i++)
    tc_list_unlink(&vrw->page_refs[i]);
  if (tc_state.last_vram_write == vrw)
    tc_state.last_vram_write = NULL;
  free(vrw->palette_records);
  free(vrw);
}

static void tc_clear_hash_cache(void)
{
  if (!tc_state.hash_cache)
    return;
  for (u32 i = 0; i < tc_state.hash_cache_cap; i++) {
    tc_hash_entry_t* e = &tc_state.hash_cache[i];
    if (!e->used)
      continue;
    /* Sources should have been destroyed first via Invalidate; if any remain,
     * unlink them so we don't dangling-deref. */
    while (e->sources.head) {
      tc_source_t* src = (tc_source_t*)e->sources.head->ref;
      tc_destroy_source(src, false);
    }
    if (e->texture) {
      opengl_texture_destroy(e->texture);
      e->texture = NULL;
    }
    e->used      = false;
    e->tombstone = false;
  }
  tc_state.hash_cache_count = 0;
  tc_state.hash_cache_memory_usage = 0;
}

/* Look up or create a hash-cache entry for (key, tex_hash, pal_hash).
 * Returns NULL on OOM.  TODO: when the cache is enabled, this also decodes
 * the texture out of g_vram and uploads it. */
static tc_hash_entry_t* tc_lookup_hash_cache(gpu_tc_source_key_t key,
                                             gpu_tc_hash_t tex_hash, gpu_tc_hash_t pal_hash)
{
  tc_hash_entry_t* e = tc_hash_cache_find(tex_hash, pal_hash, (gpu_tc_hash_t)key.mode);
  if (e)
    return e;

  e = tc_hash_cache_insert(tex_hash, pal_hash, (gpu_tc_hash_t)key.mode);
  if (!e)
    return NULL;

  e->ref_count       = 0;
  e->last_used_frame = tc_state.frame_number;
  e->texture         = NULL;
  e->sources.head    = NULL;
  e->sources.tail    = NULL;

  /* Decode the page out of g_vram + apply CLUT lookup -> RGBA8 staging buffer,
   * then create the GPU texture in a single shot.  Mirrors
   * GPUTextureCache::LookupHashCache GPU-upload branch.
   *
   * NOTE: replacement disk loading (ApplyTextureReplacements) goes here
   * when wired; between the decode and the opengl_texture_create call. */
  const u32 dest_stride = TC_DECODE_TEXEL_W * 4u;
  tc_decode_page(key.page, key.palette, key.mode, tc_decode_scratch, dest_stride);

  e->texture = opengl_texture_create(TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H,
                                     /*layers=*/1u, /*levels=*/1u, /*samples=*/1u,
                                     GPU_TEXTURE_TYPE_TEXTURE,
                                     GPU_TEXTURE_FORMAT_RGBA8,
                                     GPU_TEXTURE_FLAG_NONE,
                                     tc_decode_scratch, dest_stride,
                                     /*err=*/NULL);
  /* One-time INFO_LOG so the smoke test confirms uploads actually fire on
   * the first miss and subsequent draws hit the cache (no further log
   * after that).  Drop once apitrace / dev OSD telemetry is wired. */
  static bool tc_logged_first_upload = false;
  if (e->texture && !tc_logged_first_upload) {
    tc_logged_first_upload = true;
    INFO_LOG("TC: first GPU upload (page=%u mode=%u palette=0x%04X) -> %ux%u RGBA8",
             (u32)key.page, (u32)key.mode, (u32)key.palette.bits,
             TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H);
  }
  if (!e->texture) {
    e->used      = false;
    e->tombstone = true;
    tc_state.hash_cache_count--;
    return NULL;
  }

  /* Texture-replacement swap: if the user has dropped a matching PNG into
   * `textures/<serial>/replacements/`, substitute it for the freshly-decoded
   * VRAM-derived texture.  Lazy-decode + lazy-upload happens inside
   * tc_apply_replacement; the replacement's GPU texture is owned by the
   * tc_replacement_t entry, so we just point e->texture at it after destroying
   * the decoded one we just created.  Bookkeeping: we still account the
   * decoded-page bytes (already added below) since we throw them away
   * immediately; no double-count of the replacement bytes here; those live
   * in tc_state.replacement_gpu_bytes. */
  if (g_settings.texture_replacements.enable_texture_replacements &&
      tc_state.replacements_count > 0u) {
    opengl_texture_t* repl = tc_apply_replacement(key, tex_hash, pal_hash);
    if (repl) {
      opengl_texture_destroy(e->texture);
      e->texture = repl;
    }
  }

  /* Approximate VRAM usage: width * height * 4 bytes (RGBA8). */
  tc_state.hash_cache_memory_usage += (size_t)TC_DECODE_TEXEL_W * TC_DECODE_TEXEL_H * 4u;

  /* Opportunistic LRU eviction once we exceed the configured caps.  Done
   * in-line so the cache can self-regulate without an external EndFrame
   * hook (we don't have one wired yet; TODO). */
  const u32    max_entries = g_settings.texture_replacements.config.max_hash_cache_entries;
  const size_t max_bytes   = (size_t)g_settings.texture_replacements.config.max_hash_cache_vram_usage_mb
                            
                             * 1048576u;
                             
  while ((max_entries > 0u && tc_state.hash_cache_count > max_entries) ||
         (max_bytes   > 0u && tc_state.hash_cache_memory_usage > max_bytes)) {
    if (!tc_hash_cache_evict_lru())
      break;
  }

  return e;
}

/* Build a Source for `key`, register it with all the pages it overlaps, and
 * link it into its hash-cache entry. */
static tc_source_t* tc_create_source(gpu_tc_source_key_t key)
{
  const gpu_tc_hash_t tex_hash = tc_hash_page(key.page, key.mode);
  const gpu_tc_hash_t pal_hash = (key.mode < GPU_TEXTURE_MODE_DIRECT_16BIT)
                                   ? tc_hash_palette(key.palette, key.mode)
                                   : 0;

  tc_hash_entry_t* hc = tc_lookup_hash_cache(key, tex_hash, pal_hash);
  if (!hc)
    return NULL;

  tc_source_t* src = (tc_source_t*)calloc(1, sizeof(tc_source_t));
  if (!src)
    return NULL;

  src->key             = key;
  src->texture         = hc->texture;
  src->from_hash_cache = hc;
  src->texture_hash    = tex_hash;
  src->palette_hash    = pal_hash;
  src->active_uv_l = src->active_uv_t = INT32_MAX;
  src->active_uv_r = src->active_uv_b = INT32_MIN;

  /* Texture rect (handles VRAM-X-wrap by clamping to row 0..VRAM_WIDTH if the
   * page would extend past the right edge; mirrors GetTextureRect). */
  {
    u32 left   = vram_page_start_x((u32)key.page);
    u32 top    = vram_page_start_y((u32)key.page);
    u32 right  = left + texture_page_width_for_mode(key.mode);
    u32 bottom = top  + VRAM_PAGE_HEIGHT;
    if (right > VRAM_WIDTH)  { left = 0; right  = VRAM_WIDTH;  }
    if (bottom > VRAM_HEIGHT){ top  = 0; bottom = VRAM_HEIGHT; }
    src->texture_l = (s32)left;
    src->texture_t = (s32)top;
    src->texture_r = (s32)right;
    src->texture_b = (s32)bottom;
  }

  hc->ref_count++;
  tc_list_append(&hc->sources, src, &src->hash_cache_ref);

  /* Track which pages we've already added refs for, to dedupe overlap. */
  u32 page_refns[GPU_TC_MAX_PAGE_REFS_PER_SOURCE] = {0};
  const u32 page_count = texture_page_count_for_mode(key.mode);
  for (u32 i = 0; i < page_count && src->num_page_refs < GPU_TC_MAX_PAGE_REFS_PER_SOURCE; i++) {
    const u32 wrapped =
      (((u32)key.page + i) & VRAM_PAGE_X_MASK) | ((u32)key.page & VRAM_PAGE_Y_MASK);
    bool dup = false;
    for (u32 j = 0; j < src->num_page_refs; j++) {
      if (page_refns[j] == wrapped) { dup = true; break; }
    }
    if (dup)
      continue;
    const u32 ri = src->num_page_refs++;
    page_refns[ri] = wrapped;
    tc_list_prepend(&tc_state.pages[wrapped].sources, src, &src->page_refs[ri]);
  }

  /* Palette rect + refs. */
  if (key.mode < GPU_TEXTURE_MODE_DIRECT_16BIT) {
    const u32 width = get_palette_width(key.mode);
    u32 left   = gpu_texture_palette_reg_get_x_base(key.palette);
    u32 top    = gpu_texture_palette_reg_get_y_base(key.palette);
    u32 right  = left + width;
    u32 bottom = top  + 1u;
    if (right > VRAM_WIDTH) right = VRAM_WIDTH;
    src->palette_l = (s32)left;
    src->palette_t = (s32)top;
    src->palette_r = (s32)right;
    src->palette_b = (s32)bottom;

    const u32 pal_pages = palette_page_count_for_mode(key.mode);
    const u32 pal_first = palette_page_number(key.palette);
    for (u32 i = 0; i < pal_pages && src->num_page_refs < GPU_TC_MAX_PAGE_REFS_PER_SOURCE; i++) {
      const u32 wrapped =
        (((pal_first + i) & VRAM_PAGE_X_MASK) | (pal_first & VRAM_PAGE_Y_MASK));
      bool dup = false;
      for (u32 j = 0; j < src->num_page_refs; j++) {
        if (page_refns[j] == wrapped) { dup = true; break; }
      }
      if (dup)
        continue;
      const u32 ri = src->num_page_refs++;
      page_refns[ri] = wrapped;
      tc_list_append(&tc_state.pages[wrapped].sources, src, &src->page_refs[ri]);
    }
  }

  return src;
}

bool gpu_texture_cache_initialize(gpu_hw_t* backend, Error* error)
{
  (void)error;

  memset(&tc_state, 0, sizeof(tc_state));
  tc_state.backend     = backend;
  tc_state.initialized = true;
  tc_state.enabled     = g_settings.gpu_texture_cache;

  if (!tc_state.enabled)
    return true;

  tc_hash_cache_init();
  tc_state.track_vram_writes = g_settings.texture_replacements.always_track_uploads;

  /* Pick up replacements for the already-running game.  system_update_
   * running_game() likely fired before us and got an early-return, so do
   * the walk ourselves now that tc_state.initialized=true. */
  const char* serial = system_get_game_serial();
  if (serial && *serial)
    tc_state.current_serial = strdup(serial);
  gpu_texture_cache_reload_texture_replacements(/*show_info=*/true,
                                                /*show_info_if_none=*/false);

  /* Sources/pages start zeroed (calloc-equivalent via memset above). */
  return true;
}

static void tc_replacements_clear(void);  /* defined below in the replacement-loader block */

void gpu_texture_cache_shutdown(void)
{
  if (!tc_state.initialized)
    return;
  gpu_texture_cache_invalidate();
  tc_clear_hash_cache();
  tc_hash_cache_destroy();
  tc_replacements_clear();
  free(tc_state.current_serial); tc_state.current_serial = NULL;
  memset(&tc_state, 0, sizeof(tc_state));
}

bool gpu_texture_cache_update_settings(bool use_texture_cache, Error* error)
{
  (void)error;
  const bool prev_enabled = tc_state.enabled;
  tc_state.enabled = use_texture_cache && tc_state.initialized;
  if (prev_enabled != tc_state.enabled)
    gpu_texture_cache_invalidate();
  return true;
}

void gpu_texture_cache_clear(void)
{
  if (!tc_state.initialized)
    return;
  tc_invalidate_all_sources();
  tc_clear_hash_cache();
}

void gpu_texture_cache_invalidate(void)
{
  if (!tc_state.initialized)
    return;
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++) {
    tc_invalidate_page_sources(i);
    tc_page_entry_t* p = &tc_state.pages[i];
    p->num_draw_rects = 0;
    p->total_draw_l = p->total_draw_t = p->total_draw_r = p->total_draw_b = 0;
    while (p->writes.tail)
      tc_remove_vram_write((tc_vram_write_t*)p->writes.tail->ref);
  }
  tc_clear_hash_cache();
  tc_state.last_vram_write = NULL;
}

bool gpu_texture_cache_is_enabled(void)
{
  return tc_state.enabled;
}

const gpu_tc_source_t* gpu_texture_cache_lookup_source(gpu_tc_source_key_t key,
                                                        s32 uv_l, s32 uv_t, s32 uv_r, s32 uv_b,
                                                       gpu_tc_palette_record_flags_t flags) 
{
  if (!tc_state.enabled || (u32)key.page >= NUM_VRAM_PAGES)
    return NULL;

  tc_list_t* list = &tc_state.pages[key.page].sources;
  for (tc_node_t* n = list->head; n; n = n->next) {
    tc_source_t* s = (tc_source_t*)n->ref;
    if (gpu_tc_source_key_eq(s->key, key)) {
      tc_list_move_to_front(list, n);
      if (s->from_hash_cache)
        s->from_hash_cache->last_used_frame = tc_state.frame_number;
      if (!tc_rect_empty(uv_l, uv_t, uv_r, uv_b)) {
        tc_rect_union(&s->active_uv_l, &s->active_uv_t, &s->active_uv_r, &s->active_uv_b,
                      uv_l, uv_t, uv_r, uv_b);
        s->palette_record_flags |= flags;
      }
      return s;
    }
  }

  tc_source_t* fresh = tc_create_source(key);
  if (fresh && !tc_rect_empty(uv_l, uv_t, uv_r, uv_b)) {
    tc_rect_union(&fresh->active_uv_l, &fresh->active_uv_t, &fresh->active_uv_r, &fresh->active_uv_b,
                  uv_l, uv_t, uv_r, uv_b);
    fresh->palette_record_flags |= flags;
  }
  return fresh;
}

opengl_texture_t* gpu_texture_cache_source_get_texture(const gpu_tc_source_t* src)
{
  if (!src || !src->from_hash_cache)
    return NULL;
  return src->from_hash_cache->texture;
}

void gpu_texture_cache_add_written_rectangle(s32 left, s32 top, s32 right, s32 bottom,
                                             bool update_vram_writes,
                                             bool remove_from_hash_cache)
{
  (void)update_vram_writes;
  if (!tc_state.initialized || tc_rect_empty(left, top, right, bottom))
    return;

  /* Clamp to VRAM bounds. */
  if (left   < 0)                left   = 0;
  if (top    < 0)                top    = 0;
  if (right  > (s32)VRAM_WIDTH)  right  = (s32)VRAM_WIDTH;
  if (bottom > (s32)VRAM_HEIGHT) bottom = (s32)VRAM_HEIGHT;
  if (tc_rect_empty(left, top, right, bottom))
    return;

  TC_LOOP_RECT_PAGES_BEGIN(left, top, right, bottom, page_var) {
    tc_invalidate_page_sources_in_rect(page_var, left, top, right, bottom, remove_from_hash_cache);
  } TC_LOOP_RECT_PAGES_END;

  if (update_vram_writes && tc_state.track_vram_writes) {
    tc_vram_write_t* vrw = (tc_vram_write_t*)calloc(1, sizeof(tc_vram_write_t));
    if (vrw) {
      vrw->active_l = left; vrw->active_t = top;
      vrw->active_r = right; vrw->active_b = bottom;
      vrw->write_l  = left; vrw->write_t  = top;
      vrw->write_r  = right; vrw->write_b  = bottom;
      vrw->hash     = 0;       /* fingerprint generated at replacement-match time */

      TC_LOOP_RECT_PAGES_BEGIN(left, top, right, bottom, page_var2) {
        if (vrw->num_page_refs >= GPU_TC_MAX_PAGE_REFS_PER_WRITE)
          continue;
        const u32 ri = vrw->num_page_refs++;
        tc_list_append(&tc_state.pages[page_var2].writes, vrw, &vrw->page_refs[ri]);
      } TC_LOOP_RECT_PAGES_END;

      if (vrw->num_page_refs == 0) {
        /* Empty rect or out-of-range page index; drop without leaking. */
        free(vrw->palette_records);
        free(vrw);
      } else {
        tc_state.last_vram_write = vrw;
      }
    }
  }
}

void gpu_texture_cache_add_drawn_rectangle(s32 left, s32 top, s32 right, s32 bottom,
                                           s32 clip_l, s32 clip_t, s32 clip_r, s32 clip_b)
{
  (void)clip_l; (void)clip_t; (void)clip_r; (void)clip_b;
  if (!tc_state.initialized || tc_rect_empty(left, top, right, bottom))
    return;
  tc_state.frame_number++;

  if (left   < 0)                left   = 0;
  if (top    < 0)                top    = 0;
  if (right  > (s32)VRAM_WIDTH)  right  = (s32)VRAM_WIDTH;
  if (bottom > (s32)VRAM_HEIGHT) bottom = (s32)VRAM_HEIGHT;
  if (tc_rect_empty(left, top, right, bottom))
    return;

  /* Drawn rect implies the destination region is dirtied; invalidate any
   * sources that read from it. */
  TC_LOOP_RECT_PAGES_BEGIN(left, top, right, bottom, page_var) {
    tc_page_entry_t* p = &tc_state.pages[page_var];
    if (p->num_draw_rects == 0) {
      p->total_draw_l = left;  p->total_draw_t = top;
      p->total_draw_r = right; p->total_draw_b = bottom;
    } else {
      tc_rect_union(&p->total_draw_l, &p->total_draw_t, &p->total_draw_r, &p->total_draw_b,
                    left, top, right, bottom);
    }
    if (p->num_draw_rects < TC_NUM_PAGE_DRAW_RECTS) {
      const u32 i = p->num_draw_rects++;
      p->draw_rects_l[i] = left;  p->draw_rects_t[i] = top;
      p->draw_rects_r[i] = right; p->draw_rects_b[i] = bottom;
    }
    tc_invalidate_page_sources_in_rect(page_var, left, top, right, bottom, false);
  } TC_LOOP_RECT_PAGES_END;
}

void gpu_texture_cache_write_vram(u32 x, u32 y, u32 width, u32 height, const void* data,
                                  bool set_mask, bool check_mask,
                                  s32 bounds_l, s32 bounds_t, s32 bounds_r, s32 bounds_b)
{
  (void)data; (void)set_mask; (void)check_mask;
  (void)bounds_l; (void)bounds_t; (void)bounds_r; (void)bounds_b;
  if (!tc_state.initialized)
    return;
  /* Forward as a written-rectangle invalidation. */
  gpu_texture_cache_add_written_rectangle((s32)x, (s32)y, (s32)(x + width), (s32)(y + height),
                                          /*update_vram_writes=*/true,
                                          /*remove_from_hash_cache=*/false);
}

void gpu_texture_cache_copy_vram(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                                 u32 width, u32 height,
                                 bool set_mask, bool check_mask,
                                  s32 src_l, s32 src_t, s32 src_r, s32 src_b,
                                 s32 dst_l, s32 dst_t, s32 dst_r, s32 dst_b) 
{
  (void)src_x; (void)src_y; (void)set_mask; (void)check_mask;
  (void)src_l; (void)src_t; (void)src_r; (void)src_b;
  (void)dst_l; (void)dst_t; (void)dst_r; (void)dst_b;
  if (!tc_state.initialized)
    return;
  /* The destination side of a VRAM->VRAM copy invalidates the cache. */
  gpu_texture_cache_add_written_rectangle((s32)dst_x, (s32)dst_y,
                                          (s32)(dst_x + width), (s32)(dst_y + height),
                                          /*update_vram_writes=*/true,
                                          /*remove_from_hash_cache=*/false);
}

void gpu_texture_cache_invalidate_clut(gpu_texture_palette_reg_t reg, bool clut_is_8bit)
{
  if (!tc_state.initialized)
    return;
  /* CLUT update invalidates sources whose palette region overlaps the new
   * CLUT (or any CLUT page in 8-bit mode). */
  const u32 px = gpu_texture_palette_reg_get_x_base(reg);
  const u32 py = gpu_texture_palette_reg_get_y_base(reg);
  const u32 width  = clut_is_8bit ? 256u : 16u;
  const u32 right  = (px + width > VRAM_WIDTH) ? VRAM_WIDTH : (px + width);
  gpu_texture_cache_add_written_rectangle((s32)px, (s32)py, (s32)right, (s32)(py + 1u),
                                          /*update_vram_writes=*/false,
                                          /*remove_from_hash_cache=*/false);
}

static bool tc_is_page_drawn(u32 page_index, s32 l, s32 t, s32 r, s32 b)
{
  if (page_index >= NUM_VRAM_PAGES)
    return false;
  const tc_page_entry_t* p = &tc_state.pages[page_index];
  if (p->num_draw_rects == 0)
    return false;
  if (!tc_rect_overlaps(p->total_draw_l, p->total_draw_t, p->total_draw_r, p->total_draw_b, l, t, r, b))
    return false;
  if (p->num_draw_rects == 1)
    return true;
  for (u32 i = 0; i < p->num_draw_rects; i++) {
    if (tc_rect_overlaps(p->draw_rects_l[i], p->draw_rects_t[i],
                         p->draw_rects_r[i], p->draw_rects_b[i], l, t, r, b))
      return true;
  }
  return false;
}

bool gpu_texture_cache_is_rect_drawn(s32 l, s32 t, s32 r, s32 b)
{
  if (!tc_state.initialized || tc_rect_empty(l, t, r, b))
    return false;
  s32 cl = l, ct = t, cr = r, cb = b;
  if (cl < 0)                cl = 0;
  if (ct < 0)                ct = 0;
  if (cr > (s32)VRAM_WIDTH)  cr = (s32)VRAM_WIDTH;
  if (cb > (s32)VRAM_HEIGHT) cb = (s32)VRAM_HEIGHT;
  if (tc_rect_empty(cl, ct, cr, cb))
    return false;

  bool any_drawn = false;
  TC_LOOP_RECT_PAGES_BEGIN(cl, ct, cr, cb, page_var) {
    if (tc_is_page_drawn(page_var, l, t, r, b)) {
      any_drawn = true;
    }
  } TC_LOOP_RECT_PAGES_END;
  return any_drawn;
}

bool gpu_texture_cache_are_source_pages_drawn(gpu_tc_source_key_t key,
                                              s32 l, s32 t, s32 r, s32 b)
{
  if (!tc_state.initialized)
    return false;

  switch (key.mode) {
    case GPU_TEXTURE_MODE_PALETTE_4BIT:
      return tc_is_page_drawn((u32)key.page, l, t, r, b);

    case GPU_TEXTURE_MODE_PALETTE_8BIT: {
      const u32 yoffs = ((u32)key.page & VRAM_PAGE_Y_MASK);
      return tc_is_page_drawn((u32)key.page, l, t, r, b) ||
             tc_is_page_drawn((((u32)key.page + 1u) & VRAM_PAGE_X_MASK) + yoffs, l, t, r, b);
    }

    case GPU_TEXTURE_MODE_DIRECT_16BIT:
    case GPU_TEXTURE_MODE_RESERVED_DIRECT_16BIT: {
      const u32 yoffs = ((u32)key.page & VRAM_PAGE_Y_MASK);
      return tc_is_page_drawn((u32)key.page, l, t, r, b) ||
             tc_is_page_drawn((((u32)key.page + 1u) & VRAM_PAGE_X_MASK) + yoffs, l, t, r, b) ||
             tc_is_page_drawn((((u32)key.page + 2u) & VRAM_PAGE_X_MASK) + yoffs, l, t, r, b) ||
             tc_is_page_drawn((((u32)key.page + 3u) & VRAM_PAGE_X_MASK) + yoffs, l, t, r, b);
    }
  }
  return false;
}

/* Helper: evict the unused (ref_count==0) LRU hash-cache entry.  Returns true
 * if something was evicted.  Walks the open-address table looking for the
 * oldest `last_used_frame` among unreferenced entries; ref_count>0 entries
 * are still in use by a live source and can't be dropped without first
 * destroying that source.  Mirrors Compact's purge loop but uses a
 * single-pass min instead of a sorted purge_list (we drop one entry
 * per call; caller loops in compact). */
static bool tc_hash_cache_evict_lru(void)
{
  if (!tc_state.hash_cache || tc_state.hash_cache_cap == 0)
    return false;

  u32  victim_idx = (u32)-1;
  u32  victim_age = UINT32_MAX;
  for (u32 i = 0; i < tc_state.hash_cache_cap; i++) {
    tc_hash_entry_t* e = &tc_state.hash_cache[i];
    if (!e->used || e->ref_count > 0)
      continue;
    if (e->last_used_frame <= victim_age) {
      victim_age = e->last_used_frame;
      victim_idx = i;
    }
  }
  if (victim_idx == (u32)-1)
    return false;

  tc_hash_entry_t* victim = &tc_state.hash_cache[victim_idx];
  if (victim->texture) {
    const size_t bytes = (size_t)TC_DECODE_TEXEL_W * TC_DECODE_TEXEL_H * 4u;
    if (tc_state.hash_cache_memory_usage >= bytes)
      tc_state.hash_cache_memory_usage -= bytes;
    opengl_texture_destroy(victim->texture);
    victim->texture = NULL;
  }
  victim->used      = false;
  victim->tombstone = true;
  memset(&victim->sources, 0, sizeof(victim->sources));
  tc_state.hash_cache_count--;
  return true;
}

void gpu_texture_cache_compact(void)
{
  if (!tc_state.initialized)
    return;

  const u32    max_entries  = g_settings.texture_replacements.config.max_hash_cache_entries;
  const size_t max_bytes    = (size_t)g_settings.texture_replacements.config.max_hash_cache_vram_usage_mb
                             
                              * 1048576u;

                              
  /* Evict unreferenced LRU entries until we're under both caps (or there's
   * nothing left to evict).  Replacement disk-image LRU is TODO. */
  while ((max_entries > 0u && tc_state.hash_cache_count > max_entries) ||
         (max_bytes   > 0u && tc_state.hash_cache_memory_usage > max_bytes)) {
    if (!tc_hash_cache_evict_lru())
      break;
  }
}

/* --- Texture replacement loading ------------------------------------- */

/* Decode lower-case 16-hex into a u64.  Returns false unless exactly 16 hex
 * digits were consumed.  Tolerates upper-case for forward-compat. */
static bool tc_parse_hex64(const char* p, size_t n, u64* out)
{
  if (n != 16u) return false;
  u64 v = 0u;
  for (size_t i = 0; i < 16u; i++) {
    const char c = p[i];
    u32 d;
    if      (c >= '0' && c <= '9') d = (u32)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
    else                            return false;
    v = (v << 4) | (u64)d;
  }
  *out = v;
  return true;
}

/* Parse `texpage-<mode>-<srchash:16>-<palhash:16>-...png` or
 * `texupload-<mode>-<srchash:16>-...png`.  Decodes the mode + the 16-hex
 * hash(es); ignores the rest of the filename (extents, etc.) since the
 * lookup-time matcher is keyed on (mode, tex_hash, pal_hash) only.
 * Returns false on any malformed entry (caller skips it). */
/* Mode token decoder: P4 / P8 / C16 / STP4 / STP8 / STC16.  Semitransparent
 * variants fold to their base mode; the LookupHashCache key keys off
 * GPUTextureMode (no ST bit). */
static bool tc_parse_mode_token(const char* tok, size_t len, u8* out_mode)
{
  if (len == 2 && tok[0] == 'P' && tok[1] == '4')   { *out_mode = (u8)GPU_TEXTURE_MODE_PALETTE_4BIT; return true; }
  if (len == 2 && tok[0] == 'P' && tok[1] == '8')   { *out_mode = (u8)GPU_TEXTURE_MODE_PALETTE_8BIT; return true; }
  if (len == 3 && tok[0] == 'C' && tok[1] == '1' && tok[2] == '6')
                                                    { *out_mode = (u8)GPU_TEXTURE_MODE_DIRECT_16BIT; return true; }
  if (len == 4 && tok[0] == 'S' && tok[1] == 'T' && tok[2] == 'P' && tok[3] == '4')
                                                    { *out_mode = (u8)GPU_TEXTURE_MODE_PALETTE_4BIT; return true; }
  if (len == 4 && tok[0] == 'S' && tok[1] == 'T' && tok[2] == 'P' && tok[3] == '8')
                                                    { *out_mode = (u8)GPU_TEXTURE_MODE_PALETTE_8BIT; return true; }
  if (len == 5 && tok[0] == 'S' && tok[1] == 'T' && tok[2] == 'C' && tok[3] == '1' && tok[4] == '6')
                                                    { *out_mode = (u8)GPU_TEXTURE_MODE_DIRECT_16BIT; return true; }
  return false;
}

static bool tc_parse_replacement_filename(const char* fname, tc_replacement_t* out)
{
  if (!fname) return false;
  /* Quick shape check + advance past the "texpage-" / "texupload-" prefix. */
  const char* p;
  if      (strncmp(fname, "texpage-",   8) == 0) p = fname + 8;
  else if (strncmp(fname, "texupload-",10) == 0) p = fname + 10;
  else                                            return false;

  /* Mode token. */
  const char* dash = strchr(p, '-');
  if (!dash) return false;
  if (!tc_parse_mode_token(p, (size_t)(dash - p), &out->mode))
    return false;
  p = dash + 1;

  /* First 16-hex token = src hash, mandatory. */
  dash = strchr(p, '-');
  if (!dash) return false;
  if (!tc_parse_hex64(p, (size_t)(dash - p), &out->tex_hash))
    return false;
  p = dash + 1;

  /* Second token may be the palette hash (16 hex, 0...0 if untextured) or
   * something else (e.g. a dimension token starting with a digit followed by
   * 'x').  Try a hash parse first, fall back to "no pal hash". */
  out->has_pal_hash = 0;
  out->pal_hash     = 0u;
  dash = strchr(p, '-');
  if (dash && tc_parse_hex64(p, (size_t)(dash - p), &out->pal_hash))
    out->has_pal_hash = 1;

  return true;
}

/* Free CPU + GPU resources for a single replacement.  Caller must adjust the
 * accounting counters and shrink the array size separately. */
static void tc_replacement_release(tc_replacement_t* r)
{
  if (!r) return;
  if (r->pixels) { tc_free_image(r->pixels); r->pixels = NULL; }
  if (r->gpu_tex) { opengl_texture_destroy(r->gpu_tex); r->gpu_tex = NULL; }
  free(r->path); r->path = NULL;
  r->width = r->height = 0u;
  r->gpu_bytes = 0u;
}

static void tc_replacements_clear(void)
{
  for (u32 i = 0; i < tc_state.replacements_count; i++)
    tc_replacement_release(&tc_state.replacements[i]);
  free(tc_state.replacements);
  tc_state.replacements          = NULL;
  tc_state.replacements_count    = 0u;
  tc_state.replacements_cap      = 0u;
  tc_state.replacement_cpu_bytes = 0u;
  tc_state.replacement_gpu_bytes = 0u;
}

/* Linear-scan match against the loaded replacement table.  Returns the GL
 * texture to use, or NULL on no-match / load-failure.  Called from
 * tc_lookup_hash_cache after the VRAM-derived texture has already been
 * decoded + uploaded (so a failure here just leaves the original in place).
 *
 * Match rule: tex_hash + (pal_hash if mode<DIRECT_16BIT) + mode.  We compare
 * the base mode (semitransparent ST* variants fold to the same enum value at
 * parse time, mirroring the upstream HashCacheKey shape).  Mode comparison is
 * skipped only when the parsed entry's mode is 0xff; that's the parse
 * failure / poisoned-after-bad-decode sentinel.
 *
 * TODO(scale): once we honour non-1x replacements we'll need to track scale
 * here.  For now any replacement size other than TC_DECODE_TEXEL_W x
 * TC_DECODE_TEXEL_H gets a one-line warning and is still applied (UVs may
 * miss-sample on non-integer scales).
 *
 * TODO(eviction): tc_state.replacement_gpu_bytes is bumped on upload but
 * gpu_texture_cache_compact does not factor it in yet; leave that to a
 * follow-up. */
static opengl_texture_t* tc_apply_replacement(gpu_tc_source_key_t key,
                                              gpu_tc_hash_t tex_hash, gpu_tc_hash_t pal_hash)
{
  const bool needs_pal = (key.mode < GPU_TEXTURE_MODE_DIRECT_16BIT);

  for (u32 i = 0; i < tc_state.replacements_count; i++) {
    tc_replacement_t* r = &tc_state.replacements[i];
    if (r->tex_hash != (u64)tex_hash) continue;
    if (r->mode != 0xffu && r->mode != (u8)key.mode) continue;
    if (needs_pal) {
      if (!r->has_pal_hash) continue;
      if (r->pal_hash != (u64)pal_hash) continue;
    }

    /* Lazy CPU decode. */
    if (!r->pixels) {
      if (!tc_load_png_rgba8(r->path, &r->pixels, &r->width, &r->height)) {
        WARNING_LOG("[texrep] Failed to decode '%s'; skipping replacement", r->path);
        /* Poison the entry so we don't keep retrying every lookup. */
        r->mode = 0xffu;
        r->tex_hash = 0u; r->pal_hash = 0u; r->has_pal_hash = 0;
        continue;
      }
      tc_state.replacement_cpu_bytes += (size_t)r->width * (size_t)r->height * 4u;
      if (r->width != TC_DECODE_TEXEL_W || r->height != TC_DECODE_TEXEL_H) {
        WARNING_LOG("[texrep] '%s' is %ux%u, native is %ux%u; UV may miss-sample",
                    r->path, r->width, r->height, TC_DECODE_TEXEL_W, TC_DECODE_TEXEL_H);
      }
    }

    /* Lazy GPU upload via opengl_texture_create (RGBA8 + GL_TEXTURE_2D). */
    if (!r->gpu_tex) {
      r->gpu_tex = opengl_texture_create(r->width, r->height,
                                         /*layers=*/1u, /*levels=*/1u, /*samples=*/1u,
                                         GPU_TEXTURE_TYPE_TEXTURE,
                                         GPU_TEXTURE_FORMAT_RGBA8,
                                         GPU_TEXTURE_FLAG_NONE,
                                         r->pixels, r->width * 4u,
                                         /*err=*/NULL);
      if (!r->gpu_tex) {
        WARNING_LOG("[texrep] GL upload failed for '%s'; skipping", r->path);
        continue;
      }
      r->gpu_bytes = (size_t)r->width * (size_t)r->height * 4u;
      tc_state.replacement_gpu_bytes += r->gpu_bytes;
      INFO_LOG("[texrep] Bound replacement '%s' (%ux%u) for tex=%016llX pal=%016llX mode=%u",
               r->path, r->width, r->height,
               (unsigned long long)tex_hash, (unsigned long long)pal_hash, (u32)key.mode);
    }

    r->last_used_frame = tc_state.frame_number;
    return r->gpu_tex;
  }

  return NULL;
}

static bool tc_replacements_grow(u32 new_cap)
{
  if (new_cap <= tc_state.replacements_cap) return true;
  tc_replacement_t* nb = (tc_replacement_t*)
      realloc(tc_state.replacements, sizeof(tc_replacement_t) * (size_t)new_cap);
  if (!nb) return false;
  tc_state.replacements     = nb;
  tc_state.replacements_cap = new_cap;
  return true;
}

void gpu_texture_cache_game_serial_changed(void)
{
  if (!tc_state.initialized) return;

  const char* serial = system_get_game_serial();
  /* Skip the rewalk if the serial hasn't actually changed; avoids a churn
   * of "same files re-scanned" on save-state load / m3u disc swap. */
  if (tc_state.current_serial && serial && strcmp(tc_state.current_serial, serial) == 0)
    return;

  /* First transition (NULL -> non-empty serial) logs even if found=0 so the
   * user sees one line per game load.  Inter-disc swaps stay quiet. */
  const bool first_transition =
      (tc_state.current_serial == NULL || tc_state.current_serial[0] == '\0') &&
      serial && *serial;

  tc_replacements_clear();
  free(tc_state.current_serial); tc_state.current_serial = NULL;
  if (serial && *serial)
    tc_state.current_serial = strdup(serial);

  gpu_texture_cache_reload_texture_replacements(/*show_info=*/first_transition,
                                                /*show_info_if_none=*/false);
}

void gpu_texture_cache_reload_texture_replacements(bool show_info, bool show_info_if_none)
{
  if (!tc_state.initialized) return;

  /* Drop whatever we had; the directory layout may have changed at runtime. */
  tc_replacements_clear();

  const char* serial = (tc_state.current_serial && *tc_state.current_serial)
                       ? tc_state.current_serial
                       : system_get_game_serial();
  if (!serial || !*serial) {
    if (show_info_if_none)
      INFO_LOG("[texrep] No game serial available; skipping replacement scan");
    return;
  }

  char dir[1024];
  const int dn = snprintf(dir, sizeof dir, "textures/%s/replacements", serial);
  if (dn < 0 || (size_t)dn >= sizeof dir) {
    WARNING_LOG("[texrep] Replacement directory path too long for serial '%s'", serial);
    return;
  }

  DIR* d = opendir(dir);
  if (!d) {
    /* ENOENT is normal: no pack installed.  Anything else (perm denied,
     * etc.) is worth surfacing once. */
    if (errno != ENOENT)
      WARNING_LOG("[texrep] opendir('%s') failed: %s", dir, strerror(errno));
    if (show_info || show_info_if_none)
      INFO_LOG("[texrep] No replacement pack found for '%s'", serial);
    return;
  }

  u32 found = 0u;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    /* Skip dotfiles + entries we can't classify cheaply. */
    if (e->d_name[0] == '.') continue;

    /* Suffix check: must end in .png (case-insensitive). */
    const size_t nlen = strlen(e->d_name);
    if (nlen <= 4u || strcasecmp(e->d_name + (nlen - 4u), ".png") != 0)
      continue;

    /* Build the full path. */
    char fp[1280];
    const int fn = snprintf(fp, sizeof fp, "%s/%s", dir, e->d_name);
    if (fn < 0 || (size_t)fn >= sizeof fp) continue;

    struct stat st;
    if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    tc_replacement_t r;
    memset(&r, 0, sizeof r);
    if (!tc_parse_replacement_filename(e->d_name, &r))
      continue;

    if (!tc_replacements_grow(tc_state.replacements_count == 0u
                              ? 64u
                              : tc_state.replacements_cap * 2u)) {
      WARNING_LOG("[texrep] Out of memory growing replacement table");
      break;
    }
    r.path = strdup(fp);
    if (!r.path) { tc_replacement_release(&r); break; }
    tc_state.replacements[tc_state.replacements_count++] = r;
    found++;
  }
  closedir(d);

  if (found > 0u || show_info)
    INFO_LOG("[texrep] Found %u replacement textures for '%s'", found, serial);
  else if (show_info_if_none)
    INFO_LOG("[texrep] No replacement textures matched for '%s'", serial);
}

bool gpu_texture_cache_get_state_size(state_wrapper_t* sw, u32* out_size)
{
  if (state_wrapper_get_version(sw) < 73) {
    *out_size = 0;
    return true;
  }

  /* Save format: u32 num_writes, then per-write blob.  We never write
   * anything (track_vram_writes always false), so the size is just the
   * marker + the count word. */
  *out_size = sizeof(u32);
  return true;
}

bool gpu_texture_cache_do_state(state_wrapper_t* sw, bool skip)
{
  (void)skip;
  if (state_wrapper_get_version(sw) < 73) {
    if (!skip && tc_state.initialized)
      gpu_texture_cache_invalidate();
    return true;
  }

  if (!state_wrapper_do_marker(sw, "GPUTextureCache"))
    return false;

  u32 num_vram_writes = 0;  /* always 0; tracking is off */
  state_wrapper_do_u32(sw, &num_vram_writes);

  if (state_wrapper_is_reading(sw)) {
    /* On load, skip any persisted vram-write blobs from a future-cache state. */
    for (u32 i = 0; i < num_vram_writes; i++) {
      /* skip 16 (active_rect) + 16 (write_rect) + 8 (hash) bytes */
      state_wrapper_skip_bytes(sw, 16 + 16 + 8);
      u32 num_palette_records = 0;
      state_wrapper_do_u32(sw, &num_palette_records);
      state_wrapper_skip_bytes(sw, (size_t)num_palette_records * TC_STATE_PALETTE_RECORD_SIZE);
    }
    if (!skip && tc_state.initialized)
      gpu_texture_cache_invalidate();
  }

  return !state_wrapper_has_error(sw);
}
