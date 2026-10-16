/*
 * CDImagePPF: a PPF v1/v2/v3 patch overlay on top of an already-opened
 * parent CDImage.  This is NOT a top-level format - instead the factory in
 * cd_image.c looks for a sidecar `<basename>.ppf` after a successful open
 * and wraps via cd_image_overlay_ppf.
 *
 * Patches are stored as (sector_index → 2352-byte replacement) pairs in a
 * hand-rolled FNV-1a open-address hash table.  Reads consult the table
 * first; misses delegate to the parent image.
 */

#include "cd_image.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDImage);

#define PPF_DESC_SIZE        50u
#define PPF_BLOCKCHECK_SIZE  1024u
#define PPF_RAW_SECTOR_SIZE  ((u32)CD_IMAGE_RAW_SECTOR_SIZE)

/* Sector → replacement-data-offset open-address hash table. */
typedef struct ppf_map_entry {
  u32  sector;     /* key */
  u32  offset;     /* offset into replacement_data */
  bool used;
} ppf_map_entry_t;

typedef struct cd_image_ppf {
  cd_image_t base;          /* MUST be first */

  cd_image_t* parent;       /* owned: destroyed by ppf destroy */
  s64         patch_size;
  u32         replacement_offset;

  u8*         replacement_data;
  size_t      replacement_size;
  size_t      replacement_cap;

  ppf_map_entry_t* map;
  size_t           map_cap;     /* power of 2, 0 until first insert */
  size_t           map_count;
} cd_image_ppf_t;

static u8* ppf_grow_replacement(cd_image_ppf_t* p, size_t add)
{
  if (p->replacement_size + add > p->replacement_cap) {
    size_t new_cap = p->replacement_cap ? p->replacement_cap * 2 : (PPF_RAW_SECTOR_SIZE * 16u);
    while (new_cap < p->replacement_size + add) new_cap *= 2;
    p->replacement_data = (u8*)realloc(p->replacement_data, new_cap);
    if (!p->replacement_data) abort();
    p->replacement_cap = new_cap;
  }
  u8* slot = p->replacement_data + p->replacement_size;
  p->replacement_size += add;
  return slot;
}

static u32 ppf_map_hash(u32 sector)
{
  /* FNV-1a 32-bit on the 4 bytes of the sector index. */
  u32 h = 0x811c9dc5u;
  for (u32 i = 0; i < 4; i++) {
    h ^= (u8)(sector >> (i * 8));
    h *= 0x01000193u;
  }
  return h;
}

static void ppf_map_grow(cd_image_ppf_t* p);

static u32* ppf_map_lookup_or_insert(cd_image_ppf_t* p, u32 sector, bool* inserted)
{
  /* Grow before inserting if past 0.7 load factor. */
  if (p->map_cap == 0 || (p->map_count + 1) * 10u >= p->map_cap * 7u)
    ppf_map_grow(p);

  const u32 h = ppf_map_hash(sector);
  u32 mask = (u32)(p->map_cap - 1);
  u32 idx = h & mask;
  while (p->map[idx].used) {
    if (p->map[idx].sector == sector) { *inserted = false; return &p->map[idx].offset; }
    idx = (idx + 1) & mask;
  }
  p->map[idx].used   = true;
  p->map[idx].sector = sector;
  p->map[idx].offset = 0;
  p->map_count++;
  *inserted = true;
  return &p->map[idx].offset;
}

static const u32* ppf_map_find(const cd_image_ppf_t* p, u32 sector)
{
  if (p->map_cap == 0) return NULL;
  const u32 h = ppf_map_hash(sector);
  u32 mask = (u32)(p->map_cap - 1);
  u32 idx = h & mask;
  while (p->map[idx].used) {
    if (p->map[idx].sector == sector) return &p->map[idx].offset;
    idx = (idx + 1) & mask;
  }
  return NULL;
}

static void ppf_map_grow(cd_image_ppf_t* p)
{
  size_t new_cap = p->map_cap ? p->map_cap * 2 : 64u;
  ppf_map_entry_t* new_map = (ppf_map_entry_t*)calloc(new_cap, sizeof(*new_map));
  if (!new_map) abort();

  if (p->map) {
    u32 mask = (u32)(new_cap - 1);
    for (size_t i = 0; i < p->map_cap; i++) {
      if (!p->map[i].used) continue;
      u32 h = ppf_map_hash(p->map[i].sector);
      u32 idx = h & mask;
      while (new_map[idx].used) idx = (idx + 1) & mask;
      new_map[idx] = p->map[i];
    }
    free(p->map);
  }
  p->map = new_map;
  p->map_cap = new_cap;
}

