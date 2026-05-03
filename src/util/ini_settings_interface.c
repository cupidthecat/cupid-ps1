/*
 * Replaces SimpleIni third-party header with hand-rolled INI parser.
 *
 * Grammar (per line, after stripping outer whitespace):
 *   <empty>           -> ignored
 *   ;<rest>           -> comment
 *   #<rest>           -> comment
 *   [<name>]          -> section header (name is whitespace-trimmed)
 *   <key> = <value>   -> entry. Trailing inline comment from first unquoted
 *                        '#' or ';' is stripped.  A value fully wrapped in
 *                        double quotes ("...") is taken verbatim sans quotes
 *                        (so it can contain '#'/';').
 *
 * Duplicate keys are NOT collapsed on load - they form a "string list" run
 * in the same section.  Storage keeps each section's entries sorted by key,
 * with same-key entries kept in insertion order inside the run.
 */

#include "ini_settings_interface.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

 /*
 * across threads so concurrent saves can't interleave a partial write with a
 * concurrent reader's parse.  */
static pthread_mutex_t s_ini_load_save_mutex = PTHREAD_MUTEX_INITIALIZER;

static char* dup_view(const char* data, size_t len)
{
  char* p = (char*)malloc(len + 1u);
  if (len > 0u)
    memcpy(p, data, len);
  p[len] = '\0';
  return p;
}

static char* dup_cstr(const char* s)
{
  return dup_view(s, strlen(s));
}

static void trim_view(const char* data, size_t len, const char** out_data, size_t* out_len)
{
  size_t start = 0;
  while (start < len && string_util_is_whitespace(data[start]))
    start++;
  size_t end = len;
  while (end > start && string_util_is_whitespace(data[end - 1u]))
    end--;
  *out_data = data + start;
  *out_len  = end - start;
}

/* Strips inline comment from a raw value.  A value wrapped in double quotes
 * is returned verbatim (quotes removed) so it can contain '#'/';'. */
static void strip_value_comment(const char* data, size_t len, const char** out_data, size_t* out_len)
{
  trim_view(data, len, &data, &len);
  if (len >= 2u && data[0] == '"' && data[len - 1u] == '"') {
    *out_data = data + 1u;
    *out_len  = len - 2u;
    return;
  }
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '#' || data[i] == ';') {
      len = i;
      break;
    }
  }
  trim_view(data, len, out_data, out_len);
}

static bool value_needs_quoting(const char* data, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '#' || data[i] == ';')
      return true;
  }
  return false;
}

/* Lower-bound index of the first section whose name >= needle.  When found is
 * non-NULL, *found is set true iff sections[returned].name == needle. */
static size_t section_lower_bound(const ini_settings_interface_t* self, const char* name, bool* found)
{
  size_t lo = 0, hi = self->section_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (strcmp(self->sections[mid].name, name) < 0)
      lo = mid + 1u;
    else
      hi = mid;
  }
  if (found)
    *found = (lo < self->section_count && strcmp(self->sections[lo].name, name) == 0);
  return lo;
}

static ini_section_t* find_section(ini_settings_interface_t* self, const char* name)
{
  bool found = false;
  const size_t idx = section_lower_bound(self, name, &found);
  return found ? &self->sections[idx] : NULL;
}

static const ini_section_t* find_section_const(const ini_settings_interface_t* self, const char* name)
{
  bool found = false;
  const size_t idx = section_lower_bound(self, name, &found);
  return found ? &self->sections[idx] : NULL;
}

static ini_section_t* get_or_create_section(ini_settings_interface_t* self, const char* name)
{
  bool found = false;
  const size_t idx = section_lower_bound(self, name, &found);
  if (found)
    return &self->sections[idx];

  if (self->section_count == self->section_cap) {
    const size_t new_cap = self->section_cap ? self->section_cap * 2u : 8u;
    self->sections = (ini_section_t*)realloc(self->sections, new_cap * sizeof(ini_section_t));
    self->section_cap = new_cap;
  }
  /* Shift up to keep array sorted. */
  if (idx < self->section_count) {
    memmove(&self->sections[idx + 1u], &self->sections[idx],
            (self->section_count - idx) * sizeof(ini_section_t));
  }
  self->sections[idx].name        = dup_cstr(name);
  self->sections[idx].entries     = NULL;
  self->sections[idx].entry_count = 0;
  self->sections[idx].entry_cap   = 0;
  self->section_count++;
  return &self->sections[idx];
}

