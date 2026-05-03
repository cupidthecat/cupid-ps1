#include "small_string.h"

#include "assert.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static u32 u32_max(u32 a, u32 b) { return a > b ? a : b; }
static u32 u32_min(u32 a, u32 b) { return a < b ? a : b; }
static s32 s32_max(s32 a, s32 b) { return a > b ? a : b; }

static u32 clamp_offset(s32 offset, u32 len)
{
  if (offset < 0)
    return (u32)s32_max(0, (s32)len + offset);
  return u32_min((u32)offset, len);
}

static u32 clamp_count(s32 count, u32 len, u32 real_offset)
{
  if (count < 0)
    return u32_min(len - real_offset, (u32)s32_max(0, (s32)len + count));
  return u32_min(len - real_offset, (u32)count);
}

void small_string_init(small_string_t* s)
{
  s->buffer      = NULL;
  s->length      = 0;
  s->buffer_size = 0;
  s->on_heap     = false;
}

void small_string_init_stack(small_string_t* s, char* buf, u32 buf_size)
{
  s->buffer      = buf;
  s->length      = 0;
  s->buffer_size = buf_size;
  s->on_heap     = false;
  if (buf_size > 0) buf[0] = '\0';
}

void small_string_destroy(small_string_t* s)
{
  if (s->on_heap) {
    free(s->buffer);
    s->buffer      = NULL;
    s->buffer_size = 0;
    s->on_heap     = false;
  }
  s->length = 0;
}

void small_string_reserve(small_string_t* s, u32 new_reserve)
{
  const u32 real_reserve = new_reserve + 1;
  if (s->buffer_size >= real_reserve) return;

  if (s->on_heap) {
    char* p = (char*)realloc(s->buffer, real_reserve);
    if (!p) Panic("Memory allocation failed.");
    s->buffer = p;
  } else {
    char* p = (char*)malloc(real_reserve);
    if (!p) Panic("Memory allocation failed.");
    if (s->length > 0) memcpy(p, s->buffer, s->length);
    p[s->length] = '\0';
    s->buffer    = p;
    s->on_heap   = true;
  }
  s->buffer_size = real_reserve;
}

void small_string_make_room_for(small_string_t* s, u32 extra)
{
  const u32 required = s->length + extra;
  if (s->buffer_size > required) return;
  small_string_reserve(s, u32_max(required, s->buffer_size * 2));
}

void small_string_shrink_to_fit(small_string_t* s)
{
  const u32 want = s->length + 1;
  if (!s->on_heap || want == s->buffer_size) return;
  if (s->length == 0) {
    free(s->buffer);
    s->buffer      = NULL;
    s->buffer_size = 0;
    s->on_heap     = false;
    return;
  }
  char* p = (char*)realloc(s->buffer, want);
  if (!p) Panic("Memory allocation failed.");
  s->buffer      = p;
  s->buffer_size = want;
}

void small_string_resize(small_string_t* s, u32 new_size, char fill, bool shrink_if_smaller)
{
  if (new_size > s->length) {
    small_string_reserve(s, new_size);
    memset(s->buffer + s->length, (unsigned char)fill, new_size - s->length);
    s->length = new_size;
    s->buffer[s->length] = '\0';
  } else {
    s->buffer[new_size] = '\0';
    s->length = new_size;
    if (shrink_if_smaller) small_string_shrink_to_fit(s);
  }
}

void small_string_set_size(small_string_t* s, u32 new_size, bool shrink_if_smaller)
{
  DebugAssert(new_size <= s->buffer_size);
  s->length = new_size;
  s->buffer[new_size] = '\0';
  if (shrink_if_smaller) small_string_shrink_to_fit(s);
}

void small_string_update_size(small_string_t* s)
{
  s->length = (u32)strlen(s->buffer);
}

void small_string_clear(small_string_t* s)
{
  if (s->buffer_size == 0) return;
  s->buffer[0] = '\0';
  s->length    = 0;
}

void small_string_assign_view(small_string_t* s, const char* data, u32 len)
{
  small_string_clear(s);
  if (len > 0)
    small_string_append_view(s, data, len);
}

void small_string_assign_cstr(small_string_t* s, const char* str)
{
  if (!str) { small_string_clear(s); return; }
  small_string_assign_view(s, str, (u32)strlen(str));
}

void small_string_assign(small_string_t* s, const small_string_t* src)
{
  small_string_assign_view(s, src->buffer, src->length);
}

void small_string_move_assign(small_string_t* s, small_string_t* src)
{
  if (src->on_heap) {
    if (s->on_heap) free(s->buffer);
    s->buffer      = src->buffer;
    s->length      = src->length;
    s->buffer_size = src->buffer_size;
    s->on_heap     = true;
    src->buffer    = NULL;
    src->length    = 0;
    src->buffer_size = 0;
    src->on_heap   = false;
  } else {
    small_string_assign_view(s, src->buffer, src->length);
  }
}