static bool add_patch(cd_image_ppf_t* p, u64 offset, const u8* patch, u32 patch_size,
                      const u8* undo_data, u32 undo_size, Error* error)
{
  (void)undo_size; /* asserted == patch_size by caller */
  u32 remaining = patch_size;
  u32 patch_offset = 0;
  while (remaining > 0) {
    const u32 sector_index = (u32)(offset / PPF_RAW_SECTOR_SIZE) + p->replacement_offset;
    const u32 sector_offset = (u32)(offset % PPF_RAW_SECTOR_SIZE);
    if (sector_index >= cd_image_get_lba_count(p->parent)) {
      WARNING_LOG("Ignoring out-of-range sector %u (max %u)", sector_index, cd_image_get_lba_count(p->parent));
      return true;
    }

    const u32 bytes_to_patch = (remaining < (PPF_RAW_SECTOR_SIZE - sector_offset))
                                   ? remaining : (PPF_RAW_SECTOR_SIZE - sector_offset);

    bool inserted;
    u32* slot_offset = ppf_map_lookup_or_insert(p, sector_index, &inserted);
    if (inserted) {
      const u32 replacement_buffer_start = (u32)p->replacement_size;
      u8* dest = ppf_grow_replacement(p, PPF_RAW_SECTOR_SIZE);
      if (!cd_image_seek_lba(p->parent, sector_index)
          || !cd_image_read_raw_sector(p->parent, dest, NULL)) {
        Error_set_string_fmt(error, "Failed to read sector %u from parent image", sector_index);
        return false;
      }
      *slot_offset = replacement_buffer_start;
    }

    u8* dst_sector = p->replacement_data + *slot_offset;
    if (undo_data) {
      if (memcmp(undo_data + patch_offset, dst_sector + sector_offset, bytes_to_patch) != 0) {
        WARNING_LOG("Original file data does not match undo data for patch at offset %llu size %u",
                    (unsigned long long)offset, bytes_to_patch);
      }
    }
    memcpy(dst_sector + sector_offset, patch + patch_offset, bytes_to_patch);
    offset += bytes_to_patch;
    patch_offset += bytes_to_patch;
    remaining -= bytes_to_patch;
  }
  return true;
}

static u32 read_file_id_diz(FILE* fp, u32 version)
{
  const int lenidx = (version == 2) ? 4 : 2;

  u32 magic;
  if (fseek(fp, -(lenidx + 4), SEEK_END) != 0 || fread(&magic, sizeof(magic), 1, fp) != 1) {
    WARNING_LOG("Failed to read diz magic");
    return 0;
  }
  if (magic != 0x5A49442Eu) /* .DIZ */
    return 0;

  u32 dlen = 0;
  if (fseek(fp, -lenidx, SEEK_END) != 0 || fread(&dlen, lenidx, 1, fp) != 1) {
    WARNING_LOG("Failed to read diz length");
    return 0;
  }
  if (dlen > (u32)ftell(fp)) {
    WARNING_LOG("diz length out of range");
    return 0;
  }

  if (fseek(fp, -(lenidx + 16 + (long)dlen), SEEK_END) != 0) {
    WARNING_LOG("Failed to seek for diz");
    return 0;
  }
  return dlen;
}

