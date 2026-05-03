/*
 * In-memory CD image: copies all sectors from another cd_image_t into RAM,
 * then serves reads from the buffer.  Used for pre-caching small images so
 * the rest of the emulator never blocks on disk I/O.
 */

#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/progress_callback.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDImage);

typedef struct {
  cd_image_t base;
  u8*        memory;
  u32        memory_sectors;
} cd_image_memory_t;

static bool memory_read_sector_from_index(cd_image_t* self, void* buffer,
                                          const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_memory_t* m = (cd_image_memory_t*)self;
  DebugAssert(index->file_index == 0);
  const u64 sector_number = index->file_offset + lba_in_index;
  if (sector_number >= m->memory_sectors) return false;
  const size_t off = (size_t)sector_number * (size_t)CD_IMAGE_RAW_SECTOR_SIZE;
  memcpy(buffer, &m->memory[off], CD_IMAGE_RAW_SECTOR_SIZE);
  return true;
}

static bool memory_is_precached(const cd_image_t* self) { (void)self; return true; }

static void memory_destroy(cd_image_t* self)
{
  cd_image_memory_t* m = (cd_image_memory_t*)self;
  free(m->memory);
  m->memory         = NULL;
  m->memory_sectors = 0;
}

static const cd_image_vtable_t s_memory_vtable = {
  memory_read_sector_from_index,
  NULL,
  NULL,                /* has_subchannel_data */
  NULL,                /* precache */
  memory_is_precached,
  NULL,                /* get_size_on_disk */
  memory_destroy,
  NULL,                /* has_sub_images */
  NULL,                /* get_sub_image_count */
  NULL,                /* get_current_sub_image */
  NULL,                /* get_sub_image_title */
  NULL,                /* switch_sub_image */
};

static cd_image_position_t s_zero_pos = { 0, 0, 0 };

cd_image_t* cd_image_create_memory_image(cd_image_t* src, progress_callback_t* pc, Error* err)
{
  cd_image_memory_t* m = (cd_image_memory_t*)calloc(1, sizeof(*m));
  if (!m) {
    Error_set_string(err, "out of memory");
    return NULL;
  }
  m->base.vtbl = &s_memory_vtable;

  /* Tally sectors actually backed by file data (non-pregap indices). */
  u32 total_sectors = 0;
  for (u32 i = 0; i < cd_image_get_index_count(src); i++) {
    const cd_image_index_t* idx = cd_image_get_index(src, i);
    if (idx->file_sector_size > 0)
      total_sectors += idx->length;
  }
  if (total_sectors == 0 ||
      (u64)CD_IMAGE_RAW_SECTOR_SIZE * (u64)total_sectors >= (u64)SIZE_MAX) {
    Error_set_string(err, "Insufficient address space");
    free(m);
    return NULL;
  }
  m->memory_sectors = total_sectors;

  progress_callback_set_status_text_fmt(pc, "Allocating memory for %u sectors...", total_sectors);
  m->memory = (u8*)malloc((size_t)CD_IMAGE_RAW_SECTOR_SIZE * total_sectors);
  if (!m->memory) {
    Error_set_string_fmt(err, "Failed to allocate memory for %u sectors", total_sectors);
    free(m);
    return NULL;
  }

  static const char title[] = "Preloading CD image to RAM...";
  progress_callback_set_title(pc, title, sizeof(title) - 1);
  progress_callback_set_progress_range(pc, total_sectors);
  progress_callback_set_progress_value(pc, 0);

  u8* mp = m->memory;
  u32 read = 0;
  for (u32 i = 0; i < cd_image_get_index_count(src); i++) {
    const cd_image_index_t* idx = cd_image_get_index(src, i);
    if (idx->file_sector_size == 0) continue;
    for (u32 lba = 0; lba < idx->length; lba++) {
      if (!cd_image_read_sector_from_index(src, mp, idx, lba)) {
        ERROR_LOG("Failed to read LBA %u in index %u", lba, i);
        free(m->memory);
        free(m);
        return NULL;
      }
      progress_callback_set_progress_value(pc, read);
      mp += CD_IMAGE_RAW_SECTOR_SIZE;
      read++;
    }
  }

  /* Copy track + index tables into the new image. */
  for (u32 i = 1; i <= cd_image_get_track_count(src); i++) {
    const cd_image_track_t* src_t = cd_image_get_track(src, i);
    cd_image_track_t* dst_t = cd_image_push_track(&m->base);
    *dst_t = *src_t;
  }

  u32 cur_off = 0;
  for (u32 i = 0; i < cd_image_get_index_count(src); i++) {
    const cd_image_index_t* src_i = cd_image_get_index(src, i);
    cd_image_index_t* dst_i = cd_image_push_index(&m->base);
    *dst_i = *src_i;
    dst_i->file_index = 0;
    if (dst_i->file_sector_size > 0) {
      dst_i->file_offset = cur_off;
      cur_off           += dst_i->length;
    }
  }
  Assert(cur_off == m->memory_sectors);

  /* Inherit path + total length. */
  const char* src_path = cd_image_get_path(src);
  if (src_path && *src_path) {
    size_t n = strlen(src_path);
    m->base.filename = (char*)malloc(n + 1);
    if (m->base.filename) memcpy(m->base.filename, src_path, n + 1);
  }
  m->base.lba_count = cd_image_get_lba_count(src);

  if (!cd_image_seek_track_msf(&m->base, 1, s_zero_pos)) {
    Error_set_string(err, "Initial seek failed");
    memory_destroy(&m->base);
    free(m->base.filename);
    free(m);
    return NULL;
  }
  return &m->base;
}