/* Lower-bound index of the first entry with key >= needle. */
static size_t key_lower_bound(const ini_section_t* sec, const char* key)
{
  size_t lo = 0, hi = sec->entry_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (strcmp(sec->entries[mid].key, key) < 0)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

/* Upper-bound: first entry with key > needle.  Used to find the end of a
 * duplicate-key run for string-list semantics. */
static size_t key_upper_bound(const ini_section_t* sec, const char* key)
{
  size_t lo = 0, hi = sec->entry_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (strcmp(sec->entries[mid].key, key) <= 0)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

static void insert_kv(ini_section_t* sec, const char* key, const char* value, size_t value_len)
{
  const size_t end_idx = key_upper_bound(sec, key);

  if (sec->entry_count == sec->entry_cap) {
    const size_t new_cap = sec->entry_cap ? sec->entry_cap * 2u : 8u;
    sec->entries = (ini_kvp_t*)realloc(sec->entries, new_cap * sizeof(ini_kvp_t));
    sec->entry_cap = new_cap;
  }
  if (end_idx < sec->entry_count) {
    memmove(&sec->entries[end_idx + 1u], &sec->entries[end_idx],
            (sec->entry_count - end_idx) * sizeof(ini_kvp_t));
  }
  sec->entries[end_idx].key   = dup_cstr(key);
  sec->entries[end_idx].value = dup_view(value, value_len);
  sec->entry_count++;
}

/* Erases entries[begin..end) from sec.  Frees their key/value strings. */
static void erase_kv_range(ini_section_t* sec, size_t begin, size_t end)
{
  for (size_t i = begin; i < end; i++) {
    free(sec->entries[i].key);
    free(sec->entries[i].value);
  }
  if (end < sec->entry_count) {
    memmove(&sec->entries[begin], &sec->entries[end],
            (sec->entry_count - end) * sizeof(ini_kvp_t));
  }
  sec->entry_count -= (end - begin);
}

static void clear_section_entries(ini_section_t* sec)
{
  for (size_t i = 0; i < sec->entry_count; i++) {
    free(sec->entries[i].key);
    free(sec->entries[i].value);
  }
  sec->entry_count = 0;
}

static void free_section(ini_section_t* sec)
{
  clear_section_entries(sec);
  free(sec->entries);
  free(sec->name);
  sec->entries = NULL;
  sec->name = NULL;
  sec->entry_cap = 0;
}

static bool ini_v_is_empty(settings_interface_t* base)
{
  const ini_settings_interface_t* self = (const ini_settings_interface_t*)base;
  for (size_t i = 0; i < self->section_count; i++) {
    if (self->sections[i].entry_count > 0)
      return false;
  }
  return true;
}

static bool ini_v_lookup_value(const settings_interface_t* base, const char* section,
                               const char* key, small_string_t* out)
{
  const ini_settings_interface_t* self = (const ini_settings_interface_t*)base;
  const ini_section_t* sec = find_section_const(self, section);
  if (!sec)
    return false;

  const size_t idx = key_lower_bound(sec, key);
  if (idx >= sec->entry_count || strcmp(sec->entries[idx].key, key) != 0)
    return false;

  const char* v = sec->entries[idx].value;
  small_string_assign_view(out, v, (u32)strlen(v));
  return true;
}

static void ini_v_store_value(settings_interface_t* base, const char* section,
                              const char* key, const char* value)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = get_or_create_section(self, section);

  const size_t lo = key_lower_bound(sec, key);
  if (lo < sec->entry_count && strcmp(sec->entries[lo].key, key) == 0) {
    /* Existing entry: overwrite first, then drop any duplicates so storing a
     * single value collapses a previous string-list run. */
    if (strcmp(sec->entries[lo].value, value) == 0) {
      const size_t hi = key_upper_bound(sec, key);
      if (hi - lo == 1u)
        return; /* Already up to date and no duplicates. */
    }
    free(sec->entries[lo].value);
    sec->entries[lo].value = dup_cstr(value);

    const size_t hi = key_upper_bound(sec, key);
    if (hi - lo > 1u)
      erase_kv_range(sec, lo + 1u, hi);
  } else {
    insert_kv(sec, key, value, strlen(value));
  }
  self->dirty = true;
}

static bool ini_v_contains_value(const settings_interface_t* base, const char* section, const char* key)
{
  const ini_settings_interface_t* self = (const ini_settings_interface_t*)base;
  const ini_section_t* sec = find_section_const(self, section);
  if (!sec)
    return false;
  const size_t idx = key_lower_bound(sec, key);
  return (idx < sec->entry_count && strcmp(sec->entries[idx].key, key) == 0);
}

static void ini_v_delete_value(settings_interface_t* base, const char* section, const char* key)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = find_section(self, section);
  if (!sec)
    return;

  const size_t lo = key_lower_bound(sec, key);
  if (lo >= sec->entry_count || strcmp(sec->entries[lo].key, key) != 0)
    return;

  const size_t hi = key_upper_bound(sec, key);
  erase_kv_range(sec, lo, hi);
  self->dirty = true;
}