static bool read_v1_patch(cd_image_ppf_t* p, FILE* fp, Error* error)
{
  char desc[PPF_DESC_SIZE + 1] = { 0 };
  if (fseek(fp, 6, SEEK_SET) != 0 || fread(desc, 1, PPF_DESC_SIZE, fp) != PPF_DESC_SIZE) {
    Error_set_errno_prefix(error, "Failed to read description: ", errno);
    return false;
  }

  u32 filelen;
  if (fseek(fp, 0, SEEK_END) != 0 || (filelen = (u32)ftell(fp)) == 0 || filelen < 56) {
    Error_set_errno_prefix(error, "Invalid ppf file: ", errno);
    return false;
  }

  u32 count = filelen - 56u;
  if (count == 0) {
    Error_set_string(error, "Invalid count/filelen");
    return false;
  }
  if (fseek(fp, 56, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "Failed to seek to patch data: ", errno);
    return false;
  }

  u8* temp = NULL; u32 temp_cap = 0;
  while (count > 0) {
    u32 offset; u8 chunk_size;
    if (fread(&offset, sizeof(offset), 1, fp) != 1
        || fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1) {
      Error_set_errno_prefix(error, "Incomplete ppf: ", errno);
      free(temp);
      return false;
    }
    if (chunk_size > temp_cap) {
      temp = (u8*)realloc(temp, chunk_size);
      if (!temp) abort();
      temp_cap = chunk_size;
    }
    if (fread(temp, 1, chunk_size, fp) != chunk_size) {
      Error_set_errno_prefix(error, "Failed to read patch data: ", errno);
      free(temp);
      return false;
    }
    if (!add_patch(p, offset, temp, chunk_size, NULL, 0, error)) { free(temp); return false; }
    count -= (u32)sizeof(offset) + (u32)sizeof(chunk_size) + chunk_size;
  }
  free(temp);
  INFO_LOG("Loaded %zu replacement sectors from version 1 PPF", p->map_count);
  return true;
}

static bool read_v2_patch(cd_image_ppf_t* p, FILE* fp, Error* error)
{
  char desc[PPF_DESC_SIZE + 1] = { 0 };
  if (fseek(fp, 6, SEEK_SET) != 0 || fread(desc, 1, PPF_DESC_SIZE, fp) != PPF_DESC_SIZE) {
    Error_set_errno_prefix(error, "Failed to read description: ", errno);
    return false;
  }
  INFO_LOG("Patch description: %s", desc);

  const u32 idlen = read_file_id_diz(fp, 2);

  u32 origlen;
  if (fseek(fp, 56, SEEK_SET) != 0 || fread(&origlen, sizeof(origlen), 1, fp) != 1) {
    Error_set_errno_prefix(error, "Failed to read size: ", errno);
    return false;
  }
  (void)origlen;

  /* Blockcheck: 1024 bytes at offset 32 of sector (16 + replacement_offset). */
  u8* bc = (u8*)malloc(PPF_BLOCKCHECK_SIZE);
  if (!bc) abort();
  if (fread(bc, 1, PPF_BLOCKCHECK_SIZE, fp) != PPF_BLOCKCHECK_SIZE) {
    Error_set_errno_prefix(error, "Failed to read blockcheck data: ", errno);
    free(bc);
    return false;
  }
  {
    u8 src[PPF_RAW_SECTOR_SIZE];
    const u32 blockcheck_src_sector = 16u + p->replacement_offset;
    if (cd_image_seek_lba(p->parent, blockcheck_src_sector)
        && cd_image_read_raw_sector(p->parent, src, NULL)) {
      if (memcmp(&src[32], bc, PPF_BLOCKCHECK_SIZE) != 0)
        WARNING_LOG("Blockcheck failed. The patch may not apply correctly.");
    } else {
      WARNING_LOG("Failed to read blockcheck sector %u", blockcheck_src_sector);
    }
  }
  free(bc);

  u32 filelen;
  if (fseek(fp, 0, SEEK_END) != 0 || (filelen = (u32)ftell(fp)) == 0 || filelen < 1084) {
    Error_set_errno_prefix(error, "Invalid ppf file: ", errno);
    return false;
  }

  u32 count = filelen - 1084u;
  if (idlen > 0) {
    const u32 extra = idlen + 38u;
    if (count <= extra) return false;
    count -= extra;
  }
  if (count == 0) return false;
  if (fseek(fp, 1084, SEEK_SET) != 0) return false;

  u8* temp = NULL; u32 temp_cap = 0;
  while (count > 0) {
    u32 offset; u8 chunk_size;
    if (fread(&offset, sizeof(offset), 1, fp) != 1
        || fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1) {
      Error_set_errno_prefix(error, "Incomplete ppf: ", errno);
      free(temp);
      return false;
    }
    if (chunk_size > temp_cap) {
      temp = (u8*)realloc(temp, chunk_size);
      if (!temp) abort();
      temp_cap = chunk_size;
    }
    if (fread(temp, 1, chunk_size, fp) != chunk_size) {
      Error_set_errno_prefix(error, "Failed to read patch data: ", errno);
      free(temp);
      return false;
    }
    if (!add_patch(p, offset, temp, chunk_size, NULL, 0, error)) { free(temp); return false; }
    count -= (u32)sizeof(offset) + (u32)sizeof(chunk_size) + chunk_size;
  }
  free(temp);
  INFO_LOG("Loaded %zu replacement sectors from version 2 PPF", p->map_count);
  return true;
}