void small_string_append_view(small_string_t* s, const char* data, u32 len)
{
  if (len == 0) return;
  DebugAssert(data != s->buffer);
  small_string_make_room_for(s, len);
  DebugAssert((len + s->length) < s->buffer_size);
  memcpy(s->buffer + s->length, data, len);
  s->length += len;
  s->buffer[s->length] = '\0';
}

void small_string_append_cstr(small_string_t* s, const char* str)
{
  if (!str) return;
  small_string_append_view(s, str, (u32)strlen(str));
}

void small_string_append_char(small_string_t* s, char c)
{
  small_string_append_view(s, &c, 1);
}

void small_string_append(small_string_t* s, const small_string_t* other)
{
  DebugAssert(s != other);
  small_string_append_view(s, other->buffer, other->length);
}

void small_string_append_hex(small_string_t* s, const void* data, size_t len, bool comma_separate)
{
  if (len == 0) return;
  const u8* bytes = (const u8*)data;
  #define HX(c_) ((char)(((c_) >= 0xA) ? (((c_) - 0xA) + 'a') : ((c_) + '0')))
  if (!comma_separate) {
    small_string_make_room_for(s, (u32)(len * 2));
    for (size_t i = 0; i < len; i++) {
      s->buffer[s->length++] = HX((u8)(bytes[i] >> 4));
      s->buffer[s->length++] = HX((u8)(bytes[i] & 0xF));
    }
  } else {
    small_string_make_room_for(s, 4 + (u32)(len - 1) * 6);
    s->buffer[s->length++] = '0';
    s->buffer[s->length++] = 'x';
    s->buffer[s->length++] = HX((u8)(bytes[0] >> 4));
    s->buffer[s->length++] = HX((u8)(bytes[0] & 0xF));
    for (size_t i = 1; i < len; i++) {
      s->buffer[s->length++] = ',';
      s->buffer[s->length++] = ' ';
      s->buffer[s->length++] = '0';
      s->buffer[s->length++] = 'x';
      s->buffer[s->length++] = HX((u8)(bytes[i] >> 4));
      s->buffer[s->length++] = HX((u8)(bytes[i] & 0xF));
    }
  }
  #undef HX
  s->buffer[s->length] = '\0';
}

void small_string_append_vsprintf(small_string_t* s, const char* fmt, va_list ap)
{
  char stack_buf[1024];
  char* heap_buf = NULL;
  char* buf      = stack_buf;
  u32   buf_sz   = (u32)sizeof(stack_buf);
  u32   written;

  for (;;) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    const int ret = vsnprintf(buf, buf_sz, fmt, ap_copy);
    va_end(ap_copy);
    if (ret < 0 || (u32)ret >= buf_sz - 1) {
      buf_sz *= 2;
      char* p = (char*)realloc(heap_buf, buf_sz);
      if (!p) Panic("Memory allocation failed.");
      heap_buf = p;
      buf      = p;
      continue;
    }
    written = (u32)ret;
    break;
  }

  small_string_append_view(s, buf, written);
  free(heap_buf);
}

void small_string_append_sprintf(small_string_t* s, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  small_string_append_vsprintf(s, fmt, ap);
  va_end(ap);
}

void small_string_prepend_view(small_string_t* s, const char* data, u32 len)
{
  if (len == 0) return;
  DebugAssert(data != s->buffer);
  small_string_make_room_for(s, len);
  DebugAssert((len + s->length) < s->buffer_size);
  memmove(s->buffer + len, s->buffer, s->length);
  memcpy(s->buffer, data, len);
  s->length += len;
  s->buffer[s->length] = '\0';
}

void small_string_prepend_cstr(small_string_t* s, const char* str)
{
  if (!str) return;
  small_string_prepend_view(s, str, (u32)strlen(str));
}

void small_string_prepend_char(small_string_t* s, char c)
{
  small_string_prepend_view(s, &c, 1);
}

void small_string_prepend(small_string_t* s, const small_string_t* other)
{
  DebugAssert(s != other);
  small_string_prepend_view(s, other->buffer, other->length);
}

void small_string_prepend_vsprintf(small_string_t* s, const char* fmt, va_list ap)
{
  char stack_buf[1024];
  char* heap_buf = NULL;
  char* buf      = stack_buf;
  u32   buf_sz   = (u32)sizeof(stack_buf);
  u32   written;

  for (;;) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    const int ret = vsnprintf(buf, buf_sz, fmt, ap_copy);
    va_end(ap_copy);
    if (ret < 0 || (u32)ret >= buf_sz - 1) {
      buf_sz *= 2;
      char* p = (char*)realloc(heap_buf, buf_sz);
      if (!p) Panic("Memory allocation failed.");
      heap_buf = p;
      buf      = p;
      continue;
    }
    written = (u32)ret;
    break;
  }

  small_string_prepend_view(s, buf, written);
  free(heap_buf);
}

