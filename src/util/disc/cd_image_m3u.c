#include "cd_image_m3u.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDImage);

#define CD_IMAGE_M3U_NO_INDEX UINT32_C(0xFFFFFFFF)

typedef struct {
  char* filename;   /* heap, owned */
  char* title;      /* heap, owned (basename minus extension) */
} cd_image_m3u_entry_t;

typedef struct {
  cd_image_t            base;            /* MUST be first member */
  cd_image_m3u_entry_t* entries;
  u32                   entry_count;
  u32                   entry_capacity;
  cd_image_t*           current_image;   /* owned */
  u32                   current_index;
} cd_image_m3u_t;

static cd_image_m3u_t* m3u_from_base(cd_image_t* i)
{
  return (cd_image_m3u_t*)i;
}
static const cd_image_m3u_t* m3u_from_base_const(const cd_image_t* i)
{
  return (const cd_image_m3u_t*)i;
}

static bool m3u_read_sector_from_index(cd_image_t* self, void* buffer,
                                       const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_m3u_t* m = m3u_from_base(self);
  return cd_image_read_sector_from_index(m->current_image, buffer, index, lba_in_index);
}

static bool m3u_read_subchannel_q(cd_image_t* self, cd_image_subq_t* out,
                                  const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_m3u_t* m = m3u_from_base(self);
  return cd_image_read_subchannel_q(m->current_image, out, index, lba_in_index);
}

static bool m3u_has_subchannel_data(const cd_image_t* self)
{
  const cd_image_m3u_t* m = m3u_from_base_const(self);
  return m->current_image && cd_image_has_subchannel_data(m->current_image);
}

static cd_image_precache_result_t m3u_precache(cd_image_t* self, Error* error)
{
  cd_image_m3u_t* m = m3u_from_base(self);
  if (!m->current_image)
    return CD_IMAGE_PRECACHE_RESULT_UNSUPPORTED;
  return cd_image_precache(m->current_image, error);
}

static bool m3u_is_precached(const cd_image_t* self)
{
  const cd_image_m3u_t* m = m3u_from_base_const(self);
  return m->current_image && cd_image_is_precached(m->current_image);
}

static s64 m3u_get_size_on_disk(const cd_image_t* self)
{
  const cd_image_m3u_t* m = m3u_from_base_const(self);
  if (!m->current_image)
    return -1;
  return cd_image_get_size_on_disk(m->current_image);
}

static bool m3u_has_sub_images(const cd_image_t* self)
{
  (void)self;
  return true;
}

static u32 m3u_get_sub_image_count(const cd_image_t* self)
{
  return m3u_from_base_const(self)->entry_count;
}

static u32 m3u_get_current_sub_image(const cd_image_t* self)
{
  return m3u_from_base_const(self)->current_index;
}

static char* m3u_get_sub_image_title(const cd_image_t* self, u32 index)
{
  const cd_image_m3u_t* m = m3u_from_base_const(self);
  if (index >= m->entry_count || !m->entries[index].title)
    return NULL;
  return strdup(m->entries[index].title);
}

static bool m3u_switch_sub_image(cd_image_t* self, u32 index, Error* error)
{
  cd_image_m3u_t* m = m3u_from_base(self);

  if (index >= m->entry_count) {
    Error_set_string_fmt(error, "Sub-image %u out of range (count=%u).", index, m->entry_count);
    return false;
  }
  if (index == m->current_index)
    return true;

  cd_image_t* new_image = cd_image_open(m->entries[index].filename, error);
  if (!new_image) {
    ERROR_LOG("Failed to load subimage %u (%s)", index, m->entries[index].filename);
    return false;
  }

  cd_image_copy_toc(&m->base, new_image);

  cd_image_destroy(m->current_image);
  m->current_image = new_image;
  m->current_index = index;

  /* Seek to the first track (lba 0 / track 1) so the caller starts on the */
  if (!cd_image_seek_lba(&m->base, 0))
    Panic("Failed to seek to start after sub-image change.");

  return true;
}

static void m3u_destroy(cd_image_t* self)
{
  cd_image_m3u_t* m = m3u_from_base(self);
  cd_image_destroy(m->current_image);
  m->current_image = NULL;
  for (u32 k = 0; k < m->entry_count; k++) {
    free(m->entries[k].filename);
    free(m->entries[k].title);
  }
  free(m->entries);
  m->entries = NULL;
  m->entry_count = 0;
  m->entry_capacity = 0;
}