static bool read_v3_patch(cd_image_ppf_t* p, FILE* fp, Error* error)
{
  char desc[PPF_DESC_SIZE + 1] = { 0 };
  if (fseek(fp, 6, SEEK_SET) != 0 || fread(desc, 1, PPF_DESC_SIZE, fp) != PPF_DESC_SIZE) {
    Error_set_errno_prefix(error, "Failed to read description: ", errno);
    return false;
  }
  INFO_LOG("Patch description: %s", desc);

  const u32 idlen = read_file_id_diz(fp, 3);

  u8 image_type, block_check, undo;
  if (fseek(fp, 56, SEEK_SET) != 0
      || fread(&image_type, sizeof(image_type), 1, fp) != 1
      || fread(&block_check, sizeof(block_check), 1, fp) != 1 
      || fread(&undo, sizeof(undo), 1, fp) != 1) {
    Error_set_errno_prefix(error, "Failed to read headers: ", errno);
    return false;
  }
  (void)image_type;
  /* TODO: blockcheck */

  fseek(fp, 0, SEEK_END);
  u32 count = (u32)ftell(fp);

  u32 seekpos = block_check ? 1084u : 60u;
  if (seekpos >= count) {
    Error_set_string(error, "File is too short");
    return false;
  }
  count -= seekpos;
  if (idlen > 0) {
    const u32 extralen = idlen + 18u + 16u + 2u;
    if (count < extralen) {
      Error_set_string(error, "File is too short (diz)");
      return false;
    }
    count -= extralen;
  }
  if (fseek(fp, seekpos, SEEK_SET) != 0) {
    Error_set_errno_prefix(error, "Failed to seek to patch data: ", errno);
    return false;
  }

  u8* temp = NULL; u32 temp_cap = 0;
  while (count > 0) {
    u64 offset; u8 chunk_size;
    if (fread(&offset, sizeof(offset), 1, fp) != 1
        || fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1) {
      Error_set_errno_prefix(error, "Incomplete ppf: ", errno);
      free(temp);
      return false;
    }
    const u32 read_chunk_size = undo ? (u32)chunk_size * 2u : (u32)chunk_size;
    if (read_chunk_size > temp_cap) {
      temp = (u8*)realloc(temp, read_chunk_size);
      if (!temp) abort();
      temp_cap = read_chunk_size;
    }
    if (fread(temp, 1, read_chunk_size, fp) != read_chunk_size) {
      Error_set_errno_prefix(error, "Failed to read patch data: ", errno);
      free(temp);
      return false;
    }

    const u8* patch_ptr = temp;
    const u8* undo_ptr  = NULL;
    if (undo) undo_ptr = temp + chunk_size;

    if (!add_patch(p, offset, patch_ptr, chunk_size, undo_ptr, chunk_size, error)) { free(temp); return false; }
    count -= (u32)sizeof(offset) + (u32)sizeof(chunk_size) + read_chunk_size;
  }
  free(temp);
  INFO_LOG("Loaded %zu replacement sectors from version 3 PPF", p->map_count);
  return true;
}

static bool cdimage_ppf_read_sector_from_index(cd_image_t* self, void* buffer,
                                               const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  const u32 sector_number = index->start_lba_on_disc + lba_in_index;
  const u32* slot = ppf_map_find(p, sector_number);
  if (!slot)
    return p->parent->vtbl->read_sector_from_index(p->parent, buffer, index, lba_in_index);
  memcpy(buffer, p->replacement_data + *slot, PPF_RAW_SECTOR_SIZE);
  return true;
}

static bool cdimage_ppf_read_subchannel_q(cd_image_t* self, cd_image_subq_t* subq,
                                          const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  return cd_image_read_subchannel_q(p->parent, subq, index, lba_in_index);
}

