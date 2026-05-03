/*
 * Implements the on-disk MCD layout (header + 15 directory frames + file
 * blocks) plus the GME/VGS/PSX/MCS importers consumers may want to feed in.
 * std::vector<FileInfo> becomes a heap (arr, count) pair allocated with
 * malloc/realloc; importers/exporters use the file_system.h helpers.
 */

#include "core/memory_card_image.h"

#include "util/shiftjis.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/types.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(MemoryCard);

#define DATA_SIZE        ((u32)MEMORY_CARD_IMAGE_DATA_SIZE)
#define BLOCK_SIZE       ((u32)MEMORY_CARD_IMAGE_BLOCK_SIZE)
#define FRAME_SIZE       ((u32)MEMORY_CARD_IMAGE_FRAME_SIZE)
#define FRAMES_PER_BLOCK ((u32)MEMORY_CARD_IMAGE_FRAMES_PER_BLOCK)
#define NUM_BLOCKS       ((u32)MEMORY_CARD_IMAGE_NUM_BLOCKS)
#define NUM_FRAMES       ((u32)MEMORY_CARD_IMAGE_NUM_FRAMES)
#define FILE_TOTAL_LEN   ((u32)MEMORY_CARD_IMAGE_FILE_TOTAL_LENGTH)
#define ICON_W           ((u32)MEMORY_CARD_IMAGE_ICON_WIDTH)
#define ICON_H           ((u32)MEMORY_CARD_IMAGE_ICON_HEIGHT)

/* MCD raw file size for legacy raw-import limit (matches DirectoryFrame). */
#define DIRECTORY_FRAME_FILE_NAME_LENGTH 20u

#pragma pack(push, 1)
typedef struct directory_frame {
  u32 block_allocation_state;
  u32 file_size;
  u16 next_block_number;
  char filename[FILE_TOTAL_LEN + 1];
  u8 zero_pad_1;
  u8 pad_2[95];
  u8 checksum;
} directory_frame_t;

typedef struct title_frame {
  char id[2];
  u8 icon_flag;
  u8 unk_block_number;
  u8 title[64];
  u8 pad_1[12];
  u8 pad_2[16];
  u16 icon_palette[16];
} title_frame_t;
#pragma pack(pop)

_Static_assert(sizeof(directory_frame_t) == MEMORY_CARD_IMAGE_FRAME_SIZE,
               "directory_frame_t must be 128 bytes");
_Static_assert(sizeof(title_frame_t) == MEMORY_CARD_IMAGE_FRAME_SIZE,
               "title_frame_t must be 128 bytes");

static inline u8* frame_ptr_mut(u8* data, u32 block, u32 frame)
{
  return data + (block * BLOCK_SIZE) + (frame * FRAME_SIZE);
}

static inline const u8* frame_ptr(const u8* data, u32 block, u32 frame)
{
  return data + (block * BLOCK_SIZE) + (frame * FRAME_SIZE);
}

static inline directory_frame_t* dir_ptr_mut(u8* data, u32 frame)
{
  return (directory_frame_t*)frame_ptr_mut(data, 0, frame);
}

static inline const directory_frame_t* dir_ptr(const u8* data, u32 frame)
{
  return (const directory_frame_t*)frame_ptr(data, 0, frame);
}

static inline const title_frame_t* title_ptr(const u8* data, u32 block)
{
  return (const title_frame_t*)frame_ptr(data, block, 0);
}

/* Per-frame XOR checksum across bytes [0, FRAME_SIZE-1).  Used by the
 * directory / header / broken-sector frames; the byte at FRAME_SIZE-1 stores
 * the result. */
static u8 frame_checksum(const u8* frame)
{
  u8 c = frame[0];
  for (u32 i = 1; i < FRAME_SIZE - 1u; i++)
    c ^= frame[i];
  return c;
}

static void update_directory_checksum(directory_frame_t* df)
{
  df->checksum = frame_checksum((const u8*)df);
}

/* Mirrors VRAMRGBA5551ToRGBA8888 in gpu_helpers.h.  The 527/23/>>6 magic
 * expands a 5-bit channel to 8 bits without divisions. */
static inline u32 rgba5551_to_rgba8888(u32 color)
{
#define E5TO8(c) ((((c) * 527u) + 23u) >> 6)
  const u32 r = E5TO8(color & 31u);
  const u32 g = E5TO8((color >> 5) & 31u);
  const u32 b = E5TO8((color >> 10) & 31u);
  const u32 a = ((color >> 15) != 0u) ? 255u : 0u;
  return r | (g << 8) | (b << 16) | (a << 24);
#undef E5TO8
}

