#ifndef CUPID_CORE_SAVE_STATE_VERSION_H
#define CUPID_CORE_SAVE_STATE_VERSION_H

#include "common/types.h"

#define SAVE_STATE_MAGIC           0x43435544u  /* 'DUCC' little-endian */
#define SAVE_STATE_VERSION         84u
#define SAVE_STATE_MINIMUM_VERSION 42u

_Static_assert(SAVE_STATE_VERSION >= SAVE_STATE_MINIMUM_VERSION,
               "save state version below minimum supported");

enum {
  SAVE_STATE_MAX_TITLE_LENGTH    = 128,
  SAVE_STATE_MAX_SERIAL_LENGTH   = 32,
  SAVE_STATE_MAX_SAVE_STATE_SIZE = 32 * 1024 * 1024,
};

typedef enum : u32 {
  SAVE_STATE_COMPRESSION_TYPE_NONE      = 0,
  SAVE_STATE_COMPRESSION_TYPE_DEFLATE   = 1,
  SAVE_STATE_COMPRESSION_TYPE_ZSTANDARD = 2,
  SAVE_STATE_COMPRESSION_TYPE_XZ        = 3,
} save_state_compression_type_t;

#pragma pack(push, 4)
typedef struct {
  u32  magic;
  u32  version;
  char title[SAVE_STATE_MAX_TITLE_LENGTH];
  char serial[SAVE_STATE_MAX_SERIAL_LENGTH];

  u32 media_path_length;
  u32 offset_to_media_path;
  u32 media_subimage_index;

  /* Screenshot compression added in version 69. Uncompressed size is implied
   * by width * height. */
  u32 screenshot_compression_type;
  u32 screenshot_width;
  u32 screenshot_height;
  u32 screenshot_compressed_size;
  u32 offset_to_screenshot;

  u32 data_compression_type;
  u32 data_compressed_size;
  u32 data_uncompressed_size;
  u32 offset_to_data;
} save_state_header_t;
#pragma pack(pop)

#endif /* CUPID_CORE_SAVE_STATE_VERSION_H */