void small_string_prepend_sprintf(small_string_t* s, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  small_string_prepend_vsprintf(s, fmt, ap);
  va_end(ap);
}

void small_string_insert_view(small_string_t* s, s32 offset, const char* data, u32 len)
{
  if (len == 0) return;
  small_string_make_room_for(s, len);
  const u32 real_off = clamp_offset(offset, s->length);
  DebugAssert(real_off <= s->length);
  const u32 after = s->length - real_off;
  if (after > 0)
    memmove(s->buffer + real_off + len, s->buffer + real_off, after);
  memcpy(s->buffer + real_off, data, len);
  s->length += len;
  s->buffer[s->length] = '\0';
}

void small_string_insert_cstr(small_string_t* s, s32 offset, const char* str)
{
  if (!str) return;
  small_string_insert_view(s, offset, str, (u32)strlen(str));
}

void small_string_vsprintf(small_string_t* s, const char* fmt, va_list ap)
{
  small_string_clear(s);
  small_string_append_vsprintf(s, fmt, ap);
}

void small_string_sprintf(small_string_t* s, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  small_string_vsprintf(s, fmt, ap);
  va_end(ap);
}

bool small_string_equals_cstr(const small_string_t* s, const char* str)
{
  if (s->length == 0) return strlen(str) == 0;
  return strcmp(s->buffer, str) == 0;
}

bool small_string_equals(const small_string_t* s, const small_string_t* o)
{
  return s->length == o->length && (s->length == 0 || memcmp(s->buffer, o->buffer, s->length) == 0);
}

bool small_string_equals_view(const small_string_t* s, const char* data, u32 len)
{
  return s->length == len && (len == 0 || memcmp(s->buffer, data, len) == 0);
}

bool small_string_iequals_cstr(const small_string_t* s, const char* str)
{
  if (s->length == 0) return strlen(str) == 0;
  return strcasecmp(s->buffer, str) == 0;
}

bool small_string_iequals(const small_string_t* s, const small_string_t* o)
{
  return s->length == o->length && (s->length == 0 || strncasecmp(s->buffer, o->buffer, s->length) == 0);
}

bool small_string_iequals_view(const small_string_t* s, const char* data, u32 len)
{
  return s->length == len && (len == 0 || strncasecmp(s->buffer, data, len) == 0);
}

int small_string_compare_view(const small_string_t* s, const char* data, u32 len)
{
  if (s->length == 0) return (len == 0) ? 0 : -1;
  if (len == 0)        return 1;
  const int r = strncmp(s->buffer, data, u32_min(s->length, len));
  if (s->length == len || r != 0) return r;
  return s->length > len ? 1 : -1;
}

int small_string_compare_cstr(const small_string_t* s, const char* str)
{
  return small_string_compare_view(s, str, (u32)strlen(str));
}

int small_string_compare(const small_string_t* s, const small_string_t* o)
{
  return small_string_compare_view(s, o->buffer, o->length);
}

int small_string_icompare_view(const small_string_t* s, const char* data, u32 len)
{
  if (s->length == 0) return (len == 0) ? 0 : -1;
  if (len == 0)        return 1;
  const int r = strncasecmp(s->buffer, data, u32_min(s->length, len));
  if (s->length == len || r != 0) return r;
  return s->length > len ? 1 : -1;
}

int small_string_icompare_cstr(const small_string_t* s, const char* str)
{
  return small_string_icompare_view(s, str, (u32)strlen(str));
}

int small_string_icompare(const small_string_t* s, const small_string_t* o)
{
  return small_string_icompare_view(s, o->buffer, o->length);
}

bool small_string_starts_with_view(const small_string_t* s, const char* data, u32 len, bool case_sensitive)
{
  if (len > s->length) return false;
  return case_sensitive ? (memcmp(s->buffer, data, len) == 0)
                        : (strncasecmp(s->buffer, data, len) == 0);
}

bool small_string_starts_with_cstr(const small_string_t* s, const char* str, bool case_sensitive)
{
  return small_string_starts_with_view(s, str, (u32)strlen(str), case_sensitive);
}

bool small_string_ends_with_view(const small_string_t* s, const char* data, u32 len, bool case_sensitive)
{
  if (len > s->length) return false;
  const u32 off = s->length - len;
  return case_sensitive ? (memcmp(s->buffer + off, data, len) == 0)
                        : (strncasecmp(s->buffer + off, data, len) == 0);
}