/* Some games leave the alpha bit at 0 even for opaque pixels; treat the
 * single transparency cue as "all bits zero"; otherwise force opaque. */
static inline u32 icon_color_to_rgba8(u16 col)
{
  return (col == 0) ? 0u : rgba5551_to_rgba8888((u32)col | 0x8000u);
}

static bool import_card_mcd(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error);
static bool import_card_gme(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error);
static bool import_card_vgs(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error);
static bool import_card_psx(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error);

static bool import_save_with_directory_frame(u8* data, const char* filename, s64 file_size, Error* error);
static bool import_raw_save                 (u8* data, const char* filename, s64 file_size, Error* error);

bool memory_card_image_load_from_file(u8* data, const char* filename, Error* error)
{
  FILE* fp = fs_open_file(filename, "rb", error);
  if (!fp)
    return false;

  const s64 size = fs_fsize64(fp, error);
  if (size != (s64)DATA_SIZE)
  {
    const char* fname_d;
    u32 fname_l;
    path_get_file_name_cstr(filename, &fname_d, &fname_l);
    Error_set_string_fmt(error, "Memory card %.*s is incorrect size (expected %u got %lld)",
                         (int)fname_l, fname_d, (unsigned)DATA_SIZE, (long long)size);
    fclose(fp);
    return false;
  }

  const size_t num_read = fread(data, 1, DATA_SIZE, fp);
  fclose(fp);
  if (num_read != DATA_SIZE)
  {
    Error_set_string_fmt(error, "Only read %zu of %u sectors from '%s'",
                         num_read / FRAME_SIZE, (unsigned)NUM_FRAMES, filename);
    return false;
  }

  VERBOSE_LOG("Loaded memory card from %s", filename);
  return true;
}

bool memory_card_image_save_to_file(const u8* data, const char* filename, Error* error)
{
  /* Atomic-rename helper isn't ported yet; a plain write is good enough for
   * the SaveIfChanged path; on partial failure the OSD layer surfaces it. */
  if (!fs_write_binary_file(filename, data, DATA_SIZE, error))
  {
    const char* fname_d;
    u32 fname_l;
    path_get_file_name_cstr(filename, &fname_d, &fname_l);
    ERROR_LOG("Failed to save memory card '%.*s'", (int)fname_l, fname_d);
    return false;
  }

  return true;
}

bool memory_card_image_is_valid(const u8* data)
{
  /* Only the magic is checked; per-frame XOR checksums are not enforced
   * (some cards in the wild have one bad checksum byte and otherwise work). */
  const u8* fptr = frame_ptr(data, 0, 0);
  return fptr[0] == 'M' && fptr[1] == 'C';
}

void memory_card_image_format(u8* data)
{
  /* Erased state on a real card is 0xFF.  Stamp the well-known structural
   * frames on top so the BIOS treats the card as valid. */
  memset(data, 0xFF, DATA_SIZE);

  /* Header: "MC" + zeros + checksum. */
  {
    u8* fptr = frame_ptr_mut(data, 0, 0);
    memset(fptr, 0, FRAME_SIZE);
    fptr[0] = 'M';
    fptr[1] = 'C';
    fptr[0x7F] = frame_checksum(fptr);
  }

  /* 15 directory entries: 0xA0 = "free", next-block pointer 0xFFFF. */
  for (u32 frame = 1; frame < 16; frame++)
  {
    u8* fptr = frame_ptr_mut(data, 0, frame);
    memset(fptr, 0, FRAME_SIZE);
    fptr[0] = 0xA0;
    fptr[8] = 0xFF;
    fptr[9] = 0xFF;
    fptr[0x7F] = frame_checksum(fptr);
  }

  /* 20 broken-sector list entries: all 0xFFFF / 0xFFFFFFFF. */
  for (u32 frame = 16; frame < 36; frame++)
  {
    u8* fptr = frame_ptr_mut(data, 0, frame);
    memset(fptr, 0, FRAME_SIZE);
    fptr[0] = 0xFF;
    fptr[1] = 0xFF;
    fptr[2] = 0xFF;
    fptr[3] = 0xFF;
    fptr[8] = 0xFF;
    fptr[9] = 0xFF;
    fptr[0x7F] = frame_checksum(fptr);
  }

  /* 20 broken-sector replacement frames: zero-filled. */
  for (u32 frame = 36; frame < 56; frame++)
    memset(frame_ptr_mut(data, 0, frame), 0x00, FRAME_SIZE);

  /* 7 unused frames. */
  for (u32 frame = 56; frame < 63; frame++)
    memset(frame_ptr_mut(data, 0, frame), 0x00, FRAME_SIZE);

  /* Write-test frame: copy of the header. */
  memcpy(frame_ptr_mut(data, 0, 63), frame_ptr(data, 0, 0), FRAME_SIZE);
}

