#include "path.h"

#include "assert.h"
#include "small_string.h"

#include <stdlib.h>
#include <string.h>

static bool is_separator(char ch)
{
  return ch == '/'
#if defined(_WIN32)
      || ch == '\\'
#endif
      ;
}

#if defined(_WIN32)
/* Length of the normalized root, including a final slash when present.
 * Drive-relative prefixes are protected but are not absolute paths. */
static u32 windows_root_length(const char* data, u32 len)
{
  if (len >= 2 && data[1] == ':' && ((data[0] >= 'A' && data[0] <= 'Z') ||
                                     (data[0] >= 'a' && data[0] <= 'z')))
    return len >= 3 && data[2] == '/' ? 3 : 2;
  if (len < 1 || data[0] != '/') return 0;
  if (len < 2 || data[1] != '/') return 1;
  u32 pos = 2;
  if (len >= 4 && (data[2] == '?' || data[2] == '.') && data[3] == '/') {
    pos = 4;
    if (len >= 7 && data[5] == ':' && data[6] == '/') return 7;
    if (len >= 8 && !_strnicmp(data + 4, "UNC/", 4)) pos = 8;
    else {
      while (pos < len && data[pos] != '/') pos++;
      return pos < len ? pos + 1 : pos;
    }
  }
  /* The server and share form one indivisible UNC root. */
  for (unsigned part = 0; part < 2; part++) {
    while (pos < len && data[pos] != '/') pos++;
    if (pos < len) pos++;
  }
  return pos;
}
#endif

/* Append `src` (data,len) to dst, normalising '/' runs.  Mirrors the */
static void path_append_normalised(small_string_t* dst, const char* src, u32 len)
{
  small_string_make_room_for(dst, len);
#if defined(_WIN32)
  if (!dst->length && len >= 2 && is_separator(src[0]) && is_separator(src[1])) {
    small_string_append_cstr(dst, "//"); src += 2; len -= 2;
  }
#endif

  bool last_separator = (dst->length > 0 && dst->buffer[dst->length - 1] == FS_OSPATH_SEPARATOR_CHARACTER);
  for (u32 i = 0; i < len; i++) {
    const char ch = src[i];
    if (is_separator(ch)) {
      if (last_separator) continue;
      last_separator = true;
      small_string_append_char(dst, FS_OSPATH_SEPARATOR_CHARACTER);
    } else {
      last_separator = false;
      small_string_append_char(dst, ch);
    }
  }
}

/* Returns position of last '/', or U32_MAX if none.  When include_separator,
 * the returned position is one past the separator. */
static u32 last_separator_pos(const char* data, u32 len, bool include_separator)
{
  for (u32 i = len; i > 0; i--) {
    if (is_separator(data[i - 1]))
      return include_separator ? i : (i - 1);
  }
  return (u32)-1;
}

static void trim_trailing_separators(small_string_t* s)
{
  u32 minimum = 1;
#if defined(_WIN32)
  minimum = windows_root_length(s->buffer, s->length);
#endif
  while (s->length > minimum && s->buffer[s->length - 1] == FS_OSPATH_SEPARATOR_CHARACTER) {
    s->buffer[s->length - 1] = '\0';
    s->length--;
  }
}

bool path_is_absolute_view(const char* data, u32 len)
{
#if defined(_WIN32)
  return (len >= 1 && is_separator(data[0])) ||
         (len >= 3 && data[1] == ':' && is_separator(data[2]) &&
          ((data[0] >= 'A' && data[0] <= 'Z') || (data[0] >= 'a' && data[0] <= 'z')));
#else
  return (len >= 1 && data[0] == '/');
#endif
}

bool path_is_absolute_cstr(const char* path)
{
  return path && path_is_absolute_view(path, (u32)strlen(path));
}

void path_to_native_view(small_string_t* out, const char* data, u32 len)
{
  small_string_clear(out);
  path_append_normalised(out, data, len);
  trim_trailing_separators(out);
}

void path_to_native_cstr(small_string_t* out, const char* path)
{
  path_to_native_view(out, path, (u32)strlen(path));
}

void path_build_relative_view(small_string_t* out,
                              const char* filename_data, u32 filename_len,
                              const char* new_filename_data, u32 new_filename_len)
{
  small_string_clear(out);

#if defined(_WIN32)
  if (path_is_absolute_view(new_filename_data, new_filename_len) ||
      (new_filename_len >= 2 && new_filename_data[1] == ':')) {
    path_to_native_view(out, new_filename_data, new_filename_len);
    return;
  }
#endif

  const u32 pos = last_separator_pos(filename_data, filename_len, true);
  if (pos != (u32)-1)
    small_string_append_view(out, filename_data, pos);
  small_string_append_view(out, new_filename_data, new_filename_len);
}