static const cd_image_vtable_t s_m3u_vtable = {
   .read_sector_from_index = m3u_read_sector_from_index,
  .read_subchannel_q      = m3u_read_subchannel_q,
  .has_subchannel_data    = m3u_has_subchannel_data,
  .precache               = m3u_precache,
  .is_precached           = m3u_is_precached,
  .get_size_on_disk       = m3u_get_size_on_disk,
  .destroy                = m3u_destroy,
  .has_sub_images         = m3u_has_sub_images,
  .get_sub_image_count    = m3u_get_sub_image_count,
  .get_current_sub_image  = m3u_get_current_sub_image,
  .get_sub_image_title    = m3u_get_sub_image_title,
  .switch_sub_image       = m3u_switch_sub_image, 
};

static bool is_ws(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static void grow_entries(cd_image_m3u_t* m)
{
  if (m->entry_count < m->entry_capacity)
    return;
  const u32 new_cap = (m->entry_capacity == 0) ? 4u : (m->entry_capacity * 2u);
  cd_image_m3u_entry_t* p = (cd_image_m3u_entry_t*)realloc(
    m->entries, new_cap * sizeof(cd_image_m3u_entry_t));
  if (!p) Panic("Memory allocation failed.");
  m->entries = p;
  m->entry_capacity = new_cap;
}

static char* strndup_to_heap(const char* data, u32 len)
{
  char* p = (char*)malloc((size_t)len + 1u);
  if (!p) Panic("Memory allocation failed.");
  if (len) memcpy(p, data, len);
  p[len] = '\0';
  return p;
}

static void push_entry(cd_image_m3u_t* m, const char* line, u32 len, const char* m3u_path)
{
  /* Normalise to native path (Linux: collapse '//' runs, strip trailing '/'). */
  small_string_t native; small_string_init(&native);
  path_to_native_view(&native, line, len);

  /* Resolve relative paths against the m3u file's directory. */
  small_string_t resolved; small_string_init(&resolved);
  if (path_is_absolute_cstr(small_string_c_str(&native))) {
  } else {
    path_build_relative_cstr(&resolved, m3u_path, small_string_c_str(&native));
  }

  const char* final_path =
    path_is_absolute_cstr(small_string_c_str(&native))
      ? small_string_c_str(&native)
      : small_string_c_str(&resolved);

  /* title = basename without extension. */
  const char* base_data = NULL; u32 base_len = 0;
  path_get_file_name_cstr(final_path, &base_data, &base_len);
  const char* title_data = NULL; u32 title_len = 0;
  path_strip_extension_view(base_data, base_len, &title_data, &title_len);

  grow_entries(m);
  cd_image_m3u_entry_t* e = &m->entries[m->entry_count++];
  e->filename = strndup_to_heap(final_path, (u32)strlen(final_path));
  e->title    = strndup_to_heap(title_data, title_len);

  DEV_LOG("Read path from m3u: '%s'", e->filename);

  small_string_destroy(&native);
  small_string_destroy(&resolved);
}

cd_image_t* cd_image_open_m3u(const char* path, Error* error)
{
  char*  buf = NULL;
  size_t buf_len = 0;
  if (!fs_read_file_to_string_path(path, &buf, &buf_len, error)) {
    return NULL;
  }
  if (buf_len == 0) {
    free(buf);
    Error_set_string(error, "Failed to read M3u file");
    return NULL;
  }

  cd_image_m3u_t* m = (cd_image_m3u_t*)calloc(1, sizeof(*m));
  if (!m) Panic("Memory allocation failed.");
  cd_image_init(&m->base, &s_m3u_vtable);
  m->base.filename = strdup(path);
  m->current_index = CD_IMAGE_M3U_NO_INDEX;

  /* Walk the buffer line by line. */
  const char* p   = buf;
  const char* end = buf + buf_len;
  while (p < end) {
    /* Find end of current line. */
    const char* line_end = p;
    while (line_end < end && *line_end != '\n')
      line_end++;

    /* Trim leading whitespace. */
    const char* s = p;
    while (s < line_end && is_ws(*s))
      s++;

    /* Skip comments and empty lines. */
    if (s == line_end || *s == '#') {
      p = (line_end < end) ? line_end + 1 : line_end;
      continue;
    }

    /* Trim trailing whitespace. */
    const char* e = line_end;
    while (e > s && is_ws(*(e - 1)))
      e--;

    if (e > s)
      push_entry(m, s, (u32)(e - s), path);

    p = (line_end < end) ? line_end + 1 : line_end;
  }
  free(buf);

  INFO_LOG("Loaded %u paths from m3u '%s'", m->entry_count, path);

  if (m->entry_count == 0) {
    Error_set_string_fmt(error, "M3u '%s' contains no playable entries.", path);
    cd_image_destroy(&m->base);
    return NULL;
  }

  if (!m3u_switch_sub_image(&m->base, 0, error)) {
    cd_image_destroy(&m->base);
    return NULL;
  }

  return &m->base;
}
