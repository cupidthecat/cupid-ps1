/*
 * namespace BIOS -> bios_ prefix.  std::optional<Image> becomes a bool
 * return + out-pointer.  std::span<const u8> becomes (const u8*, size_t).
 * DynamicHeapArray<u8> in the original Image becomes a heap (data, len) pair
 * the caller frees with free().
 */

#ifndef CUPID_CORE_BIOS_H
#define CUPID_CORE_BIOS_H

#include "core/types.h"

#include "common/small_string.h"

typedef struct Error Error;

enum {
  BIOS_BASE      = 0x1FC00000u,
  BIOS_SIZE      = 0x80000u,
  BIOS_SIZE_PS2  = 0x400000u,
  BIOS_SIZE_PS3  = 0x3E66F0u,
  BIOS_HASH_SIZE = 16,
};

typedef enum {
  BIOS_FAST_BOOT_PATCH_UNSUPPORTED,
  BIOS_FAST_BOOT_PATCH_TYPE1,
  BIOS_FAST_BOOT_PATCH_TYPE2,
} bios_fast_boot_patch_t;

typedef u8 bios_hash_t[BIOS_HASH_SIZE];

typedef struct bios_image_info {
  const char*            description;
  console_region_t       region;
  bool                   region_check;
  bios_fast_boot_patch_t fastboot_patch;
  u8                     priority;
  bios_hash_t            hash;
} bios_image_info_t;

typedef struct {
  const bios_image_info_t* info; /* nullable: unknown image */
  bios_hash_t              hash;
  u8*                      data;       /* heap, BIOS_SIZE bytes */
  size_t                   data_size;  /* = BIOS_SIZE on success */
} bios_image_t;

#pragma pack(push, 1)
typedef struct bios_psexe_header {
  char id[8];            /* 0x000-0x007 PS-X EXE */
  char pad1[8];          /* 0x008-0x00F */
  u32 initial_pc;        /* 0x010 */
  u32 initial_gp;        /* 0x014 */
  u32 load_address;      /* 0x018 */
  u32 file_size;         /* 0x01C excluding 0x800-byte header */
  u32 unk0;              /* 0x020 */
  u32 unk1;              /* 0x024 */
  u32 memfill_start;     /* 0x028 */
  u32 memfill_size;      /* 0x02C */
  u32 initial_sp_base;   /* 0x030 */
  u32 initial_sp_offset; /* 0x034 */
  u32 reserved[5];       /* 0x038-0x04B */
  char marker[0x7B4];    /* 0x04C-0x7FF */
} bios_psexe_header_t;
_Static_assert(sizeof(bios_psexe_header_t) == 0x800, "PS-X EXE header must be 2KB");
#pragma pack(pop)

/* .cpe files */
#define BIOS_CPE_MAGIC 0x01455043u

bool bios_supports_fast_boot      (const bios_image_info_t* info);
bool bios_can_slow_boot_disc      (const bios_image_info_t* info, disc_region_t disc_region);
void bios_image_info_get_hash_string(tiny_string_t* out, const bios_hash_t hash);

/* Loads a BIOS file from disk.  On success, *out is populated and the caller
 * must free out->data with free() (use bios_image_destroy()).  Returns false
 * on failure (out left untouched). */
bool bios_load_image_from_file(const char* filename, bios_image_t* out, Error* error);

void bios_image_destroy(bios_image_t* img);

const bios_image_info_t* bios_get_info_for_hash(const u8* image, size_t image_len, const bios_hash_t hash);

bool bios_is_valid_for_region(console_region_t console_region, console_region_t bios_region);

bool bios_patch_fast_boot(u8* image, u32 image_size, bios_fast_boot_patch_t type);

bool          bios_is_valid_psexe_header(const bios_psexe_header_t* header, size_t file_size);
disc_region_t bios_get_psexe_disc_region(const bios_psexe_header_t* header);

/* Loads the BIOS image for the specified region. */
bool bios_get_image(console_region_t region, bios_image_t* out, Error* error);

/* Search the directory; if no match, the first 512KB-or-greater BIOS-sized
 * file wins.  out untouched on failure. */
bool bios_find_image_in_directory(console_region_t region, const char* directory,
                                  bios_image_t* out, Error* error);

/* Listing helper: returns malloc'd parallel arrays
 * (out_filenames[], out_infos[]) of length *out_count.  Each filename is
 * heap-owned; free with bios_free_image_listing(). */
typedef struct {
  char*                    filename; /* heap, owned */
  const bios_image_info_t* info;     /* may be NULL for unknown */
} bios_directory_entry_t;

bool bios_find_images_in_directory(const char* directory,
                                   bios_directory_entry_t** out_entries, size_t* out_count);
void bios_free_image_listing(bios_directory_entry_t* entries, size_t count);

/* Returns true if any BIOS images are found in the configured BIOS directory. */
bool bios_has_any_images(void);

#endif /* CUPID_CORE_BIOS_H */