static void ini_v_clear_section(settings_interface_t* base, const char* section)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = find_section(self, section);
  if (sec) {
    clear_section_entries(sec);
  } else {
    /* Create an empty section so subsequent saves preserve the empty header. */
    (void)get_or_create_section(self, section);
  }
  self->dirty = true;
}

static void ini_v_remove_section(settings_interface_t* base, const char* section)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  bool found = false;
  const size_t idx = section_lower_bound(self, section, &found);
  if (!found)
    return;

  free_section(&self->sections[idx]);
  if (idx + 1u < self->section_count) {
    memmove(&self->sections[idx], &self->sections[idx + 1u],
            (self->section_count - idx - 1u) * sizeof(ini_section_t));
  }
  self->section_count--;
  self->dirty = true;
}

static void ini_v_remove_empty_sections(settings_interface_t* base)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  size_t w = 0;
  for (size_t r = 0; r < self->section_count; r++) {
    if (self->sections[r].entry_count == 0) {
      free_section(&self->sections[r]);
      self->dirty = true;
      continue;
    }
    if (w != r)
      self->sections[w] = self->sections[r];
    w++;
  }
  self->section_count = w;
}

static void ini_v_get_string_list(const settings_interface_t* base, const char* section, const char* key,
                                  char*** out_array, size_t* out_count)
{
  const ini_settings_interface_t* self = (const ini_settings_interface_t*)base;
  *out_array = NULL;
  *out_count = 0;

  const ini_section_t* sec = find_section_const(self, section);
  if (!sec)
    return;
  const size_t lo = key_lower_bound(sec, key);
  if (lo >= sec->entry_count || strcmp(sec->entries[lo].key, key) != 0)
    return;
  const size_t hi = key_upper_bound(sec, key);
  const size_t n = hi - lo;

  char** arr = (char**)malloc(n * sizeof(char*));
  for (size_t i = 0; i < n; i++)
    arr[i] = dup_cstr(sec->entries[lo + i].value);
  *out_array = arr;
  *out_count = n;
}

static void ini_v_set_string_list(settings_interface_t* base, const char* section, const char* key,
                                  const char* const* items, size_t count)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = get_or_create_section(self, section);

  /* Drop existing run for this key, then insert the new items in order. */
  const size_t lo = key_lower_bound(sec, key);
  if (lo < sec->entry_count && strcmp(sec->entries[lo].key, key) == 0) {
    const size_t hi = key_upper_bound(sec, key);
    erase_kv_range(sec, lo, hi);
  }

  for (size_t i = 0; i < count; i++)
    insert_kv(sec, key, items[i], strlen(items[i]));

  self->dirty = true;
}

static bool ini_v_remove_from_string_list(settings_interface_t* base, const char* section,
                                          const char* key, const char* item)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = find_section(self, section);
  if (!sec)
    return false;

  const size_t lo = key_lower_bound(sec, key);
  if (lo >= sec->entry_count || strcmp(sec->entries[lo].key, key) != 0)
    return false;
  const size_t hi = key_upper_bound(sec, key);

  for (size_t i = lo; i < hi; i++) {
    if (strcmp(sec->entries[i].value, item) == 0) {
      erase_kv_range(sec, i, i + 1u);
      self->dirty = true;
      return true;
    }
  }
  return false;
}

static bool ini_v_add_to_string_list(settings_interface_t* base, const char* section,
                                     const char* key, const char* item)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;

  /* Skip duplicates: scan the existing run for an entry with this value. */
  ini_section_t* sec = find_section(self, section);
  if (sec) {
    const size_t lo = key_lower_bound(sec, key);
    if (lo < sec->entry_count && strcmp(sec->entries[lo].key, key) == 0) {
      const size_t hi = key_upper_bound(sec, key);
      for (size_t i = lo; i < hi; i++) {
        if (strcmp(sec->entries[i].value, item) == 0)
          return false;
      }
    }
  }

  ini_section_t* dst = get_or_create_section(self, section);
  insert_kv(dst, key, item, strlen(item));
  self->dirty = true;
  return true;
}

