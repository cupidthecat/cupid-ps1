/*
 *   class BumpStringPool       -> struct bump_string_pool_t
 *   class BumpUniqueStringPool -> struct bump_unique_string_pool_t
 *   class StringPool           -> struct string_pool_t
 *
 * AddString(view)              -> *_add_string(p, data, len) -> size_t offset
 * GetString(offset)            -> *_get_string(p, offset, &out_len) -> const char* (NUL-terminated)
 * GetString(offset, length)    -> *_get_string_n(p, offset, length) -> const char*
 * Clear/GetSize/IsEmpty/Reserve/GetCount carry through with snake_case names.
 *
 * Empty input strings return INVALID_OFFSET.  Out-of-bounds lookups return
 * NULL with *out_len = 0.
 */

#ifndef CUPID_COMMON_STRING_POOL_H
#define CUPID_COMMON_STRING_POOL_H

#include "common/types.h"

#include <stdbool.h>
#include <stddef.h>

#define STRING_POOL_INVALID_OFFSET ((size_t)-1)

typedef struct bump_string_pool {
  char*  buf;
  size_t size;
  size_t cap;
} bump_string_pool_t;

void        bump_string_pool_init    (bump_string_pool_t* p);
void        bump_string_pool_destroy (bump_string_pool_t* p);
size_t      bump_string_pool_add     (bump_string_pool_t* p, const char* data, size_t len);
const char* bump_string_pool_get     (const bump_string_pool_t* p, size_t offset, size_t* out_len);
const char* bump_string_pool_get_n   (const bump_string_pool_t* p, size_t offset, size_t length);
void        bump_string_pool_clear   (bump_string_pool_t* p);
size_t      bump_string_pool_size    (const bump_string_pool_t* p);
bool        bump_string_pool_empty   (const bump_string_pool_t* p);
void        bump_string_pool_reserve (bump_string_pool_t* p, size_t bytes);

typedef struct bump_unique_string_ref {
  size_t offset;
  size_t length;
} bump_unique_string_ref_t;

typedef struct bump_unique_string_pool {
  char*                     buf;
  size_t                    size;
  size_t                    cap;
  bump_unique_string_ref_t* refs;
  size_t                    refs_count;
  size_t                    refs_cap;
} bump_unique_string_pool_t;

void        bump_unique_string_pool_init    (bump_unique_string_pool_t* p);
void        bump_unique_string_pool_destroy (bump_unique_string_pool_t* p);
size_t      bump_unique_string_pool_add     (bump_unique_string_pool_t* p, const char* data, size_t len);
const char* bump_unique_string_pool_get     (const bump_unique_string_pool_t* p, size_t offset, size_t* out_len);
const char* bump_unique_string_pool_get_n   (const bump_unique_string_pool_t* p, size_t offset, size_t length);
void        bump_unique_string_pool_clear   (bump_unique_string_pool_t* p);
size_t      bump_unique_string_pool_size    (const bump_unique_string_pool_t* p);
bool        bump_unique_string_pool_empty   (const bump_unique_string_pool_t* p);
size_t      bump_unique_string_pool_count   (const bump_unique_string_pool_t* p);
void        bump_unique_string_pool_reserve (bump_unique_string_pool_t* p, size_t num_strings, size_t storage_size);

typedef struct string_pool_bucket {
  size_t offset;   /* offset into buf */
  size_t length;   /* string length (no null) */
  u64    hash;     /* FNV-1a 64-bit cached */
  bool   used;
} string_pool_bucket_t;

typedef struct string_pool {
  char*                 buf;
  size_t                size;
  size_t                cap;
  string_pool_bucket_t* table;
  size_t                table_cap;   /* power of 2 */
  size_t                table_count;
} string_pool_t;

void        string_pool_init    (string_pool_t* p);
void        string_pool_destroy (string_pool_t* p);
size_t      string_pool_add     (string_pool_t* p, const char* data, size_t len);
const char* string_pool_get     (const string_pool_t* p, size_t offset, size_t* out_len);
const char* string_pool_get_n   (const string_pool_t* p, size_t offset, size_t length);
void        string_pool_clear   (string_pool_t* p);
size_t      string_pool_size    (const string_pool_t* p);
bool        string_pool_empty   (const string_pool_t* p);
size_t      string_pool_count   (const string_pool_t* p);
void        string_pool_reserve (string_pool_t* p, size_t bytes);

#endif /* CUPID_COMMON_STRING_POOL_H */
