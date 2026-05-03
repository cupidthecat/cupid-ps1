/*
 * SBI/LSD subchannel-Q overlay.  std::unordered_map<u32, SubChannelQ> becomes
 * a flat sorted array of (lba, subq) entries with binary search lookup -
 * SBI files rarely contain more than a few hundred entries (one per protected
 * sector on a libcrypt disc), so the linear-array-with-bsearch approach beats
 * the cache-miss cost of a hashtable for our access pattern (one hit per
 * SubQ generation).  Each replacement entry is 16 bytes, so a 600-entry SBI
 * fits in ~10KB.
 */

#ifndef CUPID_CORE_CDROM_SUBQ_REPLACEMENT_H
#define CUPID_CORE_CDROM_SUBQ_REPLACEMENT_H

#include "common/types.h"
#include "util/cd_image.h"

typedef struct Error Error;

typedef struct {
  u32             lba;
  cd_image_subq_t subq;
} cdrom_subq_replacement_entry_t;

typedef struct {
  cdrom_subq_replacement_entry_t* entries;
  size_t                          count;
  size_t                          capacity;
} cdrom_subq_replacement_t;

/* Lifecycle. */
void cdrom_subq_replacement_init(cdrom_subq_replacement_t* r);
void cdrom_subq_replacement_destroy(cdrom_subq_replacement_t* r);

bool cdrom_subq_replacement_load_for_image(cdrom_subq_replacement_t* out_replacement,
                                           bool* out_has_data,
                                           cd_image_t* image,
                                           const char* serial, size_t serial_len,
                                           const char* title, size_t title_len,
                                           const char* save_title, size_t save_title_len,
                                           Error* error);

/* Returns the count of replacement entries. */
size_t cdrom_subq_replacement_get_replacement_sector_count(const cdrom_subq_replacement_t* r);

/* Returns a pointer to the replacement subq for the given lba, or NULL if
 * none exists.  The returned pointer is valid until destroy. */
const cd_image_subq_t* cdrom_subq_replacement_get_replacement_subq(const cdrom_subq_replacement_t* r, u32 lba);

#endif /* CUPID_CORE_CDROM_SUBQ_REPLACEMENT_H */