static void ini_v_get_key_value_list(const settings_interface_t* base, const char* section,
                                     char*** out_keys, char*** out_values, size_t* out_count)
{
  const ini_settings_interface_t* self = (const ini_settings_interface_t*)base;
  *out_keys = NULL;
  *out_values = NULL;
  *out_count = 0;

  const ini_section_t* sec = find_section_const(self, section);
  if (!sec || sec->entry_count == 0)
    return;

  char** keys = (char**)malloc(sec->entry_count * sizeof(char*));
  char** vals = (char**)malloc(sec->entry_count * sizeof(char*));
  for (size_t i = 0; i < sec->entry_count; i++) {
    keys[i] = dup_cstr(sec->entries[i].key);
    vals[i] = dup_cstr(sec->entries[i].value);
  }
  *out_keys   = keys;
  *out_values = vals;
  *out_count  = sec->entry_count;
}

static void ini_v_set_key_value_list(settings_interface_t* base, const char* section,
                                     const char* const* keys, const char* const* values, size_t count)
{
  ini_settings_interface_t* self = (ini_settings_interface_t*)base;
  ini_section_t* sec = get_or_create_section(self, section);
  clear_section_entries(sec);

  for (size_t i = 0; i < count; i++)
    insert_kv(sec, keys[i], values[i], strlen(values[i]));

  self->dirty = true;
}

static const settings_interface_vtable_t s_vtable = {
   .is_empty                = ini_v_is_empty,
  .lookup_value            = ini_v_lookup_value,
  .store_value             = ini_v_store_value,
  .get_string_list         = ini_v_get_string_list,
  .set_string_list         = ini_v_set_string_list,
  .remove_from_string_list = ini_v_remove_from_string_list,
  .add_to_string_list      = ini_v_add_to_string_list,
  .get_key_value_list      = ini_v_get_key_value_list,
  .set_key_value_list      = ini_v_set_key_value_list,
  .contains_value          = ini_v_contains_value,
  .delete_value            = ini_v_delete_value,
  .clear_section           = ini_v_clear_section,
  .remove_section          = ini_v_remove_section,
  .remove_empty_sections   = ini_v_remove_empty_sections, 
};

bool ini_settings_interface_load_from_string(ini_settings_interface_t* self,
                                             const char* data, size_t len)
{
  ini_settings_interface_clear(self);

  ini_section_t* current = NULL;
  size_t pos = 0;

  while (pos < len) {
    /* Slice next line (no \n included). */
    size_t nl = pos;
    while (nl < len && data[nl] != '\n')
      nl++;
    const char* line_data = data + pos;
    size_t      line_len  = nl - pos;
    pos = (nl < len) ? (nl + 1u) : len;

    /* Drop trailing \r so CRLF behaves like LF. */
    if (line_len > 0u && line_data[line_len - 1u] == '\r')
      line_len--;

    trim_view(line_data, line_len, &line_data, &line_len);
    if (line_len == 0u)
      continue;
    if (line_data[0] == '#' || line_data[0] == ';')
      continue;

    /* Section header. */
    if (line_data[0] == '[') {
      const char* close = (const char*)memchr(line_data + 1u, ']', line_len - 1u);
      if (close) {
        const char* nm = line_data + 1u;
        size_t      nm_len = (size_t)(close - nm);
        trim_view(nm, nm_len, &nm, &nm_len);

        /* Build a NUL-terminated section name (get_or_create_section needs a
         * cstring for the binary search). Use a small stack-or-heap scratch. */
        char  stackbuf[256];
        char* namebuf = (nm_len + 1u <= sizeof(stackbuf)) ? stackbuf : (char*)malloc(nm_len + 1u);
        if (nm_len > 0u)
          memcpy(namebuf, nm, nm_len);
        namebuf[nm_len] = '\0';
        current = get_or_create_section(self, namebuf);
        if (namebuf != stackbuf)
          free(namebuf);
      }
      continue;
    }

    /* Key = value. */
    const char* eq = (const char*)memchr(line_data, '=', line_len);
    if (!eq)
      continue;

    const char* key_data = line_data;
    size_t      key_len  = (size_t)(eq - line_data);
    trim_view(key_data, key_len, &key_data, &key_len);

    const char* val_data = eq + 1u;
    size_t      val_len  = line_len - (size_t)((eq + 1u) - line_data);
    strip_value_comment(val_data, val_len, &val_data, &val_len);

    if (key_len == 0u)
      continue;

    /* Build a NUL-terminated key for insert_kv (needs cstring). */
    char  key_stack[256];
    char* keybuf = (key_len + 1u <= sizeof(key_stack)) ? key_stack : (char*)malloc(key_len + 1u);
    memcpy(keybuf, key_data, key_len);
    keybuf[key_len] = '\0';

    if (!current)
      current = get_or_create_section(self, "");

    insert_kv(current, keybuf, val_data, val_len);

    if (keybuf != key_stack)
      free(keybuf);
  }

  self->dirty = false;
  return true;
}