void path_build_relative_cstr(small_string_t* out, const char* filename, const char* new_filename)
{
  path_build_relative_view(out, filename, (u32)strlen(filename), new_filename, (u32)strlen(new_filename));
}

static void trim_join_tail(small_string_t* out)
{
#if defined(_WIN32)
  trim_trailing_separators(out);
#else
  while (out->length && out->buffer[out->length - 1] == '/') {
    out->buffer[--out->length] = 0;
  }
#endif
}

static void append_join_separator(small_string_t* out)
{
#if defined(_WIN32)
  if (!out->length || out->buffer[out->length - 1] == '/' || out->buffer[out->length - 1] == ':') return;
#endif
  small_string_append_char(out, '/');
}

void path_combine_view2(small_string_t* out,
                        const char* base_data, u32 base_len,
                        const char* next_data, u32 next_len)
{
  small_string_clear(out);
  small_string_reserve(out, base_len + next_len + 1);

  path_append_normalised(out, base_data, base_len);
  trim_join_tail(out);

  append_join_separator(out);
  path_append_normalised(out, next_data, next_len);
  trim_join_tail(out);
}

void path_combine_view3(small_string_t* out,
                        const char* base_data, u32 base_len,
                        const char* subdir_data, u32 subdir_len,
                        const char* next_data, u32 next_len)
{
  small_string_clear(out);
  small_string_reserve(out, base_len + subdir_len + next_len + 2);

  path_append_normalised(out, base_data, base_len);
  trim_join_tail(out);

  append_join_separator(out);
  path_append_normalised(out, subdir_data, subdir_len);
  trim_join_tail(out);

  append_join_separator(out);
  path_append_normalised(out, next_data, next_len);
  trim_join_tail(out);
}

void path_combine_cstr2(small_string_t* out, const char* base, const char* next)
{
  path_combine_view2(out, base, (u32)strlen(base), next, (u32)strlen(next));
}

void path_combine_cstr3(small_string_t* out, const char* base, const char* subdir, const char* next)
{
  path_combine_view3(out, base, (u32)strlen(base), subdir, (u32)strlen(subdir), next, (u32)strlen(next));
}

/* Split normalized paths, retaining the leading root element. The caller
 * frees both variable-capacity arrays. */
static bool split_native_alloc(const char* data, u32 len, u32** out_starts, u32** out_lens, u32* out_count)
{
  u32 cap = 16;
  u32* starts = (u32*)malloc(sizeof(u32) * cap);
  u32* lens   = (u32*)malloc(sizeof(u32) * cap);
  if (!starts || !lens) { free(starts); free(lens); return false; }
  u32 count = 0;

  u32 start = 0;
  u32 pos = 0;
  while (pos < len) {
    if (data[pos] != '/') {
      pos++;
      continue;
    }
    if (pos != start || pos == 0) {
      if (count == cap) {
        cap *= 2;
        u32* ns = (u32*)realloc(starts, sizeof(u32) * cap);
        u32* nl = (u32*)realloc(lens,   sizeof(u32) * cap);
        if (!ns || !nl) { free(ns ? ns : starts); free(nl ? nl : lens); return false; }
        starts = ns; lens = nl;
      }
      starts[count] = start;
      lens[count]   = pos - start;
      count++;
    }
    pos++;
    start = pos;
  }
  if (start != pos) {
    if (count == cap) {
      cap *= 2;
      u32* ns = (u32*)realloc(starts, sizeof(u32) * cap);
      u32* nl = (u32*)realloc(lens,   sizeof(u32) * cap);
      if (!ns || !nl) { free(ns ? ns : starts); free(nl ? nl : lens); return false; }
      starts = ns; lens = nl;
    }
    starts[count] = start;
    lens[count]   = pos - start;
    count++;
  }

  *out_starts = starts;
  *out_lens   = lens;
  *out_count  = count;
  return true;
}