static bool cdimage_ppf_has_subchannel_data(const cd_image_t* self)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  return cd_image_has_subchannel_data(p->parent);
}

static cd_image_precache_result_t cdimage_ppf_precache(cd_image_t* self, Error* error)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  return cd_image_precache(p->parent, error);
}

static bool cdimage_ppf_is_precached(const cd_image_t* self)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  return cd_image_is_precached(p->parent);
}

static s64 cdimage_ppf_get_size_on_disk(const cd_image_t* self)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  return p->patch_size + cd_image_get_size_on_disk(p->parent);
}

static char* cdimage_ppf_get_sub_image_title(const cd_image_t* self, u32 index)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  if (index != 0) return NULL;
  return cd_image_get_sub_image_title(p->parent, 0);
}

static void cdimage_ppf_destroy(cd_image_t* self)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)self;
  if (p->parent) {
    cd_image_destroy(p->parent);
    p->parent = NULL;
  }
  free(p->replacement_data);
  p->replacement_data = NULL;
  p->replacement_size = p->replacement_cap = 0;
  free(p->map);
  p->map = NULL;
  p->map_cap = p->map_count = 0;
}

static const cd_image_vtable_t s_ppf_vtable = {
   .read_sector_from_index = cdimage_ppf_read_sector_from_index,
  .read_subchannel_q      = cdimage_ppf_read_subchannel_q,
  .has_subchannel_data    = cdimage_ppf_has_subchannel_data,
  .precache               = cdimage_ppf_precache,
  .is_precached           = cdimage_ppf_is_precached,
  .get_size_on_disk       = cdimage_ppf_get_size_on_disk,
  .destroy                = cdimage_ppf_destroy,
  .get_sub_image_title    = cdimage_ppf_get_sub_image_title, 
};

cd_image_t* cd_image_overlay_ppf(const char* ppf_path, cd_image_t* parent, Error* error)
{
  cd_image_ppf_t* p = (cd_image_ppf_t*)malloc(sizeof(*p));
  if (!p) abort();
  memset(p, 0, sizeof(*p));
  cd_image_init(&p->base, &s_ppf_vtable);

  /* Adopt parent immediately so all error paths uniformly destroy via
   * cd_image_destroy(&p->base) → cdimage_ppf_destroy → cd_image_destroy(parent). */
  p->parent = parent;

  /* Adopt parent's TOC + filename. */
  cd_image_copy_toc(&p->base, parent);
  if (p->base.filename) { free(p->base.filename); p->base.filename = NULL; }
  const char* parent_path = cd_image_get_path(parent);
  if (parent_path && *parent_path) {
    p->base.filename = strdup(parent_path);
    if (!p->base.filename) abort();
  }

  /* For data tracks, replacements are offset by track-1's start_lba (the
   * 150-LBA implicit pregap on most discs). */
  const cd_image_track_t* t1 = cd_image_get_track(parent, 1);
  if (t1 && t1->mode != CD_IMAGE_TRACK_MODE_AUDIO) {
    const cd_image_index_t* i1 = cd_image_get_index(parent, 1);
    if (i1) p->replacement_offset = i1->start_lba_on_disc;
  }

  FILE* fp = fs_open_shared_file(ppf_path, "rb", FS_FILE_SHARE_DENY_WRITE, error);
  if (!fp) {
    cd_image_destroy(&p->base);
    return NULL;
  }
  p->patch_size = fs_fsize64(fp, NULL);

  u32 magic;
  if (fread(&magic, sizeof(magic), 1, fp) != 1) {
    Error_set_errno_prefix(error, "Failed to read PPF magic: ", errno);
    fclose(fp);
    cd_image_destroy(&p->base);
    return NULL;
  }

  bool ok = false;
  if (magic == 0x33465050u)      ok = read_v3_patch(p, fp, error); /* PPF3 */
  else if (magic == 0x32465050u) ok = read_v2_patch(p, fp, error); /* PPF2 */
  else if (magic == 0x31465050u) ok = read_v1_patch(p, fp, error); /* PPF1 */
  else                           Error_set_string_fmt(error, "Unknown PPF magic %08X", magic);
  fclose(fp);

  if (!ok) {
    cd_image_destroy(&p->base);
    return NULL;
  }
  return &p->base;
}