/* Returns FRAMES_PER_BLOCK on no-free (sentinel; real values are 1..15). */
static u32 get_next_free_block(const u8* data)
{
  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const directory_frame_t* df = dir_ptr(data, dir_frame);
    if ((df->block_allocation_state & 0xF0u) == 0xA0u)
      return dir_frame;
  }
  return FRAMES_PER_BLOCK;
}

u32 memory_card_image_get_free_block_count(const u8* data)
{
  u32 count = 0;
  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const directory_frame_t* df = dir_ptr(data, dir_frame);
    if ((df->block_allocation_state & 0xF0u) == 0xA0u)
      count++;
  }
  return count;
}

bool memory_card_image_enumerate_files(const u8* data, bool include_deleted,
                                       memory_card_image_file_info_t** out_files, size_t* out_count)
{
  *out_files = NULL;
  *out_count = 0;

  /* Cap is 15 directory entries -> at most 15 files. */
  memory_card_image_file_info_t* arr = (memory_card_image_file_info_t*)
      calloc(FRAMES_PER_BLOCK - 1u, sizeof(memory_card_image_file_info_t));
  if (!arr)
    return false;
  size_t count = 0;

  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const directory_frame_t* df = dir_ptr(data, dir_frame);

    /* 0x51 = first block of an active save chain; 0xA1/0xA2/0xA3 = first /
     * middle / last block of a deleted (but unallocated) chain. */
    const bool is_active  = (df->block_allocation_state == 0x51);
    const bool is_deleted = (df->block_allocation_state == 0xA1 ||
                             df->block_allocation_state == 0xA2 ||
                             df->block_allocation_state == 0xA3);
    if (!is_active && !(include_deleted && is_deleted))
      continue;

    memory_card_image_file_info_t* fi = &arr[count];

    /* Filename: copy bytes up to first NUL or buffer end (max FILE_TOTAL_LEN). */
    u32 filename_length = 0;
    while (filename_length < sizeof(df->filename) && df->filename[filename_length] != '\0')
      filename_length++;
    if (filename_length > MEMORY_CARD_IMAGE_FILENAME_BUFFER_SIZE - 1u)
      filename_length = MEMORY_CARD_IMAGE_FILENAME_BUFFER_SIZE - 1u;
    memcpy(fi->filename, df->filename, filename_length);
    fi->filename[filename_length] = '\0';

    fi->first_block = dir_frame;
    fi->size        = df->file_size;
    fi->num_blocks  = 1;
    fi->deleted     = !is_active;

    /* Walk the next-block chain.  next_block_number is 0-based vs the file
     * blocks (block 1..15), so the actual frame is +1.  Bail if it loops or
     * leaves the directory area. */
    const directory_frame_t* next_df = df;
    while (next_df->next_block_number < (NUM_BLOCKS - 1u) && fi->num_blocks < FRAMES_PER_BLOCK)
    {
      fi->num_blocks++;
      next_df = dir_ptr(data, next_df->next_block_number + 1u);
    }

    if (fi->num_blocks == FRAMES_PER_BLOCK)
    {
      WARNING_LOG("Invalid block chain in block %u", dir_frame);
      continue;
    }

    /* Title frame lives at the start of the first data block. */
    const title_frame_t* tf = title_ptr(data, dir_frame);
    u32 num_icon_frames = 0;
    if      (tf->icon_flag == 0x11) num_icon_frames = 1;
    else if (tf->icon_flag == 0x12) num_icon_frames = 2;
    else if (tf->icon_flag == 0x13) num_icon_frames = 3;
    else
    {
      WARNING_LOG("Unknown icon flag 0x%02X", tf->icon_flag);
      continue;
    }

    /* Title is stored as Shift-JIS; pad with two NULs so a runaway 2-byte
     * lead doesn't read past the buffer in shiftjis_sjis2utf8. */
    char title_sjis[sizeof(tf->title) + 2];
    memcpy(title_sjis, tf->title, sizeof(tf->title));
    title_sjis[sizeof(tf->title)]     = 0;
    title_sjis[sizeof(tf->title) + 1] = 0;
    char* title_utf8 = shiftjis_sjis2utf8(title_sjis);
    if (title_utf8)
    {
      string_util_strlcpy_cstr(fi->title, title_utf8, sizeof(fi->title));
      free(title_utf8);
    }
    else
    {
      fi->title[0] = '\0';
    }

    fi->icon_frame_count = num_icon_frames;
    for (u32 icon_frame = 0; icon_frame < num_icon_frames; icon_frame++)
    {
      const u8* indices_ptr = frame_ptr(data, dir_frame, 1u + icon_frame);
      u32* pixels_ptr = fi->icon_frames[icon_frame].pixels;
      for (u32 i = 0; i < ICON_W * ICON_H; i += 2)
      {
        *(pixels_ptr++) = icon_color_to_rgba8(tf->icon_palette[*indices_ptr & 0x0Fu]);
        *(pixels_ptr++) = icon_color_to_rgba8(tf->icon_palette[*indices_ptr >> 4u]);
        indices_ptr++;
      }
    }

    count++;
  }

  *out_files = arr;
  *out_count = count;
  return true;
}