void path_canonicalize_view(small_string_t* out, const char* data, u32 len)
{
#if defined(_WIN32)
  small_string_stack_t normalized;
  small_string_stack_init(&normalized);
  path_to_native_view(&normalized.s, data, len);
  const char* source = small_string_c_str(&normalized.s);
  const u32 length = normalized.s.length;
  const u32 root = windows_root_length(source, length);
  u32* offsets = malloc(((size_t)length + 1) * sizeof(*offsets));
  small_string_clear(out);
  if (!offsets) { small_string_destroy(&normalized.s); return; }
  small_string_append_view(out, source, root);
  u32 depth = 0, pos = root;
  while (pos < length) {
    while (pos < length && source[pos] == '/') pos++;
    const u32 start = pos;
    while (pos < length && source[pos] != '/') pos++;
    const u32 count = pos - start;
    if (!count || (count == 1 && source[start] == '.')) continue;
    if (count == 2 && source[start] == '.' && source[start + 1] == '.') {
      if (depth) {
        const u32 component = offsets[depth - 1];
        u32 begin = component;
        if (begin < out->length && out->buffer[begin] == '/') begin++;
        if (out->length - begin != 2 || memcmp(out->buffer + begin, "..", 2)) {
          out->length = offsets[--depth]; out->buffer[out->length] = 0; continue;
        }
      }
      if (root && path_is_absolute_view(source, length)) continue;
    }
    offsets[depth++] = out->length;
    if (out->length && out->buffer[out->length - 1] != '/' && out->buffer[out->length - 1] != ':')
      small_string_append_char(out, '/');
    small_string_append_view(out, source + start, count);
  }
  if (!out->length && length == 1 && source[0] == '.') small_string_append_char(out, '.');
  free(offsets); small_string_destroy(&normalized.s);
#else
  small_string_clear(out);

  u32* starts = NULL;
  u32* lens = NULL;
  u32 count = 0;
  if (!split_native_alloc(data, len, &starts, &lens, &count))
    return;

  /* Resolve "."/".." in-place over the index list by repacking. */
  u32* keep_idx = (u32*)malloc(sizeof(u32) * (count > 0 ? count : 1));
  if (!keep_idx) { free(starts); free(lens); return; }
  u32 keep_count = 0;

  for (u32 i = 0; i < count; i++) {
    const char* cd = data + starts[i];
    const u32   cl = lens[i];
    if (cl == 1 && cd[0] == '.') {
      if (count == 1)
        keep_idx[keep_count++] = i;
    } else if (cl == 2 && cd[0] == '.' && cd[1] == '.') {
      if (keep_count > 0)
        keep_count--;
      else
        keep_idx[keep_count++] = i;
    } else {
      keep_idx[keep_count++] = i;
    }
  }

  /* Re-join with '/' separators.  An empty leading element produces the
   * leading absolute slash. */
  for (u32 i = 0; i < keep_count; i++) {
    if (i > 0)
      small_string_append_char(out, FS_OSPATH_SEPARATOR_CHARACTER);
    const u32 idx = keep_idx[i];
    small_string_append_view(out, data + starts[idx], lens[idx]);
  }

  free(keep_idx);
  free(starts);
  free(lens);
#endif
}

void path_canonicalize_inplace(small_string_t* path)
{
  small_string_stack_t tmp;
  small_string_stack_init(&tmp);
  path_canonicalize_view(&tmp.s, path->buffer ? path->buffer : "", path->length);
  small_string_assign(path, &tmp.s);
  small_string_destroy(&tmp.s);
}

static void path_make_relative_normalized(small_string_t* out,
                             const char* path_data, u32 path_len,
                             const char* rel_to_data, u32 rel_to_len)
{
  small_string_clear(out);

  u32 *p_starts = NULL, *p_lens = NULL, p_count = 0;
  u32 *r_starts = NULL, *r_lens = NULL, r_count = 0;
  if (!split_native_alloc(path_data, path_len, &p_starts, &p_lens, &p_count) ||
      !split_native_alloc(rel_to_data, rel_to_len, &r_starts, &r_lens, &r_count)) {
    free(p_starts); free(p_lens); free(r_starts); free(r_lens);
    return;
  }

  /* Both must be absolute, otherwise just return path verbatim (joined). */
  if (!path_is_absolute_view(path_data, path_len) || !path_is_absolute_view(rel_to_data, rel_to_len)) {
    for (u32 i = 0; i < p_count; i++) {
      if (i > 0) small_string_append_char(out, FS_OSPATH_SEPARATOR_CHARACTER);
      small_string_append_view(out, path_data + p_starts[i], p_lens[i]);
    }
    free(p_starts); free(p_lens); free(r_starts); free(r_lens);
    return;
  }

  u32 num_same = 0;
  while (num_same < p_count && num_same < r_count) {
    if (p_lens[num_same] != r_lens[num_same] ||
#if defined(_WIN32)
        _strnicmp(path_data + p_starts[num_same], rel_to_data + r_starts[num_same], p_lens[num_same]) != 0)
#else
        memcmp(path_data + p_starts[num_same], rel_to_data + r_starts[num_same], p_lens[num_same]) != 0)
#endif
      break;
    num_same++;
  }

  if (num_same > 0) {
    const u32 num_ups = r_count - num_same;
    for (u32 i = 0; i < num_ups; i++) {
      if (out->length > 0) small_string_append_char(out, FS_OSPATH_SEPARATOR_CHARACTER);
      small_string_append_cstr(out, "..");
    }
    for (u32 i = num_same; i < p_count; i++) {
      if (out->length > 0) small_string_append_char(out, FS_OSPATH_SEPARATOR_CHARACTER);
      small_string_append_view(out, path_data + p_starts[i], p_lens[i]);
    }
  } else {
    for (u32 i = 0; i < p_count; i++) {
      if (i > 0) small_string_append_char(out, FS_OSPATH_SEPARATOR_CHARACTER);
      small_string_append_view(out, path_data + p_starts[i], p_lens[i]);
    }
  }

  free(p_starts); free(p_lens); free(r_starts); free(r_lens);
}