bool small_string_ends_with_cstr(const small_string_t* s, const char* str, bool case_sensitive)
{
  return small_string_ends_with_view(s, str, (u32)strlen(str), case_sensitive);
}

s32 small_string_find_char(const small_string_t* s, char c, u32 offset)
{
  if (s->length == 0) return SMALL_STRING_NPOS;
  DebugAssert(offset <= s->length);
  const char* at = strchr(s->buffer + offset, c);
  return at ? (s32)(at - s->buffer) : SMALL_STRING_NPOS;
}

s32 small_string_rfind_char(const small_string_t* s, char c, u32 offset)
{
  if (s->length == 0) return SMALL_STRING_NPOS;
  DebugAssert(offset <= s->length);
  const char* at = strrchr(s->buffer + offset, c);
  return at ? (s32)(at - s->buffer) : SMALL_STRING_NPOS;
}

s32 small_string_find_cstr(const small_string_t* s, const char* str, u32 offset)
{
  if (s->length == 0) return SMALL_STRING_NPOS;
  DebugAssert(offset <= s->length);
  const char* at = strstr(s->buffer + offset, str);
  return at ? (s32)(at - s->buffer) : SMALL_STRING_NPOS;
}

u32 small_string_count_char(const small_string_t* s, char c)
{
  u32 n = 0;
  for (u32 i = 0; i < s->length; i++) n += (s->buffer[i] == c);
  return n;
}

u32 small_string_replace(small_string_t* s, const char* search, const char* replacement)
{
  const u32 search_len = (u32)strlen(search);
  if (search_len == 0) return 0;
  const u32 repl_len = (u32)strlen(replacement);

  s32 offset = 0;
  u32 count  = 0;
  for (;;) {
    offset = small_string_find_cstr(s, search, (u32)offset);
    if (offset < 0) break;
    const u32 after = s->length - (u32)offset;
    DebugAssert(after >= search_len);

    const u32 new_len = s->length - search_len + repl_len;
    small_string_reserve(s, new_len);
    s->length = new_len;

    if (after > search_len) {
      memmove(&s->buffer[(u32)offset + repl_len],
              &s->buffer[(u32)offset + search_len],
              after - search_len);
      memcpy(&s->buffer[(u32)offset], replacement, repl_len);
      s->buffer[s->length] = '\0';
    } else {
      memcpy(&s->buffer[(u32)offset], replacement, repl_len);
      s->buffer[(u32)offset + repl_len] = '\0';
    }

    offset += (s32)repl_len;
    count++;
  }
  return count;
}

void small_string_erase(small_string_t* s, s32 offset, s32 count)
{
  const u32 real_off = clamp_offset(offset, s->length);
  const u32 real_cnt = clamp_count(count, s->length, real_off);

  if (real_off == 0 && real_cnt == s->length) {
    small_string_clear(s);
    return;
  }
  if ((real_off + real_cnt) == s->length) {
    s->length -= real_cnt;
    s->buffer[s->length] = '\0';
    return;
  }
  const u32 after = s->length - real_off - real_cnt;
  DebugAssert(after > 0);
  memmove(s->buffer + real_off, s->buffer + real_off + real_cnt, after);
  s->length -= real_cnt;
  s->buffer[s->length] = '\0';
}

void small_string_substr(const small_string_t* s, s32 offset, s32 count,
                         const char** out_data, u32* out_len)
{
  const u32 real_off = clamp_offset(offset, s->length);
  const u32 real_cnt = clamp_count(count, s->length, real_off);
  *out_data = (real_cnt > 0) ? (s->buffer + real_off) : NULL;
  *out_len  = real_cnt;
}

void small_string_to_lower(small_string_t* s)
{
  for (u32 i = 0; i < s->length; i++)
    s->buffer[i] = (char)tolower((unsigned char)s->buffer[i]);
}

void small_string_to_upper(small_string_t* s)
{
  for (u32 i = 0; i < s->length; i++)
    s->buffer[i] = (char)toupper((unsigned char)s->buffer[i]);
}

void tiny_string_make_sprintf(tiny_string_t* out, const char* fmt, ...)
{
  tiny_string_init(out);
  va_list ap; va_start(ap, fmt);
  small_string_vsprintf(&out->s, fmt, ap);
  va_end(ap);
}

void small_string_stack_make_sprintf(small_string_stack_t* out, const char* fmt, ...)
{
  small_string_stack_init(out);
  va_list ap; va_start(ap, fmt);
  small_string_vsprintf(&out->s, fmt, ap);
  va_end(ap);
}

void large_string_make_sprintf(large_string_t* out, const char* fmt, ...)
{
  large_string_init(out);
  va_list ap; va_start(ap, fmt);
  small_string_vsprintf(&out->s, fmt, ap);
  va_end(ap);
}