void memory_card_image_free_file_info_array(memory_card_image_file_info_t* arr, size_t count)
{
  (void)count;
  free(arr);
}

bool memory_card_image_read_file(const u8* data, const memory_card_image_file_info_t* fi,
                                 u8** out_data, size_t* out_len, Error* error)
{
  (void)error;
  const size_t buffer_size = (size_t)fi->num_blocks * BLOCK_SIZE;
  u8* buffer = (u8*)malloc(buffer_size);
  if (!buffer)
  {
    Error_set_string(error, "Out of memory");
    return false;
  }

  u32 block_number = fi->first_block;
  for (u32 i = 0; i < fi->num_blocks; i++)
  {
    if (block_number >= FRAMES_PER_BLOCK)
    {
      free(buffer);
      Error_set_string(error, "Block chain points outside the directory area");
      return false;
    }
    memcpy(buffer + ((size_t)i * BLOCK_SIZE), frame_ptr(data, block_number, 0), BLOCK_SIZE);

    const directory_frame_t* df = dir_ptr(data, block_number);
    block_number = (u32)df->next_block_number + 1u;
  }

  *out_data = buffer;
  *out_len  = buffer_size;
  return true;
}

bool memory_card_image_write_file(u8* data,
                                  const char* filename_data, u32 filename_len,
                                  const u8* buffer, size_t buffer_len, Error* error)
{
  if (buffer_len == 0)
  {
    Error_set_string(error, "Buffer is empty.");
    return false;
  }

  const u32 free_block_count = memory_card_image_get_free_block_count(data);
  const u32 num_blocks = (u32)((buffer_len + (BLOCK_SIZE - 1u)) / BLOCK_SIZE);
  if (free_block_count < num_blocks)
  {
    Error_set_string_fmt(error, "Insufficient free blocks, %u blocks are needed, but only have %u.",
                         num_blocks, free_block_count);
    return false;
  }

  if (filename_len > FILE_TOTAL_LEN)
    filename_len = FILE_TOTAL_LEN;

  directory_frame_t* last_df = NULL;
  for (u32 i = 0; i < num_blocks; i++)
  {
    const u32 block_number = get_next_free_block(data);
    if (block_number == FRAMES_PER_BLOCK)
    {
      /* Defensive; we already pre-checked the count. */
      Error_set_string(error, "Free block disappeared mid-write.");
      return false;
    }

    directory_frame_t* df = dir_ptr_mut(data, block_number);
    memset(df, 0, sizeof(*df));

    if (last_df)
    {
      /* Stamp the previous block's "next" pointer to this block-1 (0-based). */
      last_df->next_block_number = (u16)(block_number - 1u);
      update_directory_checksum(last_df);

      /* 0x53 = last block in chain; 0x52 = middle. */
      df->block_allocation_state = (i == (num_blocks - 1u)) ? 0x53u : 0x52u;
    }
    else
    {
      /* First block: 0x51, plus filename + total payload size. */
      df->block_allocation_state = 0x51u;
      df->file_size = (u32)buffer_len;
      string_util_strlcpy_view(df->filename, filename_data, filename_len, sizeof(df->filename));
    }

    df->next_block_number = 0xFFFFu;
    update_directory_checksum(df);
    last_df = df;

    u8* data_block = frame_ptr_mut(data, block_number, 0);
    const size_t src_offset = (size_t)i * BLOCK_SIZE;
    const size_t remaining  = buffer_len - src_offset;
    const size_t size_to_copy = remaining < BLOCK_SIZE ? remaining : (size_t)BLOCK_SIZE;
    const size_t size_to_zero = (size_t)BLOCK_SIZE - size_to_copy;
    memcpy(data_block, buffer + src_offset, size_to_copy);
    if (size_to_zero)
      memset(data_block + size_to_copy, 0, size_to_zero);
  }

  INFO_LOG("Wrote %zu byte (%u block) file to memory card", buffer_len, num_blocks);
  return true;
}