void path_make_relative_view(small_string_t* out, const char* data, u32 len, const char* base, u32 base_len)
{
#if defined(_WIN32)
  small_string_stack_t path, relative_to;
  small_string_stack_init(&path); small_string_stack_init(&relative_to);
  path_canonicalize_view(&path.s, data, len);
  path_canonicalize_view(&relative_to.s, base, base_len);
  const u32 root = windows_root_length(path.s.buffer, path.s.length);
  const u32 other_root = windows_root_length(relative_to.s.buffer, relative_to.s.length);
  if (root != other_root || _strnicmp(path.s.buffer, relative_to.s.buffer, root))
    small_string_assign(out, &path.s);
  else path_make_relative_normalized(out, path.s.buffer, path.s.length, relative_to.s.buffer, relative_to.s.length);
  small_string_destroy(&path.s); small_string_destroy(&relative_to.s);
#else
  path_make_relative_normalized(out, data, len, base, base_len);
#endif
}

void path_get_extension_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len)
{
  for (u32 i = path_len; i > 0; i--) {
    if (path_data[i - 1] == '.') {
      *out_data = path_data + i;
      *out_len  = path_len - i;
      return;
    }
  }
  *out_data = path_data + path_len;
  *out_len  = 0;
}

void path_get_extension_cstr(const char* path, const char** out_data, u32* out_len)
{
  path_get_extension_view(path, (u32)strlen(path), out_data, out_len);
}

void path_strip_extension_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len)
{
  for (u32 i = path_len; i > 0; i--) {
    if (path_data[i - 1] == '.') {
      *out_data = path_data;
      *out_len  = i - 1;
      return;
    }
  }
  *out_data = path_data;
  *out_len  = path_len;
}

void path_strip_extension_cstr(const char* path, const char** out_data, u32* out_len)
{
  path_strip_extension_view(path, (u32)strlen(path), out_data, out_len);
}

void path_get_file_name_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len)
{
  const u32 pos = last_separator_pos(path_data, path_len, true);
  if (pos == (u32)-1) {
    *out_data = path_data;
    *out_len  = path_len;
    return;
  }
  *out_data = path_data + pos;
  *out_len  = path_len - pos;
}

void path_get_file_name_cstr(const char* path, const char** out_data, u32* out_len)
{
  path_get_file_name_view(path, (u32)strlen(path), out_data, out_len);
}

void path_get_directory_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len)
{
  const u32 pos = last_separator_pos(path_data, path_len, false);
  if (pos == (u32)-1) {
    *out_data = path_data;
    *out_len  = 0;
    return;
  }
  *out_data = path_data;
  *out_len  = pos;
#if defined(_WIN32)
  if (pos == 2 && path_data[1] == ':') *out_len = 3;
#endif
}

void path_get_directory_cstr(const char* path, const char** out_data, u32* out_len)
{
  path_get_directory_view(path, (u32)strlen(path), out_data, out_len);
}

void path_change_extension_view(small_string_t* out,
                                const char* path_data, u32 path_len,
                                const char* new_ext_data, u32 new_ext_len)
{
  small_string_clear(out);
  u32 dot = (u32)-1;
  for (u32 i = path_len; i > 0; i--) {
    if (path_data[i - 1] == '.') { dot = i - 1; break; }
  }
  if (dot == (u32)-1) {
    small_string_append_view(out, path_data, path_len);
    if (new_ext_len > 0) {
      small_string_append_char(out, '.');
      small_string_append_view(out, new_ext_data, new_ext_len);
    }
    return;
  }
  small_string_append_view(out, path_data, dot + 1);
  small_string_append_view(out, new_ext_data, new_ext_len);
}

void path_change_extension_cstr(small_string_t* out, const char* path, const char* new_ext)
{
  path_change_extension_view(out, path, (u32)strlen(path), new_ext, (u32)strlen(new_ext));
}

void path_remove_length_limits_view(small_string_t* out, const char* data, u32 len)
{
  /* Windows filesystem calls add extended-length prefixes at the API boundary. */
  small_string_clear(out);
  small_string_append_view(out, data, len);
}
