#include "common/string_pool.h"

#include <stdlib.h>
#include <string.h>

static void* xrealloc(void* p, size_t bytes)
{
  void* r = realloc(p, bytes);
  if (!r && bytes > 0) {
    abort();
  }
  return r;
}

static void buf_reserve(char** buf, size_t* cap, size_t size, size_t add)
{
  if (size + add <= *cap) return;
  size_t new_cap = *cap ? *cap * 2 : 64;
  while (new_cap < size + add) new_cap *= 2;
  *buf = (char*)xrealloc(*buf, new_cap);
  *cap = new_cap;
}

/* FNV-1a 64-bit. */
static u64 hash_fnv1a(const char* data, size_t len)
{
  u64 h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; i++) {
    h ^= (u64)(u8)data[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

void bump_string_pool_init(bump_string_pool_t* p)
{
  p->buf = NULL;
  p->size = 0;
  p->cap = 0;
}

void bump_string_pool_destroy(bump_string_pool_t* p)
{
  free(p->buf);
  p->buf = NULL;
  p->size = 0;
  p->cap = 0;
}

size_t bump_string_pool_add(bump_string_pool_t* p, const char* data, size_t len)
{
  if (len == 0) return STRING_POOL_INVALID_OFFSET;

  const size_t offset = p->size;
  buf_reserve(&p->buf, &p->cap, p->size, len + 1);
  memcpy(p->buf + p->size, data, len);
  p->buf[p->size + len] = '\0';
  p->size += len + 1;
  return offset;
}

const char* bump_string_pool_get(const bump_string_pool_t* p, size_t offset, size_t* out_len)
{
  if (offset == STRING_POOL_INVALID_OFFSET || offset >= p->size) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  const char* s = p->buf + offset;
  if (out_len) *out_len = strlen(s);
  return s;
}

const char* bump_string_pool_get_n(const bump_string_pool_t* p, size_t offset, size_t length)
{
  /* Only the (offset+length) > size check; no InvalidOffset short-circuit. */
  if (offset > p->size || length > p->size - offset) {
    return NULL;
  }
  return p->buf + offset;
}

void bump_string_pool_clear(bump_string_pool_t* p) { p->size = 0; }

size_t bump_string_pool_size(const bump_string_pool_t* p) { return p->size; }

bool   bump_string_pool_empty(const bump_string_pool_t* p) { return p->size == 0; }

void bump_string_pool_reserve(bump_string_pool_t* p, size_t bytes)
{
  if (bytes <= p->cap) return;
  p->buf = (char*)xrealloc(p->buf, bytes);
  p->cap = bytes;
}

void bump_unique_string_pool_init(bump_unique_string_pool_t* p)
{
  p->buf = NULL;
  p->size = 0;
  p->cap = 0;
  p->refs = NULL;
  p->refs_count = 0;
  p->refs_cap = 0;
}

void bump_unique_string_pool_destroy(bump_unique_string_pool_t* p)
{
  free(p->buf);
  free(p->refs);
  p->buf = NULL;
  p->refs = NULL;
  p->size = p->cap = p->refs_count = p->refs_cap = 0;
}

/* Returns insertion index; sets *found_offset if the search key already exists. */
static size_t bump_unique_lower_bound(const bump_unique_string_pool_t* p, const char* data, size_t len, bool* found, size_t* found_offset)
{
  size_t lo = 0;
  size_t hi = p->refs_count;
  *found = false;
  *found_offset = 0;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const bump_unique_string_ref_t* r = &p->refs[mid];
    size_t cmp_len = r->length < len ? r->length : len;
    int c = memcmp(p->buf + r->offset, data, cmp_len);
    if (c == 0) c = (r->length < len) ? -1 : (r->length > len ? 1 : 0);
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  if (lo < p->refs_count) {
    const bump_unique_string_ref_t* r = &p->refs[lo];
    if (r->length == len && memcmp(p->buf + r->offset, data, len) == 0) {
      *found = true;
      *found_offset = r->offset;
    }
  }
  return lo;
}

static void bump_unique_refs_reserve(bump_unique_string_pool_t* p, size_t add)
{
  if (p->refs_count + add <= p->refs_cap) return;
  size_t new_cap = p->refs_cap ? p->refs_cap * 2 : 16;
  while (new_cap < p->refs_count + add) new_cap *= 2;
  p->refs = (bump_unique_string_ref_t*)xrealloc(p->refs, new_cap * sizeof(*p->refs));
  p->refs_cap = new_cap;
}

size_t bump_unique_string_pool_add(bump_unique_string_pool_t* p, const char* data, size_t len)
{
  if (len == 0) return STRING_POOL_INVALID_OFFSET;

  bool found;
  size_t found_offset;
  size_t insert_at = bump_unique_lower_bound(p, data, len, &found, &found_offset);
  if (found) return found_offset;

  const size_t offset = p->size;
  buf_reserve(&p->buf, &p->cap, p->size, len + 1);
  memcpy(p->buf + p->size, data, len);
  p->buf[p->size + len] = '\0';
  p->size += len + 1;

  bump_unique_refs_reserve(p, 1);
  if (insert_at < p->refs_count) {
    memmove(&p->refs[insert_at + 1], &p->refs[insert_at],
            (p->refs_count - insert_at) * sizeof(bump_unique_string_ref_t));
  }
  p->refs[insert_at].offset = offset;
  p->refs[insert_at].length = len;
  p->refs_count++;

  return offset;
}

const char* bump_unique_string_pool_get(const bump_unique_string_pool_t* p, size_t offset, size_t* out_len)
{
  if (offset >= p->size) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  const char* s = p->buf + offset;
  if (out_len) *out_len = strlen(s);
  return s;
}

const char* bump_unique_string_pool_get_n(const bump_unique_string_pool_t* p, size_t offset, size_t length)
{
  if (offset > p->size || length > p->size - offset) {
    return NULL;
  }
  return p->buf + offset;
}

void bump_unique_string_pool_clear(bump_unique_string_pool_t* p)
{
  p->size = 0;
  p->refs_count = 0;
}

size_t bump_unique_string_pool_size(const bump_unique_string_pool_t* p)  { return p->size; }
bool   bump_unique_string_pool_empty(const bump_unique_string_pool_t* p) { return p->size == 0; }
size_t bump_unique_string_pool_count(const bump_unique_string_pool_t* p) { return p->refs_count; }

void bump_unique_string_pool_reserve(bump_unique_string_pool_t* p, size_t num_strings, size_t storage_size)
{
  if (num_strings > p->refs_cap) {
    p->refs = (bump_unique_string_ref_t*)xrealloc(p->refs, num_strings * sizeof(*p->refs));
    p->refs_cap = num_strings;
  }
  if (storage_size > p->cap) {
    p->buf = (char*)xrealloc(p->buf, storage_size);
    p->cap = storage_size;
  }
}

#define SP_INITIAL_CAP    64
#define SP_LOAD_NUM       7
#define SP_LOAD_DEN       10

void string_pool_init(string_pool_t* p)
{
  p->buf = NULL;
  p->size = 0;
  p->cap = 0;
  p->table = NULL;
  p->table_cap = 0;
  p->table_count = 0;
}

void string_pool_destroy(string_pool_t* p)
{
  free(p->buf);
  free(p->table);
  p->buf = NULL;
  p->table = NULL;
  p->size = p->cap = p->table_cap = p->table_count = 0;
}

static void sp_table_insert_no_grow(string_pool_bucket_t* table, size_t cap_mask, const char* buf, u64 hash, size_t offset, size_t length)
{
  size_t idx = (size_t)(hash & cap_mask);
  while (table[idx].used) {
    /* Should never collide on (hash, length, bytes) since caller already verified absence. */
    idx = (idx + 1) & cap_mask;
  }
  table[idx].used = true;
  table[idx].hash = hash;
  table[idx].offset = offset;
  table[idx].length = length;
  (void)buf;
}

static void sp_grow(string_pool_t* p)
{
  size_t new_cap = p->table_cap ? p->table_cap * 2 : SP_INITIAL_CAP;
  string_pool_bucket_t* new_table = (string_pool_bucket_t*)calloc(new_cap, sizeof(*new_table));
  if (!new_table) abort();
  size_t mask = new_cap - 1;
  for (size_t i = 0; i < p->table_cap; i++) {
    if (p->table[i].used) {
      sp_table_insert_no_grow(new_table, mask, p->buf, p->table[i].hash, p->table[i].offset, p->table[i].length);
    }
  }
  free(p->table);
  p->table = new_table;
  p->table_cap = new_cap;
}

/* Returns existing offset if str found; else SP_NOT_FOUND. */
#define SP_NOT_FOUND ((size_t)-2)
static size_t sp_lookup(const string_pool_t* p, const char* data, size_t len, u64 hash)
{
  if (p->table_cap == 0) return SP_NOT_FOUND;
  size_t mask = p->table_cap - 1;
  size_t idx = (size_t)(hash & mask);
  while (p->table[idx].used) {
    if (p->table[idx].hash == hash && p->table[idx].length == len &&
        memcmp(p->buf + p->table[idx].offset, data, len) == 0) {
      return p->table[idx].offset;
    }
    idx = (idx + 1) & mask;
  }
  return SP_NOT_FOUND;
}

size_t string_pool_add(string_pool_t* p, const char* data, size_t len)
{
  if (len == 0) return STRING_POOL_INVALID_OFFSET;

  u64 hash = hash_fnv1a(data, len);
  size_t existing = sp_lookup(p, data, len, hash);
  if (existing != SP_NOT_FOUND) return existing;

  /* Grow before insert if past load factor. */
  if ((p->table_count + 1) * SP_LOAD_DEN >= p->table_cap * SP_LOAD_NUM) {
    sp_grow(p);
  }

  const size_t offset = p->size;
  buf_reserve(&p->buf, &p->cap, p->size, len + 1);
  memcpy(p->buf + p->size, data, len);
  p->buf[p->size + len] = '\0';
  p->size += len + 1;

  size_t mask = p->table_cap - 1;
  sp_table_insert_no_grow(p->table, mask, p->buf, hash, offset, len);
  p->table_count++;

  return offset;
}

const char* string_pool_get(const string_pool_t* p, size_t offset, size_t* out_len)
{
  if (offset >= p->size) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  const char* s = p->buf + offset;
  if (out_len) *out_len = strlen(s);
  return s;
}

const char* string_pool_get_n(const string_pool_t* p, size_t offset, size_t length)
{
  if (offset > p->size || length > p->size - offset) {
    return NULL;
  }
  return p->buf + offset;
}

void string_pool_clear(string_pool_t* p)
{
  p->size = 0;
  if (p->table) {
    memset(p->table, 0, p->table_cap * sizeof(*p->table));
  }
  p->table_count = 0;
}

size_t string_pool_size(const string_pool_t* p)  { return p->size; }
bool   string_pool_empty(const string_pool_t* p) { return p->size == 0; }
size_t string_pool_count(const string_pool_t* p) { return p->table_count; }

void string_pool_reserve(string_pool_t* p, size_t bytes)
{
  if (bytes <= p->cap) return;
  p->buf = (char*)xrealloc(p->buf, bytes);
  p->cap = bytes;
}