bool memory_card_image_delete_file(u8* data, const memory_card_image_file_info_t* fi, bool clear_sectors)
{
  INFO_LOG("Deleting '%s' from memory card (%u blocks)", fi->filename, fi->num_blocks);

  u32 block_number = fi->first_block;
  for (u32 i = 0; i < fi->num_blocks && (block_number > 0u && block_number < NUM_BLOCKS); i++)
  {
    directory_frame_t* df = dir_ptr_mut(data, block_number);
    block_number = (u32)df->next_block_number + 1u;

    if (clear_sectors)
    {
      memset(df, 0, sizeof(*df));
      df->block_allocation_state = 0xA0u;
    }
    else
    {
      /* Tombstone the chain (0xA1 first / 0xA2 middle / 0xA3 last) so it
       * remains undeletable until a write reuses the block. */
      if (i == 0u)
        df->block_allocation_state = 0xA1u;
      else if (i == (fi->num_blocks - 1u))
        df->block_allocation_state = 0xA3u;
      else
        df->block_allocation_state = 0xA2u;
    }

    df->next_block_number = 0xFFFFu;
    update_directory_checksum(df);
  }

  return true;
}

bool memory_card_image_undelete_file(u8* data, const memory_card_image_file_info_t* fi)
{
  if (!fi->deleted)
  {
    ERROR_LOG("File '%s' is not deleted", fi->filename);
    return false;
  }

  INFO_LOG("Undeleting '%s' from memory card (%u blocks)", fi->filename, fi->num_blocks);

  /* Pre-flight: walk the chain once to confirm tombstone bytes are intact;
   * a real card may have had its blocks overwritten, in which case we must
   * fail before mutating anything. */
  u32 block_number = fi->first_block;
  for (u32 i = 0; i < fi->num_blocks && (block_number > 0u && block_number < NUM_BLOCKS); i++)
  {
    const u32 this_block = block_number;
    const directory_frame_t* df = dir_ptr(data, block_number);
    block_number = (u32)df->next_block_number + 1u;

    if (i == 0u)
    {
      if (df->block_allocation_state != 0xA1u)
      {
        ERROR_LOG("Incorrect block state for %u, expected 0xA1 got 0x%02X",
                  this_block, df->block_allocation_state);
        return false;
      }
    }
    else if (i == (fi->num_blocks - 1u))
    {
      if (df->block_allocation_state != 0xA3u)
      {
        ERROR_LOG("Incorrect block state for %u, expected 0xA3 got 0x%02X",
                  this_block, df->block_allocation_state);
        return false;
      }
    }
    else
    {
      if (df->block_allocation_state != 0xA2u)
      {
        ERROR_LOG("Incorrect block state for %u, expected 0xA2 got 0x%02X",
                  this_block, df->block_allocation_state);
        return false;
      }
    }
  }

  block_number = fi->first_block;
  for (u32 i = 0; i < fi->num_blocks && (block_number > 0u && block_number < NUM_BLOCKS); i++)
  {
    directory_frame_t* df = dir_ptr_mut(data, block_number);
    block_number = (u32)df->next_block_number + 1u;

    if (i == 0u)
      df->block_allocation_state = 0x51u;
    else if (i == (fi->num_blocks - 1u))
      df->block_allocation_state = 0x53u;
    else
      df->block_allocation_state = 0x52u;

    update_directory_checksum(df);
  }

  return true;
}

bool memory_card_image_rename_file(u8* data, const memory_card_image_file_info_t* fi,
                                   const char* new_filename_data, u32 new_filename_len, Error* error)
{
  if (new_filename_len > FILE_TOTAL_LEN)
  {
    Error_set_string_fmt(error, "File name must be at maximum %u characters long.", (unsigned)FILE_TOTAL_LEN);
    return false;
  }

  /* Reject if any non-deleted file already has this name. */
  memory_card_image_file_info_t* others = NULL;
  size_t other_count = 0;
  if (memory_card_image_enumerate_files(data, false, &others, &other_count))
  {
    for (size_t i = 0; i < other_count; i++)
    {
      const memory_card_image_file_info_t* of = &others[i];
      const size_t of_len = strlen(of->filename);
      if (of_len == new_filename_len && memcmp(of->filename, new_filename_data, new_filename_len) == 0)
      {
        Error_set_string_fmt(error,
                             "Save file with the same name '%.*s' already exists in memory card.",
                             (int)new_filename_len, new_filename_data);
        memory_card_image_free_file_info_array(others, other_count);
        return false;
      }
    }
    memory_card_image_free_file_info_array(others, other_count);
  }

  directory_frame_t* df = dir_ptr_mut(data, fi->first_block);
  for (size_t i = 0; i < new_filename_len; i++)
    df->filename[i] = new_filename_data[i];
  for (size_t i = new_filename_len; i < FILE_TOTAL_LEN; i++)
    df->filename[i] = '\0';
  df->filename[FILE_TOTAL_LEN] = '\0';
  update_directory_checksum(df);
  return true;
}