/* Appends one section's contents to out.  Section header is omitted for the */
static void save_section(small_string_t* out, const ini_section_t* sec)
{
  if (sec->name[0] != '\0') {
    small_string_append_char(out, '[');
    small_string_append_cstr(out, sec->name);
    small_string_append_view(out, "]\n", 2u);
  }

  for (size_t i = 0; i < sec->entry_count; i++) {
    const char* k = sec->entries[i].key;
    const char* v = sec->entries[i].value;
    const size_t v_len = strlen(v);

    small_string_append_cstr(out, k);
    small_string_append_view(out, " = ", 3u);
    if (value_needs_quoting(v, v_len)) {
      /* WHY: bare '#'/';' would be parsed as inline comment on next load.
       * Wrap the whole value in double quotes so strip_value_comment treats
       * it verbatim. */
      small_string_append_char(out, '"');
      small_string_append_view(out, v, (u32)v_len);
      small_string_append_char(out, '"');
    } else {
      small_string_append_view(out, v, (u32)v_len);
    }
    small_string_append_char(out, '\n');
  }
}

/* Builds full INI text into out. */
static void save_to_string(const ini_settings_interface_t* self, small_string_t* out)
{
  for (size_t i = 0; i < self->section_count; i++) {
    if (i != 0u) /* blank line between sections */
      small_string_append_char(out, '\n');
    save_section(out, &self->sections[i]);
  }
}

bool ini_settings_interface_init(ini_settings_interface_t* self, const char* filename)
{
  self->base.vtbl      = &s_vtable;
  self->filename       = filename ? dup_cstr(filename) : NULL;
  self->sections       = NULL;
  self->section_count  = 0;
  self->section_cap    = 0;
  self->dirty          = false;
  return true;
}

void ini_settings_interface_destroy(ini_settings_interface_t* self)
{
  ini_settings_interface_clear(self);
  free(self->sections);
  free(self->filename);
  self->sections = NULL;
  self->section_cap = 0;
  self->filename = NULL;
  self->base.vtbl = NULL;
}

void ini_settings_interface_set_filename(ini_settings_interface_t* self, const char* filename)
{
  const bool same = (self->filename && filename && strcmp(self->filename, filename) == 0) ||
                    (!self->filename && !filename);
  if (!same)
    self->dirty = true;

  free(self->filename);
  self->filename = filename ? dup_cstr(filename) : NULL;
}

const char* ini_settings_interface_get_filename(const ini_settings_interface_t* self)
{
  return self->filename ? self->filename : "";
}

bool ini_settings_interface_is_dirty(const ini_settings_interface_t* self)
{
  return self->dirty;
}

void ini_settings_interface_clear(ini_settings_interface_t* self)
{
  for (size_t i = 0; i < self->section_count; i++)
    free_section(&self->sections[i]);
  self->section_count = 0;
}

bool ini_settings_interface_load(ini_settings_interface_t* self, Error* error)
{
  if (!self->filename || self->filename[0] == '\0') {
    Error_set_string(error, "Filename is not set.");
    return false;
  }

  pthread_mutex_lock(&s_ini_load_save_mutex);

  char*  data = NULL;
  size_t len  = 0;
  if (!fs_read_file_to_string_path(self->filename, &data, &len, error)) {
    pthread_mutex_unlock(&s_ini_load_save_mutex);
    return false;
  }

  ini_settings_interface_load_from_string(self, data, len);
  free(data);
  self->dirty = false;

  pthread_mutex_unlock(&s_ini_load_save_mutex);
  return true;
}

bool ini_settings_interface_save(ini_settings_interface_t* self, Error* error)
{
  if (!self->filename || self->filename[0] == '\0') {
    Error_set_string(error, "Filename is not set.");
    return false;
  }

  pthread_mutex_lock(&s_ini_load_save_mutex);

  small_string_t out;
  small_string_init(&out);
  save_to_string(self, &out);

  bool ok = fs_write_string_to_file_view(self->filename, out.buffer ? out.buffer : "",
                                         out.length, error);

  small_string_destroy(&out);

  if (ok)
    self->dirty = false;

  pthread_mutex_unlock(&s_ini_load_save_mutex);
  return ok;
}
