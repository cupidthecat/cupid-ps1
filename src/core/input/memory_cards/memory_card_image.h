/*
 * 128KB on-disk format used by the PS1's memory cards.  The C++ namespace
 * MemoryCardImage:: collapses to a flat memory_card_image_* prefix; the
 * 128KiB DataArray is just a plain byte array (callers stash one in their
 * struct or pass a pointer in).
 *
 * std::vector<FileInfo> from EnumerateFiles() becomes a heap (arr, count)
 * pair plus memory_card_image_free_file_info_array(); each entry holds
 * fixed-size title / filename buffers (PS1 caps at 21 bytes for the dir
 * entry name and 64 bytes for the SJIS title, decoded to UTF-8 here; 192
 * bytes is enough headroom for the worst-case 3-byte UTF-8 expansion).
 *
 * std::span<u8> import buffers become (data, len) pairs.  Error reporting
 * uses Error*; NULL is tolerated everywhere.
 */

#ifndef CUPID_CORE_MEMORY_CARD_IMAGE_H
#define CUPID_CORE_MEMORY_CARD_IMAGE_H

#include "common/types.h"

#include <stddef.h>

typedef struct Error Error;

enum {
  MEMORY_CARD_IMAGE_DATA_SIZE        = 128 * 1024,                      /* 1 Mbit */
  MEMORY_CARD_IMAGE_BLOCK_SIZE       = 8192,
  MEMORY_CARD_IMAGE_FRAME_SIZE       = 128,
  MEMORY_CARD_IMAGE_FRAMES_PER_BLOCK = MEMORY_CARD_IMAGE_BLOCK_SIZE / MEMORY_CARD_IMAGE_FRAME_SIZE,
  MEMORY_CARD_IMAGE_NUM_BLOCKS       = MEMORY_CARD_IMAGE_DATA_SIZE / MEMORY_CARD_IMAGE_BLOCK_SIZE,
  MEMORY_CARD_IMAGE_NUM_FRAMES       = MEMORY_CARD_IMAGE_DATA_SIZE / MEMORY_CARD_IMAGE_FRAME_SIZE,

  MEMORY_CARD_IMAGE_ICON_WIDTH  = 16,
  MEMORY_CARD_IMAGE_ICON_HEIGHT = 16,

  MEMORY_CARD_IMAGE_FILE_REGION_LENGTH   = 2,
  MEMORY_CARD_IMAGE_FILE_SERIAL_LENGTH   = 10,
  MEMORY_CARD_IMAGE_FILE_FILENAME_LENGTH = 8,
  MEMORY_CARD_IMAGE_FILE_TOTAL_LENGTH    = MEMORY_CARD_IMAGE_FILE_REGION_LENGTH +
                                           MEMORY_CARD_IMAGE_FILE_SERIAL_LENGTH +
                                           MEMORY_CARD_IMAGE_FILE_FILENAME_LENGTH,

  /* DirectoryFrame::filename in MCD is region(2) + serial(10) + name(8) + NUL = 21. */
  MEMORY_CARD_IMAGE_FILENAME_BUFFER_SIZE = MEMORY_CARD_IMAGE_FILE_TOTAL_LENGTH + 1,

  /* TitleFrame::title is 64 bytes Shift-JIS; UTF-8 expansion can be up to 3x. */
  MEMORY_CARD_IMAGE_TITLE_BUFFER_SIZE = 192 + 1,

  /* A save spans at most FRAMES_PER_BLOCK-1 = 15 directory entries. */
  MEMORY_CARD_IMAGE_MAX_ICON_FRAMES = 3,
};

typedef struct memory_card_image_icon_frame {
  u32 pixels[MEMORY_CARD_IMAGE_ICON_WIDTH * MEMORY_CARD_IMAGE_ICON_HEIGHT];
} memory_card_image_icon_frame_t;

typedef struct memory_card_image_file_info {
  char filename[MEMORY_CARD_IMAGE_FILENAME_BUFFER_SIZE];
  char title[MEMORY_CARD_IMAGE_TITLE_BUFFER_SIZE];
  u32  size;
  u32  first_block;
  u32  num_blocks;
  bool deleted;

  /* Inline icon storage; PS1 supports at most 3 animation frames. */
  u32                            icon_frame_count;
  memory_card_image_icon_frame_t icon_frames[MEMORY_CARD_IMAGE_MAX_ICON_FRAMES];
} memory_card_image_file_info_t;

bool memory_card_image_load_from_file(u8* data, const char* filename, Error* error);
bool memory_card_image_save_to_file  (const u8* data, const char* filename, Error* error);

void memory_card_image_format(u8* data);
bool memory_card_image_is_valid(const u8* data);

u32 memory_card_image_get_free_block_count(const u8* data);

 /* On success: *out_files heap-allocated, *out_count set, returns true.
 * Free with memory_card_image_free_file_info_array(); pass count==0 if you
 * want to free a known-empty list. */
bool memory_card_image_enumerate_files(const u8* data, bool include_deleted,
                                       memory_card_image_file_info_t** out_files, size_t* out_count);
void memory_card_image_free_file_info_array(memory_card_image_file_info_t* arr, size_t count);

bool memory_card_image_read_file (const u8* data, const memory_card_image_file_info_t* fi,
                                  u8** out_data, size_t* out_len, Error* error);
bool memory_card_image_write_file(u8* data,
                                  const char* filename_data, u32 filename_len,
                                  const u8* buffer, size_t buffer_len, Error* error);
bool memory_card_image_delete_file  (u8* data, const memory_card_image_file_info_t* fi, bool clear_sectors);
bool memory_card_image_undelete_file(u8* data, const memory_card_image_file_info_t* fi);
bool memory_card_image_rename_file  (u8* data, const memory_card_image_file_info_t* fi,
                                     const char* new_filename_data, u32 new_filename_len, Error* error);

bool memory_card_image_import_card_path (u8* data, const char* filename, Error* error);
bool memory_card_image_import_card_bytes(u8* data, const char* filename,
                                         const u8* file_data, size_t file_data_len, Error* error);

bool memory_card_image_export_save(u8* data, const memory_card_image_file_info_t* fi,
                                   const char* filename, Error* error);
bool memory_card_image_import_save(u8* data, const char* filename, Error* error);

#endif /* CUPID_CORE_MEMORY_CARD_IMAGE_H */