static bool import_card_mcd(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error)
{
  (void)filename;
  if (file_data_len != DATA_SIZE)
  {
    Error_set_string_fmt(error,
                         "File is incorrect size, expected %u bytes, got %zu bytes.",
                         (unsigned)DATA_SIZE, file_data_len);
    return false;
  }

  memcpy(data, file_data, DATA_SIZE);
  return true;
}

static bool import_card_gme(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error)
{
#pragma pack(push, 1)
  struct gme_header {
    char id[12];
    u8   unk1[4];
    u8   unk2[5];
    u8   sector0[16];
    u8   sector1[16];
    u8   unk3[11];
    char descriptions[256][15];
  };
#pragma pack(pop)
  _Static_assert(sizeof(struct gme_header) == 0xF40, "GME header layout");

  /* Some "GME" files in the wild are actually raw .mcd dumps. */
  if (file_data_len == DATA_SIZE)
    return import_card_mcd(data, filename, file_data, file_data_len, error);

  const size_t MIN_SIZE = sizeof(struct gme_header) + BLOCK_SIZE;
  if (file_data_len < MIN_SIZE)
  {
    Error_set_string_fmt(error,
                         "File is incorrect size, expected at least %zu bytes, got %zu bytes.",
                         MIN_SIZE, file_data_len);
    return false;
  }

  const size_t expected_size = sizeof(struct gme_header) + DATA_SIZE;
  if (file_data_len < expected_size)
  {
    WARNING_LOG("GME memory card '%s' is too small (got %zu expected %zu), padding with zeroes",
                filename, file_data_len, expected_size);
    if (file_data_len > sizeof(struct gme_header))
    {
      const size_t present = file_data_len - sizeof(struct gme_header);
      memcpy(data, file_data + sizeof(struct gme_header), present);
      memset(data + present, 0, DATA_SIZE - present);
    }
    else
    {
      memset(data, 0, DATA_SIZE);
    }
  }
  else
  {
    /* The header is metadata we don't reuse; skip past it. */
    memcpy(data, file_data + sizeof(struct gme_header), DATA_SIZE);
  }

  return true;
}

static bool import_card_vgs(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error)
{
  (void)filename;
  enum { HEADER_SIZE = 64u, EXPECTED_SIZE = HEADER_SIZE + (u32)DATA_SIZE };

  if (file_data_len != (size_t)EXPECTED_SIZE)
  {
    Error_set_string_fmt(error,
                         "File is incorrect size, expected %u bytes, got %zu bytes.",
                         (unsigned)EXPECTED_SIZE, file_data_len);
    return false;
  }

  /* Connectix Virtual Game Station signature: "VgsM". */
  if (file_data[0] != 'V' || file_data[1] != 'g' || file_data[2] != 's' || file_data[3] != 'M')
  {
    Error_set_string(error, "Incorrect header.");
    return false;
  }

  memcpy(data, &file_data[HEADER_SIZE], DATA_SIZE);
  return true;
}

static bool import_card_psx(u8* data, const char* filename,
                            const u8* file_data, size_t file_data_len, Error* error)
{
  (void)filename;
  enum { HEADER_SIZE = 256u, EXPECTED_SIZE = HEADER_SIZE + (u32)DATA_SIZE };

  if (file_data_len != (size_t)EXPECTED_SIZE)
  {
    Error_set_string_fmt(error,
                         "File is incorrect size, expected %u bytes, got %zu bytes.",
                         (unsigned)EXPECTED_SIZE, file_data_len);
    return false;
  }

  if (file_data[0] != 'P' || file_data[1] != 'S' || file_data[2] != 'V')
  {
    Error_set_string(error, "Incorrect header.");
    return false;
  }

  memcpy(data, &file_data[HEADER_SIZE], DATA_SIZE);
  return true;
}

bool memory_card_image_import_card_bytes(u8* data, const char* filename,
                                         const u8* file_data, size_t file_data_len, Error* error)
{
  const char* ext_d;
  u32 ext_l;
  path_get_extension_cstr(filename, &ext_d, &ext_l);
  if (ext_l == 0)
  {
    Error_set_string(error, "File must have an extension.");
    return false;
  }

  /* Switch on extension; everything compares case-insensitively. */
  if (string_util_equal_no_case_view(ext_d, ext_l, "mcd", 3) ||
      string_util_equal_no_case_view(ext_d, ext_l, "mcr", 3) ||
      string_util_equal_no_case_view(ext_d, ext_l, "mc",  2) ||
      string_util_equal_no_case_view(ext_d, ext_l, "srm", 3) ||
      string_util_equal_no_case_view(ext_d, ext_l, "psm", 3) ||
      string_util_equal_no_case_view(ext_d, ext_l, "ps",  2) ||
      string_util_equal_no_case_view(ext_d, ext_l, "ddf", 3))
    return import_card_mcd(data, filename, file_data, file_data_len, error);

  if (string_util_equal_no_case_view(ext_d, ext_l, "gme", 3))
    return import_card_gme(data, filename, file_data, file_data_len, error);

  if (string_util_equal_no_case_view(ext_d, ext_l, "mem", 3) ||
      string_util_equal_no_case_view(ext_d, ext_l, "vgs", 3))
    return import_card_vgs(data, filename, file_data, file_data_len, error);

  if (string_util_equal_no_case_view(ext_d, ext_l, "psx", 3))
    return import_card_psx(data, filename, file_data, file_data_len, error);

  Error_set_string_fmt(error, "Unknown extension '%.*s'.", (int)ext_l, ext_d);
  return false;
}

bool memory_card_image_import_card_path(u8* data, const char* filename, Error* error)
{
  u8* file_data = NULL;
  size_t file_data_len = 0;
  if (!fs_read_binary_file_path(filename, &file_data, &file_data_len, error))
    return false;

  const bool ok = memory_card_image_import_card_bytes(data, filename, file_data, file_data_len, error);
  free(file_data);
  return ok;
}

bool memory_card_image_export_save(u8* data, const memory_card_image_file_info_t* fi,
                                   const char* filename, Error* error)
{
  /* Read out the file payload. */
  u8* file_payload = NULL;
  size_t file_payload_len = 0;
  if (!memory_card_image_read_file(data, fi, &file_payload, &file_payload_len, error))
    return false;

  /* .mcs format: directory frame + raw block data, in that order. */
  FILE* fp = fs_open_file(filename, "wb", error);
  if (!fp)
  {
    free(file_payload);
    return false;
  }

  const directory_frame_t* df_ptr = dir_ptr(data, fi->first_block);
  if (fwrite(df_ptr, sizeof(directory_frame_t), 1, fp) != 1 ||
      fwrite(file_payload, file_payload_len, 1, fp) != 1)
  {
    Error_set_errno_prefix(error, "fwrite() failed: ", errno);
    fclose(fp);
    free(file_payload);
    return false;
  }

  fclose(fp);
  free(file_payload);
  return true;
}

static bool import_save_with_directory_frame(u8* data, const char* filename, s64 file_size, Error* error)
{
  /* .mcs: 128-byte directory frame + N*8KB block data, 1..15 blocks. */
  if (file_size <= (s64)FRAME_SIZE ||
      ((u64)file_size - FRAME_SIZE) % BLOCK_SIZE != 0u ||
      ((u64)file_size - FRAME_SIZE) / BLOCK_SIZE > 15u) 
  {
    Error_set_string(error, "Invalid size for save file.");
    return false;
  }

  FILE* fp = fs_open_file(filename, "rb", error);
  if (!fp)
    return false;

  directory_frame_t df;
  if (fread(&df, sizeof(df), 1, fp) != 1)
  {
    Error_set_errno_prefix(error, "Failed to read directory frame: ", errno);
    fclose(fp);
    return false;
  }

  if (df.file_size < BLOCK_SIZE || df.file_size % BLOCK_SIZE != 0u || df.file_size / BLOCK_SIZE > 15u)
  {
    Error_set_string_fmt(error, "Invalid size (%u bytes) reported by directory frame.", df.file_size);
    fclose(fp);
    return false;
  }

  u8* blocks = (u8*)malloc(df.file_size);
  if (!blocks)
  {
    Error_set_string(error, "Out of memory");
    fclose(fp);
    return false;
  }
  if (fread(blocks, df.file_size, 1, fp) != 1)
  {
    Error_set_errno_prefix(error, "Failed to read block bytes: ", errno);
    free(blocks);
    fclose(fp);
    return false;
  }
  fclose(fp);

  const u32 num_blocks = (df.file_size + (BLOCK_SIZE - 1u)) / BLOCK_SIZE;
  if (memory_card_image_get_free_block_count(data) < num_blocks)
  {
    Error_set_string(error, "Insufficient free blocks.");
    free(blocks);
    return false;
  }

  /* Reject duplicates against active saves; reuse deleted entries. */
  memory_card_image_file_info_t* fileinfos = NULL;
  size_t fi_count = 0;
  if (memory_card_image_enumerate_files(data, true, &fileinfos, &fi_count))
  {
    for (size_t i = 0; i < fi_count; i++)
    {
      memory_card_image_file_info_t* fi = &fileinfos[i];
      if (strncmp(fi->filename, df.filename, sizeof(df.filename)) == 0)
      {
        if (!fi->deleted)
        {
          Error_set_string_fmt(error,
                               "Save file with the same name '%s' already exists in memory card.",
                               fi->filename);
          memory_card_image_free_file_info_array(fileinfos, fi_count);
          free(blocks);
          return false;
        }
        memory_card_image_delete_file(data, fi, true);
      }
    }
    memory_card_image_free_file_info_array(fileinfos, fi_count);
  }

  /* Use the directory-frame filename verbatim; treat it as NUL-padded. */
  u32 fname_len = 0;
  while (fname_len < sizeof(df.filename) && df.filename[fname_len] != '\0')
    fname_len++;

  const bool ok = memory_card_image_write_file(data, df.filename, fname_len, blocks, df.file_size, error);
  free(blocks);
  return ok;
}

static bool import_raw_save(u8* data, const char* filename, s64 file_size, Error* error)
{
  (void)file_size;
  /* "Raw save" means a plain block-aligned dump named after the save id. */
  const char* fn_d;
  u32 fn_l;
  path_get_file_name_cstr(filename, &fn_d, &fn_l);

  /* Strip extension. */
  const char* base_d;
  u32 base_l;
  path_strip_extension_view(fn_d, fn_l, &base_d, &base_l);
  if (base_l == 0)
  {
    Error_set_string(error, "Invalid filename.");
    return false;
  }

  if (base_l > DIRECTORY_FRAME_FILE_NAME_LENGTH)
    base_l = DIRECTORY_FRAME_FILE_NAME_LENGTH;

  u8* blocks = NULL;
  size_t blocks_len = 0;
  if (!fs_read_binary_file_path(filename, &blocks, &blocks_len, error))
    return false;

  const u32 free_block_count = memory_card_image_get_free_block_count(data);
  const u32 num_blocks = (u32)((blocks_len + (BLOCK_SIZE - 1u)) / BLOCK_SIZE);
  if (free_block_count < num_blocks)
  {
    Error_set_string_fmt(error, "Insufficient free blocks, needs %u blocks, but only have %u.",
                         num_blocks, free_block_count);
    free(blocks);
    return false;
  }

  /* Reject duplicates; reuse deleted entries. */
  memory_card_image_file_info_t* fileinfos = NULL;
  size_t fi_count = 0;
  if (memory_card_image_enumerate_files(data, true, &fileinfos, &fi_count))
  {
    for (size_t i = 0; i < fi_count; i++)
    {
      memory_card_image_file_info_t* fi = &fileinfos[i];
      const u32 fi_len = (u32)strlen(fi->filename);
      if (fi_len == base_l && memcmp(fi->filename, base_d, base_l) == 0)
      {
        if (!fi->deleted)
        {
          Error_set_string_fmt(error,
                               "Save file with the same name '%s' already exists in memory card.",
                               fi->filename);
          memory_card_image_free_file_info_array(fileinfos, fi_count);
          free(blocks);
          return false;
        }
        memory_card_image_delete_file(data, fi, true);
      }
    }
    memory_card_image_free_file_info_array(fileinfos, fi_count);
  }

  const bool ok = memory_card_image_write_file(data, base_d, base_l, blocks, blocks_len, error);
  free(blocks);
  return ok;
}

bool memory_card_image_import_save(u8* data, const char* filename, Error* error)
{
  fs_stat_data_t sd;
  if (!fs_stat_path_data(filename, &sd, NULL) || sd.size == 0)
  {
    Error_set_string(error, "File does not exist, or is empty.");
    return false;
  }

  const char* ext_d;
  u32 ext_l;
  path_get_extension_cstr(filename, &ext_d, &ext_l);

  if (string_util_equal_no_case_view(ext_d, ext_l, "mcs", 3))
    return import_save_with_directory_frame(data, filename, sd.size, error);

  if (sd.size > 0 && sd.size < (s64)DATA_SIZE && (sd.size % BLOCK_SIZE) == 0)
    return import_raw_save(data, filename, sd.size, error);

  Error_set_string(error, "Unknown save format.");
  return false;
}
